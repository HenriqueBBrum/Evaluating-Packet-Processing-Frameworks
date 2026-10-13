#include <arpa/inet.h>
#include <errno.h>
#include <linux/if_packet.h>
#include <net/ethernet.h>
#include <net/if.h>
#include <poll.h>
#include <pthread.h>
#include <signal.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <getopt.h>
#include <sys/ioctl.h>
#include <sys/mman.h>
#include <sys/socket.h>
#include <sys/types.h>
#include <unistd.h>
#include <time.h>
#include <sched.h>

#define MAX_CORES 48

typedef struct ThreadCtx_ {
    int thread_id;
    int ifindex;
    uint32_t block_size;
    uint32_t block_nr;
    uint32_t frame_size;
    uint32_t fanout_id;
    struct tpacket_stats *kstats;
} ThreadCtx;

typedef struct {
    uint64_t packets;
    uint64_t bytes;
    uint64_t drops;
} ThreadStats;

static volatile sig_atomic_t exit_indicator = 0;
static void handle_signal(int sig){
    exit_indicator = 1;
    fprintf(stdout, "Signal received, stopping all threads...\n");
}

void print_usage(const char *prog_name){
    printf("Usage: %s [OPTIONS]\n", prog_name);
    printf("\nAPP_OPTIONS:\n");
    printf("  -h, --help              Display this help message\n");
    printf("  -t, --num_threads       Number of worker threads (> 0 and <= 48)\n");
    printf("  -s, --start_thread       Starting thread index (0-based)\n");
    printf("  -i, --interface         Network interface name (e.g., eth0)\n");
    printf("  -b, --block_size        Block size in bytes\n");
    printf("  -n, --block_nr          Number of blocks in RX ring (> 0)\n");
    printf("  -d, --duration          Duration in seconds (0 for infinite)\n");
    printf("\nExamples:\n");
    printf("  %s -t 4 -i eth0 -b 65536 -n 128 --duration 10\n", prog_name);
}

int parse_app_args(int argc, char **argv, uint8_t *num_threads, uint8_t *start_thread, char **interface,
                   uint32_t *block_size, uint32_t *block_nr, uint32_t *duration){
    int opt;
    struct option long_options[] = {
        {"help", no_argument, 0, 'h'},
        {"num_threads", required_argument, 0, 't'},
        {"start_thread", required_argument, 0, 's'},
        {"interface", required_argument, 0, 'i'},
        {"block_size", required_argument, 0, 'b'},
        {"block_nr", required_argument, 0, 'n'},
        {"duration", required_argument, 0, 'd'},
        {0, 0, 0, 0}
    };

    int option_index = 0;
    while ((opt = getopt_long(argc, argv, "ht:s:i:b:n:d:", long_options, &option_index)) != -1) {
        switch (opt) {
            case 'h':
                print_usage(argv[0]);
                return 1;
            case 't': 
                *num_threads = (uint8_t)strtoul(optarg, NULL, 10);
                break;
            case 'i':
                *interface = optarg;
                break;
            case 's':
                *start_thread = (uint8_t)strtoul(optarg, NULL, 10);
                break;
            case 'b':
                *block_size = (uint32_t)strtoul(optarg, NULL, 10);
                break;
            case 'n':
                *block_nr = (uint32_t)strtoul(optarg, NULL, 10);
                break;
            case 'd':
                *duration = (uint32_t)strtoul(optarg, NULL, 10);
                break;
            case '?':
                fprintf(stderr, "Unknown option\n");
                print_usage(argv[0]);
                return -1;
            default:
                break;
        }
    }

    return optind;
}

int validate_args(uint8_t num_threads, uint8_t start_thread, const char *interface, 
    uint32_t block_size, uint32_t block_nr, uint32_t frame_size, uint32_t duration) {
    if (num_threads == 0 || num_threads > MAX_CORES) {
        fprintf(stderr, "num_threads must be > 0 and <= %d\n", MAX_CORES);
        return 0;
    }

    if (start_thread + num_threads > MAX_CORES) {
        fprintf(stderr, "start_thread + num_threads must be <= %d\n", MAX_CORES);
        return 0;
    }

    if (interface == NULL || strlen(interface) == 0) {
        fprintf(stderr, "interface must be specified\n");
        return 0;
    }

    if (block_size == 0 || block_nr == 0) {
        fprintf(stderr, "block_size and block_nr must be > 0\n");
        return 0;
    }

    if (block_size % getpagesize() != 0) {
        fprintf(stderr, "block_size must be page-aligned (multiple of %d)\n", getpagesize());
        return 0;
    }

    if (block_size % frame_size != 0) {
        fprintf(stderr, "block_size must be a multiple of frame_size (%u)\n", frame_size);
        return 0;
    }

    if (duration == 0 || duration > 360){
        fprintf(stderr, "duration must be > 0 and <= 360\n");
        return 0;
    }

    return 1;
}

static int setup_socket(ThreadCtx *ctx, struct tpacket_req3 *req){
    int version = TPACKET_V3;

    int fd = socket(AF_PACKET, SOCK_RAW, htons(ETH_P_ALL));
    if (fd < 0) {
        fprintf(stderr, "thread %d: socket failed: %s\n", ctx->thread_id, strerror(errno));
        return -1;
    }

    if (setsockopt(fd, SOL_PACKET, PACKET_VERSION, &version, sizeof(version)) < 0) {
        fprintf(stderr, "thread %d: PACKET_VERSION failed: %s\n", ctx->thread_id, strerror(errno));
        close(fd);
        return -1;
    }

    memset(req, 0, sizeof(*req));
    req->tp_block_size = ctx->block_size;
    req->tp_block_nr = ctx->block_nr;
    req->tp_frame_size = ctx->frame_size;
    req->tp_frame_nr = (ctx->block_size * ctx->block_nr) / ctx->frame_size;
    req->tp_retire_blk_tov = 16; // Time-Out to Retire Block

    if (setsockopt(fd, SOL_PACKET, PACKET_RX_RING, req, sizeof(*req)) < 0) {
        fprintf(stderr, "thread %d: PACKET_RX_RING failed: %s\n", ctx->thread_id, strerror(errno));
        close(fd);
        return -1;
    }

    struct sockaddr_ll bind_addr;
    memset(&bind_addr, 0, sizeof(bind_addr));
    bind_addr.sll_family = AF_PACKET;
    bind_addr.sll_protocol = htons(ETH_P_ALL);
    bind_addr.sll_ifindex = ctx->ifindex;

    if (bind(fd, (struct sockaddr *)&bind_addr, sizeof(bind_addr)) < 0) {
        fprintf(stderr, "thread %d: bind failed: %s\n", ctx->thread_id, strerror(errno));
        close(fd);
        return -1;
    }

    struct packet_mreq mreq;
    memset(&mreq, 0, sizeof(mreq));
    mreq.mr_ifindex = ctx->ifindex;
    mreq.mr_type = PACKET_MR_PROMISC;
    if (setsockopt(fd, SOL_PACKET, PACKET_ADD_MEMBERSHIP, &mreq, sizeof(mreq)) < 0) {
        fprintf(stderr, "thread %d: PACKET_MR_PROMISC failed: %s\n", ctx->thread_id, strerror(errno));
        close(fd);
        return -1;
    }

    int fanout_arg = (int)((ctx->fanout_id & 0xFFFFU) | ((uint32_t)PACKET_FANOUT_CPU << 16));
    if (setsockopt(fd, SOL_PACKET, PACKET_FANOUT, &fanout_arg, sizeof(fanout_arg)) < 0) {
        fprintf(stderr, "thread %d: PACKET_FANOUT failed: %s\n", ctx->thread_id, strerror(errno));
        close(fd);
        return -1;
    }


    int busy_poll_usec = 50;
    if (setsockopt(fd, SOL_SOCKET, SO_BUSY_POLL, &busy_poll_usec, sizeof(busy_poll_usec)) < 0) {
        fprintf(stderr, "thread %d: SO_BUSY_POLL not enabled (%s)\n", ctx->thread_id, strerror(errno));
    }

#ifdef SO_BUSY_POLL_BUDGET
    {
        int busy_poll_budget = 64;
        if (setsockopt(fd, SOL_SOCKET, SO_BUSY_POLL_BUDGET, &busy_poll_budget, sizeof(busy_poll_budget)) < 0) {
            fprintf(stderr, "thread %d: SO_BUSY_POLL_BUDGET not enabled (%s)\n", ctx->thread_id, strerror(errno));
        }
    }
#endif

#ifdef SO_PREFER_BUSY_POLL
    {
        int prefer_busy_poll = 1;
        if (setsockopt(fd, SOL_SOCKET, SO_PREFER_BUSY_POLL, &prefer_busy_poll, sizeof(prefer_busy_poll)) < 0) {
            fprintf(stderr, "thread %d: SO_PREFER_BUSY_POLL not enabled (%s)\n", ctx->thread_id, strerror(errno));
        }
    }
#endif

    return fd;
}


static void *af_packet_thread(void *arg){
    ThreadCtx *ctx = (ThreadCtx *)arg;

    uint32_t block_idx = 0;

    struct tpacket_req3 req;
    int fd = setup_socket(ctx, &req);
    if (fd < 0) 
        return NULL;

    size_t ring_len = (size_t)req.tp_block_size * req.tp_block_nr;
    uint8_t *ring = mmap(NULL, ring_len, PROT_READ | PROT_WRITE, MAP_SHARED | MAP_LOCKED, fd, 0);
    if (ring == MAP_FAILED) {
        fprintf(stderr, "thread %d: mmap failed: %s\n", ctx->thread_id, strerror(errno));
        close(fd);
        return NULL;
    }

    uint64_t received_packets = 0;
    uint64_t received_bytes = 0;

    struct pollfd pfd;
    memset(&pfd, 0, sizeof(pfd));
    pfd.fd = fd;
    pfd.events = POLLIN | POLLERR;
    pfd.revents = 0;
   
    while (!exit_indicator) {
        struct tpacket_block_desc *bd = (struct tpacket_block_desc *)(ring + ((size_t)block_idx * req.tp_block_size));
        if ((bd->hdr.bh1.block_status & TP_STATUS_USER) == 0) {
            poll(&pfd, 1, -1);
            continue;
        }   

        int num_pkts = bd->hdr.bh1.num_pkts;
        unsigned long bytes = 0;
        struct tpacket3_hdr *ppd = (struct tpacket3_hdr *) ((uint8_t *) bd + bd->hdr.bh1.offset_to_first_pkt);
        for (int i = 0; i < num_pkts; ++i){
            bytes += ppd->tp_snaplen;
            ppd = (struct tpacket3_hdr *) ((uint8_t *) ppd + ppd->tp_next_offset);
        }

        received_packets += num_pkts;
        received_bytes += bytes;

        bd->hdr.bh1.block_status = TP_STATUS_KERNEL;

        block_idx = (block_idx + 1) % req.tp_block_nr;
    }

    fprintf(stdout, "thread %d: received packets=%llu received bytes=%llu\n", ctx->thread_id,
            (unsigned long long)received_packets,
            (unsigned long long)received_bytes);

    struct tpacket_stats kstats;
    socklen_t len = sizeof(kstats);
    if (getsockopt(fd, SOL_PACKET, PACKET_STATISTICS, (void *)&kstats, &len) > -1)
        fprintf(stdout, "thread %d: kernel packets=%llu kernel dropped=%llu\n",ctx->thread_id,
                (unsigned long long)kstats.tp_packets,
                (unsigned long long)kstats.tp_drops);
                

    ((ThreadCtx *)arg)->kstats->tp_packets = kstats.tp_packets;
    ((ThreadCtx *)arg)->kstats->tp_drops = kstats.tp_drops;
    munmap(ring, ring_len);
    close(fd);

    pthread_exit(NULL);
}

int main(int argc, char *argv[]){ 
    signal(SIGINT, handle_signal);
    signal(SIGTERM, handle_signal);

    uint8_t num_threads = 0, start_thread = 0;
    char *ifname = NULL;
    uint32_t frame_size = 2048;
    uint32_t block_size = 0, block_nr = 0;
    uint32_t duration = 0;

    parse_app_args(argc, argv, &num_threads, &start_thread, &ifname, &block_size, &block_nr, &duration);
    if (!validate_args(num_threads, start_thread, ifname, block_size, block_nr, frame_size, duration)) {
        print_usage(argv[0]);
        return EXIT_FAILURE;
    }

    int fd = socket(AF_PACKET, SOCK_RAW, htons(ETH_P_ALL));
    if (fd < 0) {
        fprintf(stderr, "Failed to create socket\n");
        return EXIT_FAILURE;
    }

    int ifindex = if_nametoindex(ifname);
    close(fd);
    if (ifindex < 0) {
        fprintf(stderr, "Failed to get interface index for %s\n", ifname);
        return EXIT_FAILURE;
    }

    pthread_t threads[num_threads];
    ThreadCtx ctxs[num_threads];

    fprintf(stdout, "Starting packet capture on interface %s with %u threads, where each thread will "
        "use %u blocks of %u size totalling %u bytes for a duration of %u seconds\n", ifname, num_threads, block_nr, block_size,  block_size * block_nr * num_threads, duration);

    uint32_t fanout_id = (uint32_t)getpid() & 0xFFFFU;
    for (uint8_t i = 0; i < num_threads; i++) {
        ctxs[i].thread_id = i;
        ctxs[i].ifindex = ifindex;
        ctxs[i].block_size = block_size;
        ctxs[i].block_nr = block_nr;
        ctxs[i].frame_size = frame_size;
        ctxs[i].fanout_id = fanout_id;
        ctxs[i].kstats = calloc(1, sizeof(struct tpacket_stats));
        if (ctxs[i].kstats == NULL) {
            fprintf(stderr, "allocation failed for kstats\n");
            for (uint8_t j = 0; j < i; j++) 
                free(ctxs[j].kstats);

            return EXIT_FAILURE;
        }

        pthread_attr_t thread_attrs;
        pthread_attr_init(&thread_attrs);

        cpu_set_t current_cpu_set;
        int cpu_to_bind = (start_thread + i) % MAX_CORES;
        CPU_ZERO(&current_cpu_set);
        CPU_SET(cpu_to_bind, &current_cpu_set);

        if (pthread_attr_setaffinity_np(&thread_attrs, sizeof(cpu_set_t), &current_cpu_set) != 0)
            fprintf(stdout, "Can't set CPU affinity for thread\n");

        if (pthread_create(&threads[i], &thread_attrs, af_packet_thread, &ctxs[i]) != 0) {
            fprintf(stderr, "failed to create thread %u\n", i);
            exit_indicator = 1;
            break;
        }

        pthread_attr_destroy(&thread_attrs);
    }

    struct timespec start, current;
    clock_gettime(CLOCK_MONOTONIC, &start);
    while (1 && exit_indicator == 0) {
        clock_gettime(CLOCK_MONOTONIC, &current);
        if ((current.tv_sec - start.tv_sec) >= duration) {
            exit_indicator = 1;
            break;
        }
        sleep(1);
    }

    // Stop all threads
    if (exit_indicator == 0) 
        fprintf(stdout, "%d seconds elapsed. Stopping all threads ...\n", duration);
  
    exit_indicator = 1;

    uint64_t total_packets = 0;
    uint64_t total_drops = 0;
    for (uint8_t i = 0; i < num_threads; i++) {
        if (threads[i] != 0) {
            pthread_join(threads[i], NULL);
            if (ctxs[i].kstats != NULL) {
                total_packets += ctxs[i].kstats->tp_packets;
                total_drops += ctxs[i].kstats->tp_drops;
            }
        }
    }

    for (uint8_t i = 0; i < num_threads; i++)
        free(ctxs[i].kstats);

    fprintf(stdout, "Total packets: %llu\n", (unsigned long long)total_packets);
    fprintf(stdout, "Total drops: %llu\n", (unsigned long long)total_drops);
    return 0;
}
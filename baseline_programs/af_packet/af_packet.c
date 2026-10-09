#include <arpa/inet.h>
#include <errno.h>
#include <fcntl.h>
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
#include <sys/ioctl.h>
#include <sys/mman.h>
#include <sys/socket.h>
#include <sys/types.h>
#include <unistd.h>
#include <time.h>
#include <sched.h>

#define MAX_CORES 48;


typedef struct ThreadCtx_ {
    int thread_id;
    int ifindex;
    uint32_t block_size;
    uint32_t block_nr;
    uint32_t frame_size;
    uint32_t fanout_id;
    char ifname[IFNAMSIZ];
    struct tpacket_stats *kstats;
} ThreadCtx;

static volatile sig_atomic_t exit_indicator = 0;
static void handle_signal(int sig){
    exit_indicator = 1;
    fprintf(stdout, "Signal received, stopping all threads...\n");
}

static int get_ifindex(int fd, const char *ifname, int verbose){
    struct ifreq ifr;

    memset(&ifr, 0, sizeof(ifr));
    strncpy(ifr.ifr_name, ifname, IFNAMSIZ - 1);

    if (ioctl(fd, SIOCGIFINDEX, &ifr) == -1) {
        if (verbose) {
            fprintf(stderr, "%s: failed to find interface: %s\n", ifname, strerror(errno));
        }
        return -1;
    }

    return ifr.ifr_ifindex;
}

static int setup_socket(ThreadCtx *ctx, struct tpacket_req3 *req){
    int fd;
    int version = TPACKET_V3;
    int fanout_arg;
    struct sockaddr_ll bind_addr;
    struct packet_mreq mreq;

    fd = socket(AF_PACKET, SOCK_RAW, htons(ETH_P_ALL));
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
    req->tp_retire_blk_tov = 64;

    if (setsockopt(fd, SOL_PACKET, PACKET_RX_RING, req, sizeof(*req)) < 0) {
        fprintf(stderr, "thread %d: PACKET_RX_RING failed: %s\n", ctx->thread_id, strerror(errno));
        close(fd);
        return -1;
    }

    memset(&bind_addr, 0, sizeof(bind_addr));
    bind_addr.sll_family = AF_PACKET;
    bind_addr.sll_protocol = htons(ETH_P_ALL);
    bind_addr.sll_ifindex = ctx->ifindex;

    if (bind(fd, (struct sockaddr *)&bind_addr, sizeof(bind_addr)) < 0) {
        fprintf(stderr, "thread %d: bind failed: %s\n", ctx->thread_id, strerror(errno));
        close(fd);
        return -1;
    }

    memset(&mreq, 0, sizeof(mreq));
    mreq.mr_ifindex = ctx->ifindex;
    mreq.mr_type = PACKET_MR_PROMISC;
    if (setsockopt(fd, SOL_PACKET, PACKET_ADD_MEMBERSHIP, &mreq, sizeof(mreq)) < 0) {
        fprintf(stderr, "thread %d: PACKET_MR_PROMISC failed: %s\n", ctx->thread_id, strerror(errno));
        close(fd);
        return -1;
    }

    fanout_arg = (int)((ctx->fanout_id & 0xFFFFU) | ((uint32_t)PACKET_FANOUT_CPU << 16));
    if (setsockopt(fd, SOL_PACKET, PACKET_FANOUT, &fanout_arg, sizeof(fanout_arg)) < 0) {
        fprintf(stderr, "thread %d: PACKET_FANOUT failed: %s\n", ctx->thread_id, strerror(errno));
        close(fd);
        return -1;
    }

    return fd;
}

typedef struct {
    uint64_t packets;
    uint64_t bytes;
    uint64_t drops;
} ThreadStats;

static void *af_packet_thread(void *arg){
    ThreadCtx *ctx = (ThreadCtx *)arg;

    uint32_t block_idx = 0;
    uint64_t packets = 0;
    uint64_t bytes = 0;

    struct tpacket_req3 req;
    int fd = setup_socket(ctx, &req);
    if (fd < 0) {
        return NULL;
    }

    size_t ring_len = (size_t)req.tp_block_size * req.tp_block_nr;
    uint8_t *ring = mmap(NULL, ring_len, PROT_READ | PROT_WRITE, MAP_SHARED | MAP_LOCKED, fd, 0);
    if (ring == MAP_FAILED) {
        fprintf(stderr, "thread %d: mmap failed: %s\n", ctx->thread_id, strerror(errno));
        close(fd);
        return NULL;
    }

    struct pollfd pfd;
    int ret, nfds = 1;
    pfd.fd = fd;
    pfd.events = POLLIN;
    while (!exit_indicator) {
        ret = poll(&pfd, nfds, 0);
        if (ret < 0) {
            if (errno == EINTR) 
                continue;
            
            fprintf(stderr, "thread %d: poll failed: %s\n", ctx->thread_id, strerror(errno));
            break;
        }
        if (ret == 0) 
            continue;

        struct tpacket_block_desc *bd = (struct tpacket_block_desc *)(ring + ((size_t)block_idx * req.tp_block_size));
        if ((bd->hdr.bh1.block_status & TP_STATUS_USER) == 0) 
            continue;

        uint32_t offset = bd->hdr.bh1.offset_to_first_pkt;
        struct tpacket3_hdr *ppd;
        for (uint32_t i = 0; i < bd->hdr.bh1.num_pkts; i++) {
            ppd = (struct tpacket3_hdr *)((uint8_t *)bd + offset);
            packets++;
            bytes += ppd->tp_snaplen;
            offset += ppd->tp_next_offset;
        }

        bd->hdr.bh1.block_status = TP_STATUS_KERNEL;
        block_idx = (block_idx + 1U) % req.tp_block_nr;
    }

    fprintf(stdout, "Packets processed: %llu bytes processed: %llu\n", (unsigned long long)packets, (unsigned long long)bytes);
    struct tpacket_stats kstats;
    socklen_t len = sizeof(kstats);
    if (getsockopt(fd, SOL_PACKET, PACKET_STATISTICS, (void *)&kstats, &len) > -1)
        fprintf(stdout, "thread %d (%s): kernel packets=%llu kernel dropped=%llu\n",
                ctx->thread_id,
                ctx->ifname,
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
    
    if (argc < 6) {
        fprintf(stderr, "Usage: %s <num_threads> <interface> <block_size> <block_nr> <duration>\n", argv[0]);
        return 1;
    }

    unsigned long parsed_threads = strtoul(argv[1], NULL, 10);
    if (parsed_threads == 0 || parsed_threads > 255) {
        fprintf(stderr, "Invalid num_threads: %s\n", argv[1]);
        return 1;
    }
    uint8_t num_threads = (uint8_t)parsed_threads;

    char *ifname = argv[2];
    uint32_t frame_size = 2048;
    uint32_t block_size = (uint32_t)strtoul(argv[3], NULL, 10);
    uint32_t block_nr = (uint32_t)strtoul(argv[4], NULL, 10);
    uint32_t duration = (uint32_t)strtoul(argv[5], NULL, 10);

    if (block_size == 0 || block_nr == 0) {
        fprintf(stderr, "block_size and block_nr must be > 0\n");
        return 1;
    }

    if (block_size % getpagesize() != 0) {
        fprintf(stderr, "block_size must be page-aligned (multiple of %d)\n", getpagesize());
        return 1;
    }

    if (block_size % frame_size != 0) {
        fprintf(stderr, "block_size must be a multiple of frame_size (%u)\n", frame_size);
        return 1;
    }

    int fd = socket(AF_PACKET, SOCK_RAW, htons(ETH_P_ALL));
    if (fd < 0) {
        perror("socket");
        return 1;
    }

    int ifindex = get_ifindex(fd, ifname, 1);
    close(fd);
    if (ifindex < 0) {
        return 1;
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

            return 1;
        }
        strncpy(ctxs[i].ifname, ifname, IFNAMSIZ - 1);

        pthread_attr_t thread_attrs;
        pthread_attr_init(&thread_attrs);

        cpu_set_t current_cpu_set;
        int cpu_to_bind = i % MAX_CORES;
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
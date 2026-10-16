// Based on https://github.com/gamemann/xdpstats-c/tree/master

#include <errno.h>
#include <locale.h>
#include <poll.h>
#include <pthread.h>
#include <signal.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include <unistd.h>
#include <sys/resource.h>
#include <getopt.h>
#include <sys/socket.h>

#include <arpa/inet.h>
#include <net/if.h>
#include <linux/if_link.h>
#include <linux/if_ether.h>
#include <linux/ip.h>
#include <linux/ipv6.h>
#include <linux/udp.h>
#include <linux/tcp.h>
#include <linux/icmpv6.h>

#include <sys/sysinfo.h>

#include <linux/types.h>
#include <linux/bpf.h>

#include <bpf/libbpf.h>
#include <bpf.h>
#include <xdp/xsk.h>

#include "af_xdp.h"

// #define DEBUG

static volatile sig_atomic_t exit_indicator = 0;
static void handle_signal(int sig){
    (void)sig;
    exit_indicator = 1;
     printf("Signal received, stopping all threads...\n");
}


static struct xsk_umem_info *configure_xsk_umem(void *buffer, __u64 size){
    struct xsk_umem_info *umem_info;
    int ret;

    umem_info = calloc(1, sizeof(*umem_info));
    if (!umem_info)
        return NULL;

    ret = xsk_umem__create(&umem_info->umem, buffer, size, &umem_info->fq, &umem_info->cq, NULL);
    if (ret) {
        errno = -ret;
        return NULL;
    }

    umem_info->buffer = buffer;
    return umem_info;
}

static struct xsk_socket_info *xsk_configure_socket(struct xsk_umem_info *umem_info, int rxqueue, const char *dev, int xsk_map_fd, __u32 flags, __u64 num_frames, __u8 busy_poll_mode){
    struct xsk_socket_info *xsk_info = calloc(1, sizeof(*xsk_info));
    if (!xsk_info){
        fprintf(stderr, "xsk_info = NULL\n");
        return NULL;
    }
    xsk_info->umem_info = umem_info;

    struct xsk_socket_config xsk_cfg;
    xsk_cfg.rx_size = XSK_RING_CONS__DEFAULT_NUM_DESCS;
    xsk_cfg.tx_size = XSK_RING_PROD__DEFAULT_NUM_DESCS;
    xsk_cfg.libbpf_flags = XSK_LIBBPF_FLAGS__INHIBIT_PROG_LOAD;
    xsk_cfg.xdp_flags = flags;
    xsk_cfg.bind_flags = XDP_ZEROCOPY;

    int ret = xsk_socket__create(&xsk_info->xsk, dev, rxqueue, umem_info->umem, &xsk_info->rx, &xsk_info->tx, &xsk_cfg);
    if (ret) {
        fprintf(stderr, "xdp_socket__create :: Error.\n");
        return NULL;
    }

    ret = xsk_socket__update_xskmap(xsk_info->xsk, xsk_map_fd);
    if (ret) {
        fprintf(stderr, "xsk_socket__update_xskmap :: Error.\n");
        return NULL;
    }

    xsk_info->busy_poll_mode = busy_poll_mode;

    if (xsk_info->busy_poll_mode) {
        printf("[XSK] Enabling busy poll mode for XSK socket...\n");
        int opt = 1;
        ret = setsockopt(xsk_socket__fd(xsk_info->xsk), SOL_SOCKET, SO_PREFER_BUSY_POLL, (void *)&opt,
                         sizeof(opt));
        if (ret) {
            fprintf(stderr, "setsockopt(SO_PREFER_BUSY_POLL) failed\n");
            return NULL;
        }

        /* poll for 20us if socket not ready */
        opt = 20;
        ret = setsockopt(xsk_socket__fd(xsk_info->xsk), SOL_SOCKET, SO_BUSY_POLL, (void *)&opt,
                         sizeof(opt));
        if (ret) {
            fprintf(stderr, "setsockopt(SO_BUSY_POLL) failed\n");
            return NULL;
        }

        /* send/recv RX_BATCH_SIZE packets at most */
        opt = RX_BATCH_SIZE;
        ret = setsockopt(xsk_socket__fd(xsk_info->xsk), SOL_SOCKET, SO_BUSY_POLL_BUDGET, (void *)&opt,
                         sizeof(opt));
        if (ret) {
            fprintf(stderr, "setsockopt(SO_BUSY_POLL_BUDGET) failed\n");
            return NULL;
        }
    }

    // Stuff the receive path with as many buffers as we actually have.
    __u32 idx;
    __u32 frames_to_fill = num_frames < XSK_RING_PROD__DEFAULT_NUM_DESCS ? (__u32)num_frames
                                                                          : XSK_RING_PROD__DEFAULT_NUM_DESCS;

    ret = xsk_ring_prod__reserve(&xsk_info->umem_info->fq, frames_to_fill, &idx);
    if (ret != (int)frames_to_fill){
        fprintf(stderr, "ret != frames_to_fill :: Error (ret=%d expected=%u).\n", ret, frames_to_fill);
        return NULL;
    }

    for (__u32 i = 0; i < frames_to_fill; i++)
        *xsk_ring_prod__fill_addr(&xsk_info->umem_info->fq, idx++) = (__u64)i * FRAME_SIZE;

    xsk_ring_prod__submit(&xsk_info->umem_info->fq, frames_to_fill);

    return xsk_info;
}

void *PollXSK(void *data){
    struct thread_info *ti = (struct thread_info *)data;

    int ret;
    int fd = xsk_socket__fd(ti->xsk_info->xsk);
    struct pollfd pfd;
    int nfds = 1;

    pfd.fd = fd;
    pfd.events = POLLIN;

    #ifdef DEBUG
        printf("[XSK] Starting busy-poll loop for FD %d...\n", fd);
    #endif


    while (!ti->done){
        if (!ti->xsk_info->busy_poll_mode) {
            ret = poll(&pfd, nfds, 100);
            if (ret <= 0)
                continue;
        }

        __u32 idx_rx, idx_fq = 0;
        __u32 rcvd = xsk_ring_cons__peek(&ti->xsk_info->rx, RX_BATCH_SIZE, &idx_rx);
        if (!rcvd){
            if (ti->xsk_info->busy_poll_mode || xsk_ring_prod__needs_wakeup(&ti->xsk_info->umem_info->fq)) {
                ret = recvfrom(fd, NULL, 0, MSG_DONTWAIT, NULL, NULL);
                if (ret == -1 && errno != EAGAIN && errno != EBUSY) 
                    fprintf(stderr, "recvfrom() failed: %s\n", strerror(errno));
            }
            continue;
        }

        #ifdef DEBUG
            printf("[XSK] Received %d packets from AF_XDP socket from queue ID %d\n", rcvd, ti->id);
        #endif

        int res = xsk_ring_prod__reserve(&ti->xsk_info->umem_info->fq, rcvd, &idx_fq);
        while (res != (int)rcvd) {
            if (ti->xsk_info->busy_poll_mode || xsk_ring_prod__needs_wakeup(&ti->xsk_info->umem_info->fq)) {
                ret = recvfrom(fd, NULL, 0, MSG_DONTWAIT, NULL, NULL);
                if (ret == -1 && errno != EAGAIN && errno != EBUSY) 
                    fprintf(stderr, "recvfrom() failed: %s\n", strerror(errno));

                if (ret < 0) 
                    continue;
            }

            res = xsk_ring_prod__reserve(&ti->xsk_info->umem_info->fq, rcvd, &idx_fq);
        }

        for (__u32 j = 0; j < rcvd; j++){
            __u64 addr = xsk_ring_cons__rx_desc(&ti->xsk_info->rx, idx_rx)->addr;
            __u32 len = xsk_ring_cons__rx_desc(&ti->xsk_info->rx, idx_rx++)->len;
            __u64 frame = xsk_umem__extract_addr(addr);

            void *pckt = xsk_umem__get_data(ti->xsk_info->umem_info->buffer, xsk_umem__add_offset_to_addr(addr));
            if (!pckt){
                printf("[XSK] Packet not true; dropping frame.\n");
                *xsk_ring_prod__fill_addr(&ti->xsk_info->umem_info->fq, idx_fq++) = frame;
                continue;
            }

            ti->packets++;
            ti->bytes += len;

            *xsk_ring_prod__fill_addr(&ti->xsk_info->umem_info->fq, idx_fq++) = frame;
        }
        xsk_ring_prod__submit(&ti->xsk_info->umem_info->fq, rcvd);
        xsk_ring_cons__release(&ti->xsk_info->rx, rcvd);
    }

    #ifdef DEBUG
        printf("[XSK] Exiting poll...\n");
    #endif

    pthread_exit(NULL);
}

int run_af_xdp(__u32 xdpflags, struct bpf_object *obj, __u32 num_threads, int start_thread, const char *dev, __u64 num_frames, __u32 duration, __u8 busy_poll_mode){
    signal(SIGINT, handle_signal);
    signal(SIGTERM, handle_signal);

    // Get the xsks_map file descriptor from the BPF object
    struct bpf_map *xsks_map = bpf_object__find_map_by_name(obj, "xsks_map");
    if (!xsks_map) {
        fprintf(stderr, "ERROR :: Could not find xsks_map in BPF object\n");
        return EXIT_FAILURE;
    }
    
    int xsks_fd = bpf_map__fd(xsks_map);
    if (xsks_fd < 0) {
        fprintf(stderr, "ERROR :: Invalid map file descriptor: %d\n", xsks_fd);
        return EXIT_FAILURE;
    }

    struct thread_info tinfo[num_threads];
    pthread_t threads[num_threads];

    printf("Creating AF_XDP sockets. Dev => %s. Queues => %d.\n", dev, num_threads);
   
    __u64 packet_buffer_size = num_frames * FRAME_SIZE * 2;
    __u8 sk_created = 0;
    for (__u32 i = 0; i < num_threads; i++){
        // Allocate per-thread packet buffer
        void *packet_buffer;
        if (posix_memalign(&packet_buffer, getpagesize(), packet_buffer_size)) {
            fprintf(stderr, "ERROR :: Can't allocate buffer memory for thread %d => \"%s\".\n", i, strerror(errno));
            continue;
        }

        // Initialize per-thread umem with its own packet buffer
        struct xsk_umem_info *umem = configure_xsk_umem(packet_buffer, packet_buffer_size);
        if (umem == NULL) {
            fprintf(stderr, "ERROR :: Can't create umem for thread %d => \"%s\"\n", i, strerror(errno));
            free(packet_buffer);
            continue;
        }

        // Open and configure the AF_XDP (xsk) socket.
        struct xsk_socket_info *xsk_socket = xsk_configure_socket(umem, i, (const char *)dev, xsks_fd, xdpflags, num_frames, busy_poll_mode);
        if (xsk_socket == NULL) {
            fprintf(stderr, "ERROR :: Can't setup AF_XDP socket for thread %d => \"%s\"\n", i, strerror(errno));
            xsk_umem__delete(umem->umem);
            free(umem);
            free(packet_buffer);
            continue;
        }

        tinfo[i].id = i;
        tinfo[i].xsk_info = xsk_socket;
        tinfo[i].packets = 0;
        tinfo[i].bytes = 0;
        tinfo[i].done = 0;   
        
        pthread_attr_t thread_attrs;
        pthread_attr_init(&thread_attrs);

        cpu_set_t current_cpu_set;
        int cpu_to_bind = (start_thread + i) % MAX_CORES;
        CPU_ZERO(&current_cpu_set);
        CPU_SET(cpu_to_bind, &current_cpu_set);

        if (pthread_attr_setaffinity_np(&thread_attrs, sizeof(cpu_set_t), &current_cpu_set) != 0)
            fprintf(stdout, "Can't set CPU affinity for thread\n");

        if (pthread_create(&threads[i], &thread_attrs, PollXSK, (void *)&tinfo[i]) != 0) {
            fprintf(stderr, "failed to create thread %u\n", i);
            exit_indicator = 1;
            break;
        }

        pthread_attr_destroy(&thread_attrs);
        sk_created++;
    }
    printf("Created %u XSK sockets with dedicated UMEM totaling %llu bytes\n", sk_created, packet_buffer_size*sk_created);

    __u64 timelap = 0;  
    while (!exit_indicator){
        sleep(1);
        if (duration > 0 && timelap >= duration)
            break;

        timelap++;
    }

    // Signal threads to stop
    for (__u32 i = 0; i < num_threads; i++)
        tinfo[i].done = 1;

    // Join all threads
    __u64 total_packets = 0;
    __u64 total_bytes = 0;
    for (__u32 i = 0; i < num_threads; i++) {
        pthread_join(threads[i], NULL);
        total_packets += tinfo[i].packets;
        total_bytes += tinfo[i].bytes;
        printf("Thread %d: Packets = %llu | Bytes = %llu\n", i, tinfo[i].packets, tinfo[i].bytes);

        xsk_socket__delete(tinfo[i].xsk_info->xsk);
        free(tinfo[i].xsk_info->umem_info->buffer);
        xsk_umem__delete(tinfo[i].xsk_info->umem_info->umem);

        free(tinfo[i].xsk_info->umem_info);
        free(tinfo[i].xsk_info);
    }


    printf("\nFinal Results:\n");
    printf("Duration: %llu seconds | Threads: %d\n", timelap, num_threads);
    
    printf("Total packets: %llu\n", total_packets);
    printf("Total bytes: %llu\n", total_bytes);

    return EXIT_SUCCESS;
}
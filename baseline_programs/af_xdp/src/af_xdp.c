// Based on https://github.com/gamemann/xdpstats-c/tree/master

#include <assert.h>
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

// Allocate a UMEM frame from the pool of available frames for the given XSK socket info.
static __u64 xsk_alloc_umem_frame(struct xsk_socket_info *xsk){
    __u64 frame;
    if (xsk->umem_frame_free == 0)
        return INVALID_UMEM_FRAME;

    frame = xsk->umem_frame_addr[--xsk->umem_frame_free];
    xsk->umem_frame_addr[xsk->umem_frame_free] = INVALID_UMEM_FRAME;

    return frame;
}

static void xsk_free_umem_frame(struct xsk_socket_info *xsk, __u64 frame, __u64 num_frames){
    assert(xsk->umem_frame_free < num_frames);
    xsk->umem_frame_addr[xsk->umem_frame_free++] = frame;
}

static struct xsk_socket_info *xsk_configure_socket(struct xsk_umem_info *umem_info, int rxqueue, const char *dev, int xsk_map_fd, __u32 flags, __u64 num_frames){
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

    // Initialize umem frame allocation.
    xsk_info->umem_frame_addr = calloc(num_frames, sizeof(*xsk_info->umem_frame_addr));
    for (__u64 i = 0; i < num_frames; i++)
        xsk_info->umem_frame_addr[i] = i * FRAME_SIZE;

    xsk_info->umem_frame_free = num_frames;

    // Stuff the receive path with buffers, we assume we have enough.
    __u32 idx;
    ret = xsk_ring_prod__reserve(&xsk_info->umem_info->fq, XSK_RING_PROD__DEFAULT_NUM_DESCS, &idx);
    if (ret != XSK_RING_PROD__DEFAULT_NUM_DESCS){
        fprintf(stderr, "ret != XSK_RING_PROD__DEFAULT_NUM_DESCS :: Error.\n");
        return NULL;
    }

    for (int i = 0; i < XSK_RING_PROD__DEFAULT_NUM_DESCS; i++)
        *xsk_ring_prod__fill_addr(&xsk_info->umem_info->fq, idx++) = xsk_alloc_umem_frame(xsk_info);

    xsk_ring_prod__submit(&xsk_info->umem_info->fq, XSK_RING_PROD__DEFAULT_NUM_DESCS);

    return xsk_info;
}

void *PollXSK(void *data){
    struct thread_info *ti = (struct thread_info *)data;

    struct pollfd pfd;
    int ret, nfds = 1;
    pfd.fd = xsk_socket__fd(ti->xsk_info->xsk);
    pfd.events = POLLIN;

    #ifdef DEBUG
        printf("[XSK] Starting to poll for FD %d...\n", pfd.fd);
    #endif

    while (!ti->done){
        ret = poll(&pfd, nfds, 0);
        if (ret != 1)
            continue;

        __u32 idx_rx, idx_fq = 0;
        // Reads RX_BATCH_SIZE packets
        __u32 rcvd = xsk_ring_cons__peek(&ti->xsk_info->rx, RX_BATCH_SIZE, &idx_rx);
        if (!rcvd)
            continue;

        #ifdef DEBUG
            printf("[XSK] Received %d packets from AF_XDP socket from queue ID %d\n", rcvd, ti->id);
        #endif

        int stockframes = xsk_prod_nb_free(&ti->xsk_info->umem_info->fq, ti->xsk_info->umem_frame_free);
        if (stockframes > 0){
            #ifdef DEBUG
                printf("[XSK] We have %d stock frames available to refill.\n", stockframes);
            #endif

            ret = xsk_ring_prod__reserve(&ti->xsk_info->umem_info->fq, stockframes, &idx_fq);
            while (ret != stockframes)
			    ret = xsk_ring_prod__reserve(&ti->xsk_info->umem_info->fq, stockframes, &idx_fq);

            // Only fill what we successfully reserved
            for (int j = 0; j < ret; j++) 
                *xsk_ring_prod__fill_addr(&ti->xsk_info->umem_info->fq, idx_fq++) = xsk_alloc_umem_frame(ti->xsk_info);
            
            xsk_ring_prod__submit(&ti->xsk_info->umem_info->fq, ret);
            
            #ifdef DEBUG
                printf("[XSK] Refilled fill queue with %d frames.\n", ret);
            #endif
        }

        for (__u32 j = 0; j < rcvd; j++){
            __u64 addr = xsk_ring_cons__rx_desc(&ti->xsk_info->rx, idx_rx)->addr;
            __u32 len = xsk_ring_cons__rx_desc(&ti->xsk_info->rx, idx_rx++)->len;

            void *pckt = xsk_umem__get_data(ti->xsk_info->umem_info->buffer, addr);
            if (!pckt){
                printf("[XSK] Packet not true; freeing frame.\n");
                xsk_free_umem_frame(ti->xsk_info, addr, ti->num_frames);
                continue;
            }

            ti->packets++;
            ti->bytes += len;
            
            xsk_free_umem_frame(ti->xsk_info, addr, ti->num_frames);
        }
        xsk_ring_cons__release(&ti->xsk_info->rx, rcvd);
    }

    #ifdef DEBUG
        printf("[XSK] Exiting poll...\n");
    #endif

    pthread_exit(NULL);
}

/**
 * Run AF_XDP program
 * 
 * @param dev The interface the XDP program exists on (string).
 * @param ifidx The interface the XDP program exists on (index number).
 * @param xdpflags The XDP attachment flags.
 * @param threads Number of threads to use.
 * @param obj The BPF object (to access xsks_map).
 * 
 * @return Returns 0 on success or 1 on failure.
**/
int run_af_xdp(const char *dev, int ifidx, __u32 xdpflags, __u32 num_threads, struct bpf_object *obj, __u64 num_frames, __u32 duration){
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

    printf("Creating AF_XDP sockets. Dev => %s. Index => %d. Queues => %d.\n", dev, ifidx, num_threads);
   
    __u64 packet_buffer_size = num_frames * FRAME_SIZE;
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
        struct xsk_socket_info *xsk_socket = xsk_configure_socket(umem, i, (const char *)dev, xsks_fd, xdpflags, num_frames);
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
        tinfo[i].num_frames = num_frames;    

        pthread_create(&threads[i], NULL, PollXSK, (void *)&tinfo[i]);
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
        free(tinfo[i].xsk_info->umem_frame_addr);

        free(tinfo[i].xsk_info);
    }

    __u64 avgpps = 0;
    if (total_packets > 0 && timelap > 0)
        avgpps = total_packets / timelap;

    __u64 avgbps = 0;
    if (total_bytes > 0 && timelap > 0)
        avgbps = total_bytes / timelap;

    printf("\nFinal Results:\n");
    printf("Total packets: %llu | Avg PPS: %llu\n", total_packets, avgpps);
    printf("Total bytes: %llu | Avg BPS: %llu\n", total_bytes, avgbps);
    printf("Duration: %llu seconds | Threads: %d\n", timelap, num_threads);

    return EXIT_SUCCESS;
}
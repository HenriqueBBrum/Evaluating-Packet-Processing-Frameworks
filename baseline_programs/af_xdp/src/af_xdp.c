// Based on https://github.com/gamemann/xdpstats-c/tree/master

#include <assert.h>
#include <errno.h>
#include <getopt.h>
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

struct xsk_umem_info *umem[MAX_CPUS];
struct xsk_socket_info *xsk_socket[MAX_CPUS];
static void *packet_buffer_global = NULL;

static struct xsk_umem_info *configure_xsk_umem(void *buffer, __u64 size){
    struct xsk_umem_info *umem;
    int ret;

    umem = calloc(1, sizeof(*umem));
    if (!umem){
        return NULL;
    }

    ret = xsk_umem__create(&umem->umem, buffer, size, &umem->fq, &umem->cq, NULL);
    if (ret) {
        errno = -ret;
        return NULL;
    }

    umem->buffer = buffer;
    return umem;
}

static __u64 xsk_alloc_umem_frame(struct xsk_socket_info *xsk){
    __u64 frame;
    if (xsk->umem_frame_free == 0){
        return INVALID_UMEM_FRAME;
    }

    frame = xsk->umem_frame_addr[--xsk->umem_frame_free];
    xsk->umem_frame_addr[xsk->umem_frame_free] = INVALID_UMEM_FRAME;

    return frame;
}

static void xsk_free_umem_frame(struct xsk_socket_info *xsk, __u64 frame){
    assert(xsk->umem_frame_free < NUM_FRAMES);
    xsk->umem_frame_addr[xsk->umem_frame_free++] = frame;
}

static __u64 get_xsk_umem_free_frames(struct xsk_socket_info *xsk){
    return xsk->umem_frame_free;
}

static struct xsk_socket_info *xsk_configure_socket(struct xsk_umem_info *umem, int rxqueue, const char *dev, int xsk_map_fd, __u32 flags){
    struct xsk_socket_config xsk_cfg;
    struct xsk_socket_info *xsk_info;
    __u32 idx;
    int i;
    int ret;

    xsk_info = calloc(1, sizeof(*xsk_info));
    if (!xsk_info){
        fprintf(stderr, "xsk_info = NULL\n");
        return NULL;
    }

    xsk_info->umem = umem;
    xsk_cfg.rx_size = XSK_RING_CONS__DEFAULT_NUM_DESCS;
    xsk_cfg.tx_size = XSK_RING_PROD__DEFAULT_NUM_DESCS;
    xsk_cfg.libbpf_flags = XSK_LIBBPF_FLAGS__INHIBIT_PROG_LOAD;
    xsk_cfg.xdp_flags = flags;
    xsk_cfg.bind_flags = XDP_ZEROCOPY;

    ret = xsk_socket__create(&xsk_info->xsk, dev, rxqueue, umem->umem, &xsk_info->rx, &xsk_info->tx, &xsk_cfg);
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
    for (i = 0; i < NUM_FRAMES; i++)
        xsk_info->umem_frame_addr[i] = i * FRAME_SIZE;

    xsk_info->umem_frame_free = NUM_FRAMES;

    // Stuff the receive path with buffers, we assume we have enough.
    ret = xsk_ring_prod__reserve(&xsk_info->umem->fq, XSK_RING_PROD__DEFAULT_NUM_DESCS, &idx);
    if (ret != XSK_RING_PROD__DEFAULT_NUM_DESCS){
        fprintf(stderr, "ret != XSK_RING_PROD__DEFAULT_NUM_DESCS :: Error.\n");
        return NULL;
    }

    for (i = 0; i < XSK_RING_PROD__DEFAULT_NUM_DESCS; i++){
        *xsk_ring_prod__fill_addr(&xsk_info->umem->fq, idx++) = xsk_alloc_umem_frame(xsk_info);
    }
    xsk_ring_prod__submit(&xsk_info->umem->fq, XSK_RING_PROD__DEFAULT_NUM_DESCS);

    return xsk_info;
}

void *PollXSK(void *data){
    struct thread_info *ti = (struct thread_info *)data;

    struct pollfd fds[2];
    int ret, nfds = 1;

    memset(fds, 0, sizeof(fds));
    fds[0].fd = xsk_socket__fd(ti->xsk->xsk);
    fds[0].events = POLLIN;

    #ifdef DEBUG
        fprintf(stdout, "[XSK] Starting to poll for FD %d...\n", fds[0].fd);
    #endif

    while (!ti->done){
        ret = poll(fds, nfds, 1000);
        if (ret != 1)
            continue;

        __u32 idx_rx, idx_fq = 0;
        __u32 rcvd = xsk_ring_cons__peek(&ti->xsk->rx, RX_BATCH_SIZE, &idx_rx);

        if (!rcvd)
            continue;

        #ifdef DEBUG
            fprintf(stdout, "[XSK] Received %d packets from AF_XDP socket from queue ID %d\n", rcvd, ti->id);
        #endif

        int stockframes = xsk_prod_nb_free(&ti->xsk->umem->fq, get_xsk_umem_free_frames(ti->xsk));
        if (stockframes > 0){
            #ifdef DEBUG
                fprintf(stdout, "[XSK] We have %d stock frames available to refill.\n", stockframes);
            #endif

            ret = xsk_ring_prod__reserve(&ti->xsk->umem->fq, stockframes, &idx_fq);
            while (ret != stockframes)
			    ret = xsk_ring_prod__reserve(&ti->xsk->umem->fq, stockframes, &idx_fq);

            // Only fill what we successfully reserved
            for (int j = 0; j < ret; j++) {
                *xsk_ring_prod__fill_addr(&ti->xsk->umem->fq, idx_fq++) = xsk_alloc_umem_frame(ti->xsk);
            }
            xsk_ring_prod__submit(&ti->xsk->umem->fq, ret);
            
            #ifdef DEBUG
                fprintf(stdout, "[XSK] Refilled fill queue with %d frames.\n", ret);
            #endif
        }

        for (__u32 j = 0; j < rcvd; j++){
            __u64 addr = xsk_ring_cons__rx_desc(&ti->xsk->rx, idx_rx)->addr;
            __u32 len = xsk_ring_cons__rx_desc(&ti->xsk->rx, idx_rx++)->len;

            void *pckt = xsk_umem__get_data(ti->xsk->umem->buffer, addr);
            if (!pckt){
                fprintf(stdout, "[XSK] Packet not true; freeing frame.\n");
                xsk_free_umem_frame(ti->xsk, addr);
                continue;
            }

            // Count packets and bytes locally
            ti->packets++;
            ti->bytes += len;
            
            xsk_free_umem_frame(ti->xsk, addr);
        }
        xsk_ring_cons__release(&ti->xsk->rx, rcvd);
    }

    #ifdef DEBUG
        fprintf(stdout, "[XSK] Exiting poll...\n");
    #endif

    pthread_exit(NULL);
}

/**
 * Sets up XSK (AF_XDP) sockets.
 * 
 * @param dev The interface the XDP program exists on (string).
 * @param ifidx The interface the XDP program exists on (index number).
 * @param xdpflags The XDP attachment flags.
 * @param threads Number of threads to use.
 * @param obj The BPF object (to access xsks_map).
 * @param thread_info_out Output array of thread_info structures.
 * @param threads_out Output array of pthread_t handles.
 * 
 * @return Returns 0 on success or 1 on failure.
**/
int setupxsk(const char *dev, int ifidx, __u32 xdpflags, __u32 threads, struct bpf_object *obj, struct thread_info **thread_info_out, pthread_t **threads_out){
    void *packet_buffer;
    __u64 packet_buffer_size;

    // Get actual number of RX queues from the interface
    fprintf(stdout, "Attempting to setup AF_XDP sockets. Dev => %s. Index => %d. Queues => %d.\n", dev, ifidx, threads);

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
    fprintf(stdout, "Found xsks_map with fd=%d\n", xsks_fd);

    // Allocate memory for thread info and thread handles
    struct thread_info *tinfo = calloc(threads, sizeof(struct thread_info));
    pthread_t *thread_ids = calloc(threads, sizeof(pthread_t));
    if (!tinfo || !thread_ids) {
        fprintf(stderr, "ERROR :: Failed to allocate thread structures\n");
        free(tinfo);
        free(thread_ids);
        return EXIT_FAILURE;
    }

    // Allocate memory for NUM_FRAMES of the default XDP frame size.
    packet_buffer_size = NUM_FRAMES * FRAME_SIZE;
    if (posix_memalign(&packet_buffer, getpagesize(), packet_buffer_size)) {
        fprintf(stderr, "ERROR :: Can't allocate buffer memory for XSK sockets => \"%s\".\n", strerror(errno));
        free(tinfo);
        free(thread_ids);
        return EXIT_FAILURE;
    }
    packet_buffer_global = packet_buffer;

    for (__u32 i = 0; i < threads; i++){
        // Initialize shared packet_buffer for umem usage.
        umem[i] = configure_xsk_umem(packet_buffer, packet_buffer_size);
        if (umem[i] == NULL) {
            fprintf(stderr, "ERROR :: Can't create umem \"%s\"\n", strerror(errno));
            continue;
        }

        // Open and configure the AF_XDP (xsk) socket.
        xsk_socket[i] = xsk_configure_socket(umem[i], i, (const char *)dev, xsks_fd, xdpflags);
        if (xsk_socket[i] == NULL) {
            fprintf(stderr, "ERROR :: Can't setup AF_XDP socket \"%s\"\n", strerror(errno));
            continue;
        }

        // int socket_fd = xsk_socket__fd(xsk_socket[i]->xsk);
        // for (int j = 0; j < threads; j++){
        //     fprintf(stdout, "Registering socket fd=%d for queue %d in xsks_map\n", socket_fd, j);
        //     if(bpf_map_update_elem(xsks_fd, &j, &socket_fd, BPF_ANY) != 0) {
        //         fprintf(stderr, "ERROR :: Failed to update xsks_map for socket #%d \"%s\"\n", j, strerror(errno));
        //         continue;
        //     }else{
        //         fprintf(stdout, "Updated xsks_map for queue #%d successfully with fd=%d or %d\n", j, socket_fd, socketd_fd_);
        //     }
        // }

        tinfo[i].id = i;
        tinfo[i].xsk = xsk_socket[i];
        tinfo[i].packets = 0;
        tinfo[i].bytes = 0;
        tinfo[i].done = 0;       

        pthread_create(&thread_ids[i], NULL, PollXSK, (void *)&tinfo[i]);

        fprintf(stdout, "Created XSK socket #%d\n", i);
    }

    *thread_info_out = tinfo;
    *threads_out = thread_ids;
    return EXIT_SUCCESS;
}


void cleanupxsk(__u32 threads){
    // Delete sockets first (they depend on umem)
    for (__u32 i = 0; i < threads; i++){
        if (xsk_socket[i] != NULL && xsk_socket[i]->xsk != NULL){
            xsk_socket__delete(xsk_socket[i]->xsk);
            free(xsk_socket[i]);
            xsk_socket[i] = NULL;
        }
    }

    // Then delete umem
    for (__u32 i = 0; i < threads; i++){
        if (umem[i] != NULL && umem[i]->umem != NULL){
            xsk_umem__delete(umem[i]->umem);
            free(umem[i]);
            umem[i] = NULL;
        }
    }

    // Free packet buffer
    if (packet_buffer_global != NULL) {
        free(packet_buffer_global);
        packet_buffer_global = NULL;
    }
}
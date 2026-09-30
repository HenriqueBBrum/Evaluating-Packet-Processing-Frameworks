#pragma once

#include <linux/types.h>
#include <libbpf.h>
#include <xdp/xsk.h>
#include <pthread.h>

struct bpf_object;

#define MAX_CPUS 48

#define NUM_FRAMES 8192
#define FRAME_SIZE XSK_UMEM__DEFAULT_FRAME_SIZE
#define RX_BATCH_SIZE 64
#define INVALID_UMEM_FRAME UINT64_MAX

struct xsk_umem_info {
    struct xsk_ring_prod fq;
    struct xsk_ring_cons cq;
    struct xsk_umem *umem;
    void *buffer;
};

struct xsk_socket {
    struct xsk_ring_cons *rx;
    struct xsk_ring_prod *tx;
    __u64 outstanding_tx;
    struct xsk_ctx *ctx;
    struct xsk_socket_config config;
    int fd;
};

struct xsk_socket_info{
    struct xsk_ring_cons rx;
    struct xsk_ring_prod tx;
    struct xsk_umem_info *umem;
    struct xsk_socket *xsk;

    __u64 umem_frame_addr[NUM_FRAMES];
    __u32 umem_frame_free;
};

struct thread_info{
    int id;
    struct xsk_socket_info *xsk;
    __u64 packets;
    __u64 bytes;
    volatile int done;
};

struct stats{
    __u64 pkts;
    __u64 bytes;
};

__u32 bpf_num_possible_cpus();
int setupxsk(const char *dev, int ifidx, __u32 xdpflags, __u32 threads, struct bpf_object *obj, struct thread_info **thread_info_out, pthread_t **threads_out);
void cleanupxsk(__u32 threads);
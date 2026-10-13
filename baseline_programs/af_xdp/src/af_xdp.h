#pragma once

#include <linux/types.h>
#include <libbpf.h>
#include <xdp/xsk.h>
#include <pthread.h>

#define MAX_CORES 48

#define FRAME_SIZE XSK_UMEM__DEFAULT_FRAME_SIZE
#define RX_BATCH_SIZE 32
#define INVALID_UMEM_FRAME UINT64_MAX

struct bpf_object;


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
    struct xsk_umem_info *umem_info;
    struct xsk_socket *xsk;

    __u64 *umem_frame_addr;
    __u32 umem_frame_free;
};

struct thread_info{
    int id;
    struct xsk_socket_info *xsk_info;
    __u64 packets;
    __u64 bytes;
    volatile int done;
    __u64 num_frames;
};

struct stats{
    __u64 pkts;
    __u64 bytes;
};

int run_af_xdp(__u32 xdpflags, struct bpf_object *obj, __u32 num_threads, int start_thread, const char *dev, __u64 num_frames, __u32 duration);

#include <linux/bpf.h>
#include <bpf/bpf_helpers.h>

struct {
    __uint(type, BPF_MAP_TYPE_XSKMAP);
    __uint(max_entries, 48);  // Max queue count
    __type(key, int);
    __type(value, int);
} xsks_map SEC(".maps");

SEC("xdp")
int xdp_redirect_xsk(struct xdp_md *ctx){
    int queue_id = ctx->rx_queue_index;

    return bpf_redirect_map(&xsks_map, queue_id, XDP_PASS);
}

char _license[] SEC("license") = "GPL";

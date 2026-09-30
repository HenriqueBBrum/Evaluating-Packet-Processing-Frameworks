#include <stdio.h>
#include <unistd.h>
#include <stdlib.h>
#include <signal.h>
#include <errno.h>
#include <sys/resource.h>
#include <sys/socket.h>
#include <sys/ioctl.h>
#include <linux/sockios.h>
#include <linux/ethtool.h>


#include <bpf.h>
#include <libbpf.h>
#include <linux/if_link.h>

#include <net/if.h>
#include <arpa/inet.h>

#include "af_xdp.h"

static volatile sig_atomic_t cont = 1;

static void sighndl(int sig) {
    cont = 0;
}

/**
 * Retrieves the number of RX queues for a network interface using ioctl/ethtool.
 * 
 * @param ifname Interface name (e.g., "eth0")
 * @return Number of RX queues, or -1 on error.
**/
static int get_if_num_channels(const char *ifname) {
    int sock = socket(AF_INET, SOCK_DGRAM, 0);
    if (sock < 0) {
        fprintf(stderr, "ERROR :: Failed to create socket for ioctl: %s\n", strerror(errno));
        return 0;
    }
    
    struct ifreq ifr = {0};
    struct ethtool_channels channelparam = {0};
    
    strncpy(ifr.ifr_name, ifname, IFNAMSIZ - 1);
    channelparam.cmd = ETHTOOL_GCHANNELS;
    ifr.ifr_data = (char *)&channelparam;
    
    if (ioctl(sock, SIOCETHTOOL, &ifr) < 0) {
        fprintf(stderr, "ERROR :: ioctl SIOCETHTOOL failed for %s: %s\n", ifname, strerror(errno));
        close(sock);
        return 0;
    }
    
    close(sock);
    return channelparam.combined_count;
}


int main(int argc, char *argv[]){
    if (argc < 3) {
        fprintf(stderr, "Usage: %s <interface> <num_frames> <duration>\n", argv[0]);
        fprintf(stderr, "  interface: Network interface name\n");
        fprintf(stderr, "  num_frames: Number of frames in ring buffer\n");
        fprintf(stderr, "  duration: Duration to run in seconds (0 for infinite)\n");
        return EXIT_FAILURE;
    }

    char *interface = argv[1];
    int num_frames = atoi(argv[2]);
    __u32 duration = (__u32)atoi(argv[3]);

    if (num_frames <= 0) {
        fprintf(stderr, "num_frames must be > 0\n");
        return EXIT_FAILURE;
    }

    int ifidx = if_nametoindex(interface);
    if (ifidx < 0){
        fprintf(stderr, "Failed to get interface index for %s\n", interface);
        return EXIT_FAILURE;
    }

    struct bpf_object *obj = bpf_object__open_file("bin/redirect_to_af_xdp.o", NULL);
    if (!obj) {
        fprintf(stderr, "ERROR: Failed to open BPF object file\n");
        return EXIT_FAILURE;
    }

    int err = bpf_object__load(obj);
    if (err) {
        fprintf(stderr, "ERROR: Failed to load BPF object: %d\n", err);
        bpf_object__close(obj);
        return EXIT_FAILURE;
    }

    struct bpf_program * prog = bpf_object__find_program_by_name(obj, "xdp_redirect_xsk");
    if (!prog) {
        fprintf(stderr, "ERROR: Program 'xdp_redirect_xsk' not found in object file\n");
        bpf_object__close(obj);
        exit(EXIT_FAILURE);
    }

    __u32 flags = XDP_FLAGS_DRV_MODE | XDP_FLAGS_UPDATE_IF_NOEXIST;
    int prog_fd = bpf_program__fd(prog);
    err = bpf_xdp_attach(ifidx, prog_fd, flags, NULL);
    if (err) {
        fprintf(stderr, "ERROR: Failed to attach XDP program: %d\n", err);
        bpf_object__close(obj);
        exit(EXIT_FAILURE);
    }

    int channels = get_if_num_channels(interface);
    fprintf(stdout, "Interface %s has %d combined channels\n", interface, channels);
    
    struct thread_info *thread_info = NULL;
    pthread_t *threads = NULL;
    int err_setup = setupxsk(interface, ifidx, flags, channels, obj, &thread_info, &threads);
    if (err_setup != EXIT_SUCCESS) {
        fprintf(stderr, "Error setting up AF_XDP sockets\n");
        bpf_object__close(obj);
        return EXIT_FAILURE;
    }

    signal(SIGINT, sighndl);

    __u32 timelap = 0;
    while (cont){
        sleep(1);
        if (duration > 0 && timelap >= duration)
            break;

        timelap++;
    }

    // Signal threads to stop
    for (int i = 0; i < channels; i++)
        thread_info[i].done = 1;

    // Join all threads
    __u64 total_packets = 0;
    __u64 total_bytes = 0;
    for (int i = 0; i < channels; i++) {
        pthread_join(threads[i], NULL);
        total_packets += thread_info[i].packets;
        total_bytes += thread_info[i].bytes;
        fprintf(stdout, "Thread %d: Packets = %llu | Bytes = %llu\n", i, thread_info[i].packets, thread_info[i].bytes);
    }

    __u64 avgpps = 0;
    if (total_packets > 0 && timelap > 0) {
        avgpps = total_packets / timelap;
    }

    __u64 avgbps = 0;
    if (total_bytes > 0 && timelap > 0) {
        avgbps = total_bytes / timelap;
    }

    fprintf(stdout, "\nFinal Results:\n");
    fprintf(stdout, "Total packets: %llu | Avg PPS: %llu\n", total_packets, avgpps);
    fprintf(stdout, "Total bytes: %llu | Avg BPS: %llu\n", total_bytes, avgbps);
    fprintf(stdout, "Duration: %u seconds | Threads: %d\n", timelap, channels);

    bpf_xdp_detach(ifidx, flags, NULL);
    bpf_object__close(obj);
    cleanupxsk(channels);
    free(thread_info);
    free(threads);
    
    return EXIT_SUCCESS;
}
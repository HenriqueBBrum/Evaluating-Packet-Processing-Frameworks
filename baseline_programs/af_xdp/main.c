#include <stdio.h>
#include <unistd.h>
#include <stdlib.h>
#include <getopt.h>
#include <signal.h>
#include <errno.h>
#include <string.h>
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


void print_usage(const char *prog_name){
    printf("Usage: %s [OPTIONS]\n", prog_name);
    printf("\nAPP_OPTIONS:\n");
    printf("  -h, --help              Display this help message\n");
    printf("  -i, --interface         Network interface name (e.g., eth0)\n");
    printf("  -n, --num_frames        Number of frames in ring buffer (> 0)\n");
    printf("  -d, --duration          Duration in seconds (0 for infinite)\n");
    printf("\nExamples:\n");
    printf("  %s -i eth0 -n 1024 -d 10\n", prog_name);
    printf("  %s eth0 1024 10\n", prog_name);
}

int parse_app_args(int argc, char **argv, char **interface, int *num_frames, int *duration){
    int opt;
    struct option long_options[] = {
        {"help",    no_argument,       0, 'h'},
        {"interface", required_argument, 0, 'i'},
        {"num_frames",    required_argument, 0, 'n'},
        {"duration",    required_argument, 0, 'd'},
        {0, 0, 0, 0}
    };

    int option_index = 0;
    while ((opt = getopt_long(argc, argv, "hi:n:d:", long_options, &option_index)) != -1) {
        switch (opt) {
            case 'h':
                print_usage(argv[0]);
                exit(0);
            case 'i':
                *interface = optarg;
                break;
            case 'n':
                *num_frames = atoi(optarg);
                break;
            case 'd':
                *duration = atoi(optarg);
                break;
            case '?':
                fprintf(stderr, "Unknown option\n");
                print_usage(argv[0]);
                exit(1);
            default:
                break;
        }
    }

    return optind;
}


/**
 * Retrieves the number of combined queues for a network interface using ioctl/ethtool.
 * 
 * @param ifname Interface name (e.g., "eth0")
 * @return Number of combined queues, or -1 on error.
**/
static int get_if_num_channels(const char *ifname) {
    int sock = socket(AF_INET, SOCK_DGRAM, 0);
    if (sock < 0) {
        fprintf(stderr, "ERROR :: Failed to create socket for ioctl: %s\n", strerror(errno));
        return -1;
    }
    
    struct ifreq ifr = {0};
    struct ethtool_channels channelparam = {0};
    
    strncpy(ifr.ifr_name, ifname, IFNAMSIZ - 1);
    channelparam.cmd = ETHTOOL_GCHANNELS;
    ifr.ifr_data = (char *)&channelparam;
    
    if (ioctl(sock, SIOCETHTOOL, &ifr) < 0) {
        fprintf(stderr, "ERROR :: ioctl SIOCETHTOOL failed for %s: %s\n", ifname, strerror(errno));
        close(sock);
        return -1;
    }
    
    close(sock);
    return channelparam.combined_count;
}


int main(int argc, char *argv[]){
    char *interface = NULL;
    int num_frames = 0;
    int duration = 0;

    int first_nonopt = parse_app_args(argc, argv, &interface, &num_frames, &duration_input);

    if (!interface) {
        fprintf(stderr, "Missing interface argument\n");
        print_usage(argv[0]);
        return EXIT_FAILURE;
    }
   
    if (num_frames <= 0) {
        fprintf(stderr, "num_frames must be > 0\n");
        return EXIT_FAILURE;
    }

    if (duration <= 0 || duration > 360) {
        fprintf(stderr, "Duration outside of valid range of (0,360]\n");
        print_usage(argv[0]);
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
        return EXIT_FAILURE;
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
    printf("Interface %s has %d combined channels\n", interface, channels);
    
    /** Main function call **/
    int ret = run_af_xdp(interface,  ifidx, flags, channels, obj, num_frames, (__u32)duration);
    if (ret == EXIT_FAILURE)
        fprintf(stderr, "ERROR: Failure at main function\n");

    bpf_xdp_detach(ifidx, flags, NULL);
    bpf_object__close(obj);
    
    return EXIT_SUCCESS;
}
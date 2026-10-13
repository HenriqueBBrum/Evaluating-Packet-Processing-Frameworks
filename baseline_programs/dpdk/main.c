#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <signal.h>
#include <stdbool.h>
#include <time.h>
#include<unistd.h>
#include <getopt.h>


#include <rte_eal.h>
#include <rte_errno.h>
#include <rte_ethdev.h>
#include <rte_mbuf.h>

#define MBUF_CACHE_SIZE 512
#define BURST_SIZE 32

#define MAX_LCORES 48

struct lcore_args {
    uint16_t port_id;
    uint16_t queue_id;
    struct rte_mempool *mempool;
    uint64_t total_pkts;
    uint64_t total_bytes;
    uint16_t lcore_id;
    uint32_t duration;
};


// static rte_ring* ring_buffer = nullptr;

static volatile sig_atomic_t exit_indicator = 0;
static void handle_signal(int sig){
    exit_indicator = 1;
    printf("Signal received, stopping all threads...\n");
}

void print_usage(const char *prog_name){
    printf("Usage: %s [EAL_OPTIONS] -- [APP_OPTIONS]\n", prog_name);
    printf("DPDK_OPTIONS:\n");
    printf("  EAL options (see DPDK documentation at https://doc.dpdk.org/guides/linux_gsg/linux_eal_parameters.html)\n");
    printf("  Example: --lcores=0,1 -n 4\n");
    printf("\nAPP_OPTIONS:\n");
    printf("  -h, --help              Display this help message\n");
    printf("  -v, --verbose           Enable verbose output\n");
    printf("  -a, --pci_address       Specify the pci address of the desired interface\n");
    printf("  -r, --ring_size         Specify the rx ring buffer (queue) size of the desired interface\n");
    printf("  -n, --num_frames        Specify the number of packet slots in the memory pool\n");
    printf("  -d, --duration          Experiments duration\n");
    printf("\nExample: %s --lcores=0,1 -n 4 -- -v -a 0000:01:00.0 -r 1024 -n 8192 -d 30\n", prog_name);
}

int parse_app_args(int argc, char **argv, char **pci_address, int *verbose, int *ring_size, int *num_frames, int *duration){
    int opt;
    struct option long_options[] = {
        {"help",    no_argument,       0, 'h'},
        {"verbose", no_argument,       0, 'v'},
        {"pci_address",    required_argument, 0, 'a'},
        {"ring_size",    required_argument, 0, 'r'},
        {"num_frames",    required_argument, 0, 'n'},
        {"duration",    required_argument, 0, 'd'},
        {0, 0, 0, 0}
    };

    int option_index = 0;
    while ((opt = getopt_long(argc, argv, "hva:r:n:d:", long_options, &option_index)) != -1) {
        switch (opt) {
            case 'h':
                print_usage(argv[0]);
                exit(0);
            case 'v':
                *verbose = 1;
                break;
            case 'a':
                *pci_address = optarg;
                break;
            case 'r':
                *ring_size = atoi(optarg);
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

int validate_args(const char *pci_address, int ring_size, int num_frames, int duration) {
    if (pci_address == NULL || strlen(pci_address) == 0) {
        fprintf(stderr, "Invalid argument: pci_address must be provided with -a/--pci_address.\n");
        return 0;
    }

    if (ring_size < 1024 || ring_size > 131072) {
        fprintf(stderr, "Invalid argument: ring_size must be between 1024 and 131072 (inclusive).\n");
        return 0;
    }

    if (num_frames < 1024 || num_frames > 131072) {
        fprintf(stderr, "Invalid argument: num_frames must be between 1024 and 131072 (inclusive).\n");
        return 0;
    }

    if (duration <= 0 || duration > 360) {
        fprintf(stderr, "Invalid argument: duration must be between 1 and 360 seconds (inclusive).\n");
        return 0;
    }

    return 1;
}

int read_packets_lcore(void *arg){
    struct lcore_args *largs = (struct lcore_args *)arg;
    uint16_t lcore_id = rte_lcore_id();
    printf("Lcore %u: Starting packet reading routine on queue %u\n", lcore_id, largs->queue_id);
    
    struct rte_mbuf *rx_packets[BURST_SIZE];
    uint16_t rx_count = 0;

    while (!exit_indicator) {
        rx_count = rte_eth_rx_burst(largs->port_id, largs->queue_id, rx_packets, BURST_SIZE);
        if (rx_count == 0) 
            continue;

        for (uint16_t i = 0; i < rx_count; i++)
            largs->total_bytes += (uint64_t)rx_packets[i]->data_len;

        largs->total_pkts += (uint64_t)rx_count;
        rte_pktmbuf_free_bulk(rx_packets, rx_count);
    }
    
    printf("Lcore %u: Exiting packet reading routine\n", lcore_id);
    return 0;
}



int main(int argc, char **argv){
    signal(SIGINT, handle_signal);
    signal(SIGTERM, handle_signal);

    int32_t return_val = rte_eal_init(argc, argv);
    if (return_val < 0) {
        fprintf(stderr, "Unable to initialize DPDK EAL (Environment Abstraction Layer). Error code: %s\n", rte_strerror(rte_errno));
        exit(1);
    }

    // rte_eal_init() DPDK API will return the number of DPDK EAL arguments processed (including the program name).
    // We need to skip these arguments and point to the first application argument.
    // For example: ./<dpdk_application> --lcores=0 -n 4 -- -s 1 -t 2
    // rte_eal_init() will process and return 5. We then skip to the '--' and beyond for application options.
    argc -= return_val;
    argv += return_val;

    char *pci_address = NULL;
    int verbose = 0;
    int ring_size = 1024, num_frames = 8192, duration = 10;

    if (argc > 0) 
        parse_app_args(argc, argv, &pci_address, &verbose, &ring_size, &num_frames, &duration);

    if (!validate_args(pci_address, ring_size, num_frames, duration)) {
        print_usage(argv[0]);
        rte_eal_cleanup();
        return EXIT_FAILURE;
    }

    // Detecting the logical cores (CPUs) ids passed to this DPDK application. 
    uint16_t worker_lcores[MAX_LCORES];
    uint16_t num_worker_lcores = 0;

    printf("\n------------------- APPLICATION INFORMATION --------------------\n");
    printf("Worker logical cores ids (CPU ids): ");
    int i;
    RTE_LCORE_FOREACH_WORKER(i) {
        worker_lcores[num_worker_lcores] = i;
        num_worker_lcores++;
        printf("%u ", i);
    }
    printf("\n");
    if (num_worker_lcores < 1) {
        fprintf(stderr, "At least one worker logical core is required to run this DPDK application.\n");
        return EXIT_FAILURE;
    }

    uint16_t port_id_to_use = 0;
    bool port_found = false;
    int16_t id = 0;
    int16_t total_port_count = 0;
    
    // Detecting the available ports (ethernet interfaces) in the system.
    RTE_ETH_FOREACH_DEV(id) {
        total_port_count++;
        if (total_port_count >= RTE_MAX_ETHPORTS){
            fprintf(stderr, "Total number of detected ports exceeds RTE_MAX_ETHPORTS.\n");
            rte_eal_cleanup();
            return EXIT_FAILURE;
        }

        char name[RTE_ETH_NAME_MAX_LEN];
        if (rte_eth_dev_get_name_by_port(id, name) == 0) {
            if (pci_address && strcmp(pci_address, name) == 0) {
                printf("Selected dev %s found with port id: %u\n", name, id);
                port_id_to_use = id;
                port_found = true;
                break;
            }
        }
    }

    if (!port_found) {
        fprintf(stderr, "Desired port not detected in the system.\n");
        rte_eal_cleanup();
        return EXIT_FAILURE;
    }

    // Create memory pools - one per lcore
    struct rte_mempool *mempools[MAX_LCORES];
    for (int i = 0; i < num_worker_lcores; i++) {
        uint16_t lcore_id = worker_lcores[i];
        char pool_name[32];
        snprintf(pool_name, sizeof(pool_name), "mp_lcore_%u", lcore_id);
        mempools[lcore_id] = rte_pktmbuf_pool_create(pool_name, num_frames, MBUF_CACHE_SIZE, 0, RTE_MBUF_DEFAULT_BUF_SIZE, rte_socket_id());
        if (mempools[lcore_id] == NULL) {
            fprintf(stderr, "Unable to create mempool for lcore %u\n", lcore_id);
            rte_eal_cleanup();
            return EXIT_FAILURE;
        }
    }

    // Configuring the port (ethernet interface). An ethernet interface can have multiple receive queues and transmit queues. 
    const uint16_t tx_queues = 1; // One TX queue for the port, or an error occurs
    const uint16_t rx_queues = num_worker_lcores;
    struct rte_eth_conf port_conf = {
        .rxmode = {.mq_mode = RTE_ETH_MQ_RX_NONE},
        .txmode = {.mq_mode = RTE_ETH_MQ_TX_NONE}
    };

    if ((return_val = rte_eth_dev_configure(port_id_to_use, rx_queues, tx_queues, &port_conf)) != 0) {
        fprintf(stderr, "Unable to configure port. port Id: %u Return code: %d\n", port_id_to_use, return_val);
        rte_eal_cleanup();
        return EXIT_FAILURE;
    }

    // Configuration (port_id, mempool, counters) per lcore
    struct lcore_args lcore_args_array[MAX_LCORES];
    for (int i = 0; i < num_worker_lcores; i++) {
        uint16_t lcore_id = worker_lcores[i];

        lcore_args_array[i].port_id = port_id_to_use;
        lcore_args_array[i].queue_id = i;
        lcore_args_array[i].mempool = mempools[lcore_id];
        lcore_args_array[i].total_pkts = 0;
        lcore_args_array[i].total_bytes = 0;
    }

    const int16_t socket_id = rte_eth_dev_socket_id(port_id_to_use);
    // Configure the queue(s) of the port - each queue uses the mempool of its corresponding lcore
    for (uint16_t i = 0; i < num_worker_lcores; i++) {
        uint16_t lcore_id = worker_lcores[i];
        return_val = rte_eth_rx_queue_setup(port_id_to_use, i, ring_size, socket_id, NULL, mempools[lcore_id]);
        if (return_val < 0) {
            fprintf(stderr, "Unable to setup RX queue %u Port Id: %u Return code: %d\n", i, port_id_to_use, return_val);
            rte_eal_cleanup();
            exit(1);
        }

        printf("Port Id: %u Rx Queue: %u setup successful. Socket id: %d Mempool: lcore_%u\n", port_id_to_use, i, socket_id, lcore_id);
    }
    
    // Try to enable promiscuous mode on the port.
    return_val = rte_eth_promiscuous_enable(port_id_to_use);
    if (return_val < 0) 
        printf("Warning: Unable to set the promiscuous mode for port Id: %u Return code: %d Ignoring ...\n", port_id_to_use, return_val);

    return_val = rte_eth_dev_start(port_id_to_use);
    if (return_val < 0) {
        fprintf(stderr, "Unable to start port Id: %u Return code: %d\n", port_id_to_use, return_val);
        rte_eal_cleanup();
        return EXIT_FAILURE;
    }
    
    printf("Port Id %u configuration successful. Launching packet processing on worker lcores...\n", port_id_to_use);
    for (int i = 0; i < num_worker_lcores; i++) {
        uint16_t lcore_id = worker_lcores[i];
        int ret = rte_eal_remote_launch(read_packets_lcore, &lcore_args_array[i], lcore_id);
        if (ret != 0) {
            fprintf(stderr, "Unable to launch packet processing on lcore %u\n", lcore_id);
            rte_eal_cleanup();
            return EXIT_FAILURE;
        }
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

    // Wait for all lcores to complete
    printf("%ld seconds elapsed, sending stop signal and waiting for all worker lcores to complete...\n", (current.tv_sec - start.tv_sec));
    rte_eal_mp_wait_lcore();

    uint64_t total_packets = 0;
    uint64_t total_bytes = 0;
    for (int i = 0; i < num_worker_lcores; i++) {
        total_packets += lcore_args_array[i].total_pkts;
        total_bytes += lcore_args_array[i].total_bytes;
        printf("Lcore %d: Total packets: %lu, total bytes: %lu\n", worker_lcores[i], lcore_args_array[i].total_pkts, lcore_args_array[i].total_bytes);
    }

    printf("Total packets received: %lu, total bytes %lu\n", total_packets, total_bytes);
    printf("Exiting DPDK program ...\n");
    rte_eth_dev_stop(port_id_to_use);
    rte_eth_dev_close(port_id_to_use);
    rte_eal_cleanup();

    return EXIT_SUCCESS;
}
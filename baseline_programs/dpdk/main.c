#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <signal.h>
#include <stdbool.h>

#include <getopt.h>
#include <rte_eal.h>
#include <rte_errno.h>
#include <rte_ethdev.h>
#include <rte_mbuf.h>

#define MBUF_CACHE_SIZE 512


#define BURST_SIZE 32


#define RX_RING_SIZE 1024 // Size of the RX ring buffer for each queue


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
    printf("  -p, --packet_slots       Specify the number of packet slots in the memory pool\n");
    printf("  -q, --rx_queue          Specify the RX queue to be used\n");
    printf("\nExample: %s --lcores=0,1 -n 4 -- -v -a 0000:01:00.0 -p 1024\n", prog_name);
}

int parse_app_args(int argc, char **argv, char **pci_address, int *verbose, int *packet_slots, int *rx_queue){
    int opt;
    struct option long_options[] = {
        {"help",    no_argument,       0, 'h'},
        {"verbose", no_argument,       0, 'v'},
        {"pci_address",    required_argument, 0, 'a'},
        {"packet_slots",    required_argument, 0, 'p'},
        {"rx_queue",    required_argument, 0, 'q'},
        {0, 0, 0, 0}
    };

    int option_index = 0;
    while ((opt = getopt_long(argc, argv, "hvap:q:", long_options, &option_index)) != -1) {
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
            case 'p':
                *packet_slots = atoi(optarg);
                break;
            case 'q':
                *rx_queue = atoi(optarg);
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

void read_packets_from_interface(void *param, uint64_t *total_pkts, uint64_t *total_bytes){
    uint16_t *port_id = (uint16_t *)(param);
    printf("Starting packet reading routine. Logical core id (CPU id): %u\n", rte_lcore_id());    
    struct rte_mbuf *rx_packets[BURST_SIZE];
    uint16_t rx_count = 0;

    while (!exit_indicator) {
        // Read the packets from interface. We read 32 packets at max at time.
        rx_count = rte_eth_rx_burst(*port_id, 0, rx_packets, BURST_SIZE);

        if (rx_count == 0) 
            continue;

        // Packets received. Enqueue the packets in the ring buffer. Packet processing thread will read them from the ring buffer.
        for (uint16_t i = 0; i < rx_count; i++)
            *total_bytes += (uint64_t)rx_packets[i]->data_len;

        *total_pkts += (uint64_t)rx_count;
        rte_pktmbuf_free_bulk(rx_packets, rx_count);
    }
    
    printf("Exiting packet reading routine.\n");
    return;
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
    int packet_slots = 1024; // 1024; // default value
    int rx_queues = 1; // default value
    if (argc > 0) 
        parse_app_args(argc, argv, &pci_address, &verbose, &packet_slots, &rx_queues);

    // Detecting the logical cores (CPUs) ids passed to this DPDK application. 
    uint16_t i = 0;
    uint16_t logical_cores[RTE_MAX_LCORE];
    uint16_t num_logical_cores = 0;
    printf("Logical cores ids (CPU ids): ");
    RTE_LCORE_FOREACH(i) {
        logical_cores[num_logical_cores] = i;
        num_logical_cores++;
        printf("%u ", i);
    }
    printf("\n");

    if (num_logical_cores < 2) {
        fprintf(stderr, "Atleast two logical cores are required to run this DPDK application.\n");
        exit(1);
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
            exit(1);
        }

        char name[RTE_ETH_NAME_MAX_LEN];
        if (rte_eth_dev_get_name_by_port(id, name) == 0) {
            printf("Port ID %u: %s\n", id, name);
        }

        if (pci_address && strcmp(pci_address, name) == 0) {
            printf("Selected dev %s found with port id: %u\n", name, id);
            port_id_to_use = id;
            port_found = true;
            break;
        }
    }

    if (!port_found) {
        fprintf(stderr, "Desired port not detected in the system.\n");
        rte_eal_cleanup();
        return 1;
    }
    printf("Total ports detected: %d\n", total_port_count);

    // Creating memory pool which contains the memory buffers. A memory buffer is the buffer where DPDK driver will write an incoming packet. 
    struct rte_mempool *memory_pool = rte_pktmbuf_pool_create("mp", packet_slots, MBUF_CACHE_SIZE, 0, RTE_MBUF_DEFAULT_BUF_SIZE, rte_socket_id());

    // Configuring the port (ethernet interface). An ethernet interface can have multiple receive queues and transmit queues. 
    const uint16_t tx_queues = 1;
    struct rte_eth_conf port_conf = {
        .rxmode = {
            .mq_mode = RTE_ETH_MQ_RX_NONE
        },
        .txmode = {
            .mq_mode = RTE_ETH_MQ_TX_NONE
        }
    };

    if ((return_val = rte_eth_dev_configure(port_id_to_use, rx_queues, tx_queues, &port_conf)) != 0) {
        fprintf(stderr, "Unable to configure port. port Id: %u Return code: %d\n", port_id_to_use, return_val);
        rte_eal_cleanup();
        exit(1);
    }

    const int16_t port_socket_id = rte_eth_dev_socket_id(port_id_to_use);
    const int16_t core_socket_id = rte_socket_id();

    fprintf(stdout, "Port socket id: %d Core socket id: %d\n", port_socket_id, core_socket_id);

    // Configure the queue(s) of the port.
    for (uint16_t i = 0; i < rx_queues; i++) {
        return_val = rte_eth_rx_queue_setup(port_id_to_use, i, RX_RING_SIZE, ((port_socket_id >= 0) ? port_socket_id : core_socket_id), NULL, memory_pool);
        if (return_val < 0) {
            fprintf(stderr, "Unable to setup RX queue %u Port Id: %u Return code: %d\n", i, port_id_to_use, return_val);
            rte_eal_cleanup();
            exit(1);
        }

        printf("Port Id: %u Rx Queue: %u setup successful. Socket id: %d\n", port_id_to_use, i, ((port_socket_id >= 0) ? port_socket_id : core_socket_id));
    }

    // Try to enable promiscuous mode on the port.
    return_val = rte_eth_promiscuous_enable(port_id_to_use);
    if (return_val < 0) {
        printf("Warning: Unable to set the promiscuous mode for port Id: %u Return code: %d Ignoring ...\n", port_id_to_use, return_val);
    }

    return_val = rte_eth_dev_start(port_id_to_use);
    if (return_val < 0) {
        printf("Unable to start port Id: %u Return code: %d\n", port_id_to_use, return_val);
        rte_eal_cleanup();
        exit(1);
    }

    printf("Port configuration successful. Port Id: %u\n", port_id_to_use);
    uint64_t total_packets = 0;
    uint64_t total_bytes = 0;
    read_packets_from_interface((void *)(&port_id_to_use), &total_packets, &total_bytes);
    fprintf(stdout, "Total packets received: %lu, total bytes %lu\n", total_packets, total_bytes);

    printf("Exiting DPDK program ...\n");
    rte_eth_dev_stop(port_id_to_use);
    rte_eth_dev_close(port_id_to_use);
    rte_eal_cleanup();
    return 0;
}
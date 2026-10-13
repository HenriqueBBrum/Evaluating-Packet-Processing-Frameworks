#!/bin/bash


# Packet capture tools:
# AF_PACKET
# AF_XDP
# PF_RING
# DPDK
# NETMAP

# Experiments included: 
# Single flow, multiple speeds, multiple protocols, multiple packet sizes
# Multiple flows with fixed length. multiple speeds, multiple protocols, multiple packet sizes
# Multiple flows with realistic flow lengths, multiple speeds (?)
# Microbursts

# Metrics measured:
# Packet loss
# CPU usage
# Latency ?


# Conditions:
# Same number of threads/cores, increasing
# Same number of buffers (or similar), increasing
# Same buffer size (or similar), increasing
# Polling strategy



# Input:
# Interface
# Output folder
# Loops (optional)

if [ "$#" -ne 4 ]; then
    echo "Usage: $0 <interface> <loops> <configuration_test> <output_folder>" 
    echo "  interface: network interface name to capture packets from"
    echo "  loops: number of loops to run (default: 1)"
    echo "  configuration test (y or n)"
    echo "  output folder name: folder name within the source folder where the experiment results will be stored"
    exit 1
fi

interface=$1
loops=${2:-1}
configuration_test=$3
output_folder_name=$4
pci_address=$(lshw -class network -businfo 2>/dev/null | awk -v nic="$interface" '$2==nic {gsub("pci@", "", $1); print $1; exit}')
if [ -z "$pci_address" ]; then
    echo "Could not resolve PCIe address for interface '$interface' from: lshw -class network -businfo"
    exit 1
fi
echo "Resolved PCIe address for $interface: $pci_address"

source_folder="$( cd "$( dirname "${BASH_SOURCE[0]}" )" &> /dev/null && pwd )"
output_folder="$source_folder/"output"/$output_folder_name"

# Experiments configuration
time_to_monitor=40
time_to_run=20
start_thread=20

# Trex configuration
tx_host="rising"
tx_ip="10.231.0.101" # Replace with the actual IP address of the traffic generator machine
trex_folder="/opt/trex/v3.08/" # Defaul installation path
trex_experiments_folder="~/packet_capture/" # Crerate a trex_code folder in the home directory of the tx_host and copy the pcaps and scripts to it

# NIC Queues Configuration
rx_ring_size=8192

# AF-PACKET configurations
af_packet_configs=("131072 256" "16384 32" "32768 64" "65536 128")

# PF_RING configurations
pf_ring_configs=(131072 65536)
pf_ring_module_folder="/lib/modules/6.8.0-1047-nvidia/kernel/net/pf_ring/" # Replace with the actual path to the PF_RING module folder

# AF_XDP configurations
af_xdp_configs=("1024" "8192" "32768")

# DPDK configurations
dpdk_configs=("1024" "8192" "32768")


threads=("1 2 4 8 16")
valid_answers=("y" "Y" "yes" "YES")
if [[ " ${valid_answers[@]} " =~ " $configuration_test " ]]; then
    threads=("1")
    traffic_profile=("one_flow")
    speeds=("10" "100" "1000")
    packet_sizes=("1400") #("64" "512" "1400")
    protocol=("udp" "tcp")
    for speed in "${speeds[@]}"; do
        for packet_size in "${packet_sizes[@]}"; do
            for proto in "${protocol[@]}"; do
                experiments_config+=("$speed $packet_size $proto")
            done
        done
    done
fi

strategy=("AF_PACKET") #   "AF_XDP"  "DPDK"  "vPF_RING" "AF_XDP" 

for thread in "${threads[@]}"; do
    for exp_config in "${experiments_config[@]}"; do
        read -r speed packet_size proto <<< "$exp_config"

        for strat in "${strategy[@]}"; do
            printf '%*s\n\n' "${COLUMNS:-$(tput cols)}" '' | tr ' ' '#'
            echo "Strategy: $strat"

            configs=("default")
            if [ "$strat" == "AF_PACKET" ]; then
                configs=("${af_packet_configs[@]}")
            elif [ "$strat" == "AF_XDP" ]; then
                configs=("${af_xdp_configs[@]}")
            elif [ "$strat" == "vPF_RING" ]; then
                configs=("${pf_ring_configs[@]}")
            elif [ "$strat" == "DPDK" ]; then
                configs=("${dpdk_configs[@]}")
                dpdk_thread_list="$start_thread-$((start_thread+$thread))"
            fi

            if [[ "$strat" != "DPDK" ]]; then
                sudo ethtool -K $interface rxhash on ntuple on
                sudo ethtool -C $interface adaptive-rx off adaptive-tx off rx-usecs 0 rx-frames 0
                sudo ethtool -G $interface rx $rx_ring_size
                sudo ethtool -X $interface hfunc toeplitz
                sudo ip link set $interface xdp off
                sudo ip link set $interface promisc on

                sudo ethtool -L $interface combined $thread
                thread_list="$start_thread-$((start_thread+$thread-1))"
                sudo set_irq_affinity_cpulist.sh $thread_list $interface
            fi

            for config in "${configs[@]}"; do
                config_label=$(echo "$config" | tr ' ' '_')
                echo "Running $strat baseline program with $thread threads on interface $interface with $config frames"
                for ((loop=1; loop<=loops; loop++)); do
                    output_folder_final="$output_folder/"$thread"_threads/$strat/config_$config_label/loop_$loop"
                    mkdir -p "$output_folder_final"
                    rm -f "$output_folder_final"/*
                    bash monitoring.sh "$interface" rx "$time_to_monitor" "$output_folder_final" &
                    monitor_pid=$!

                    echo "  Output folder: $output_folder_final"
                    echo "  Loop $loop of $loops"

                    if [ "$strat" == "DPDK" ]; then
                        cd "baseline_programs/dpdk/"
                        make
                        monitored_process="dpdk_pkt_capture"
                        read -r dpdk_num_frames <<< "$config"
                        sudo ./build/dpdk_pkt_capture -l $dpdk_thread_list -- -a "$pci_address" -r 1024 -n $dpdk_num_frames -d $time_to_monitor > "$output_folder_final/packet_counter.txt" &
                    elif [ "$strat" == "AF_XDP" ]; then
                        cd "baseline_programs/af_xdp/"
                        make
                        monitored_process="af_xdp"
                        read -r af_xdp_queue_count <<< "$config"
                        sudo ./build/af_xdp -s $start_thread -i "$interface" -n "$af_xdp_queue_count" -d $time_to_monitor > "$output_folder_final/packet_counter.txt" &
                    elif [ "$strat" == "vPF_RING" ]; then
                        cd "$pf_ring_module_folder"
                        monitored_process="pfcount_multichannel"
                        sudo insmod pf_ring.ko min_num_slots="$config" enable_tx_capture=0
                        sleep 5
                        sudo pfcount_multichannel -i $interface > "$output_folder_final/packet_counter.txt" &
                    elif [ "$strat" == "AF_PACKET" ]; then
                        cd "baseline_programs/af_packet/"
                        make
                        monitored_process="af_packet"
                        read -r block_size block_nr <<< "$config"
                        sudo numactl -C $thread_list ./build/af_packet -t $thread -s $start_thread -i "$interface" -b "$block_size" -n "$block_nr" -d $time_to_monitor > "$output_folder_final/packet_counter.txt" &
                    fi

                    packet_capture_pid=$!
                    if [ -n "$monitored_process" ]; then
                        sudo pidstat -C "$monitored_process" -t 1 > "$output_folder_final/pidstat.log" 2>&1 &
                        pidstat_pid=$!
                    fi
                    sleep 5

                    ssh $tx_host@$tx_ip "mkdir -p $trex_experiments_folder/out/"
                    ssh $tx_host@$tx_ip "rm -f $trex_experiments_folder/out/*"
                    ssh $tx_host@$tx_ip "cd $trex_folder && sudo ./t-rex-64 -i" > "$output_folder_final"/trex_server.log 2>&1 &
                    sleep 5
                    
                    if [[ " ${valid_answers[@]} " =~ " $configuration_test " ]]; then
                        ssh $tx_host@$tx_ip "source ~/trex_venv/bin/activate && python3 $trex_experiments_folder/stl/one_flow.py -d $time_to_run --log_file $trex_experiments_folder/out/trex.log -r $speed --pkt_size $packet_size --protocol $proto"
                        sleep 2
                    fi
                    
                    if [[ "$strat" == "vPF_RING" ]]; then
                        cp /proc/net/pf_ring/*$interface* "$output_folder_final/"
                        chmod a+w "$output_folder_final/"*
                    fi

                    if [[ -n "$packet_capture_pid" ]] && kill -0 "$packet_capture_pid" 2>/dev/null; then
                        kill -s SIGTERM "$packet_capture_pid"
                        wait "$packet_capture_pid"
                    fi
                    if [[ -n "$pidstat_pid" ]] && kill -0 "$pidstat_pid" 2>/dev/null; then
                        kill -s SIGTERM "$pidstat_pid"
                        wait "$pidstat_pid"
                    fi
                    wait $monitor_pid

                    if [[ "$strat" == "vPF_RING" ]]; then
                        sudo rmmod pf_ring
                    fi

                    ssh $tx_host@$tx_ip "sudo pkill -f t-rex-64"
                    scp $tx_host@$tx_ip:"$trex_experiments_folder/out/*" "$output_folder_final"
                    cd $source_folder
                    exit
                done
                printf '%*s\n\n' "${COLUMNS:-$(tput cols)}" '' | tr ' ' '-'
            done
        done
        exit
    done
done
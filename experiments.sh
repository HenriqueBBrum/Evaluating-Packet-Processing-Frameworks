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
# IP of traffic generator machine
# Output folder
# Loops (optional)


if [ "$#" -ne 2 ]; then
    echo "Usage: $0 <interface> <loops>" 
    echo "  interface: network interface name to capture packets from"
    echo "  loops: number of loops to run (default: 1)"
    exit 1
fi

interface=$1
loops=${2:-1}

source_folder="$( cd "$( dirname "${BASH_SOURCE[0]}" )" &> /dev/null && pwd )"
echo "Source folder: $source_folder"
output_folder="$source_folder/output"

# Trex configuration
tx_host="rising"
tx_ip="10.231.0.101" # Replace with the actual IP address of the traffic generator machine
trex_folder="/opt/trex/v3.08/" # Defaul installation path
trex_experiments_folder="~/packet_capture/" # Crerate a trex_code folder in the home directory of the tx_host and copy the pcaps and scripts to it

# RX NIC configuration
rx_nic_cores="9-47"
rx_nic_cores_num=39

# Experiments configuration
time_to_monitor=20
time_to_run=10

# PF_RING
pf_ring_module_folder="/lib/modules/6.8.0-1047-nvidia/kernel/net/pf_ring/" # Replace with the actual path to the PF_RING module folder

threads=("1") # ("1" "2" "4" "8")
traffic_profile=("one_flow")
strategy=("AF_PACKET" "AF_XDP" "vPF_RING" "zcPF_RING" "DPDK" "NETMAP")

sudo ethtool -K $interface rxhash on ntuple on
# sudo ethtool -A $interface rx off
sudo ethtool -C $interface adaptive-rx off adaptive-tx off rx-usecs 0 rx-frames 0
sudo ethtool -G $interface rx 8192
sudo ethtool -X $interface hfunc toeplitz
sudo ip link set $interface xdp off
sudo ip link set $interface promisc on

for thread in "${threads[@]}"; do
    sudo ethtool -L $interface combined $thread
    thread_list="20-$((20+$thread-1))"
    sudo set_irq_affinity_cpulist.sh $thread_list $interface

    for strat in "${strategy[@]}"; do
        if [[ "$strat" != "AF_PACKET" ]] && [[ "$strat" != "vPF_RING" ]]; then
            continue
        fi
        echo "$strat"
        for ((loop=1; loop<=loops; loop++)); do
            echo "  Loop $loop of $loops"
            output_folder_final="$output_folder/"$thread"_threads/$strat/loop_$loop"
            mkdir -p "$output_folder_final"
            echo "  Output folder: $output_folder_final"
            rm "$output_folder_final"/*
            bash monitoring.sh "$interface" rx "$time_to_monitor" "$output_folder_final" &
            monitor_pid=$!

            echo "  Running $strat baseline program with $thread threads on interface $interface"

            if [ "$strat" == "DPDK" ]; then
                sleep 5
            elif [ "$strat" == "AF_XDP" ]; then
                sleep 5
            elif [ "$strat" == "vPF_RING" ]; then
                cd "$pf_ring_module_folder"
                sudo insmod pf_ring.ko min_num_slots=65536 enable_tx_capture=0
                sleep 5
                sudo pfcount_multichannel -i $interface > "$output_folder_final/packet_counter.txt" &
            elif [ "$strat" == "zcPF_RING" ]; then
                sleep 5
            elif [ "$strat" == "NETMAP" ]; then
                sleep 5
            elif [ "$strat" == "AF_PACKET" ]; then
                cd "baseline_programs/af_packet/"
                sudo numactl -C $thread_list  ./af_packet "$thread" "$interface" 65536 128 "$time_to_monitor" > "$output_folder_final/packet_counter.txt" &
            fi

            packet_capture_pid=$!
            sudo pidstat -u -C af_packet 1 > "$output_folder_final/pidstat.log" 2>&1 &

            ssh $tx_host@$tx_ip "mkdir -p $trex_experiments_folder/out/"
            ssh $tx_host@$tx_ip "rm -f $trex_experiments_folder/out/*"
            ssh $tx_host@$tx_ip "cd $trex_folder && sudo ./t-rex-64 -i" > "$output_folder_final"/trex_server.log 2>&1 &
            sleep 5
            ssh $tx_host@$tx_ip "source ~/trex_venv/bin/activate && python3 $trex_experiments_folder/stl/one_flow.py -d $time_to_run --log_file $trex_experiments_folder/out/trex.log -r 10 --pkt_size 1400 --protocol udp"
            sleep 2
            printf '%*s\n\n' "${COLUMNS:-$(tput cols)}" '' | tr ' ' '-'
            if [ "$strat" == "vPF_RING" ]; then
                cp /proc/net/pf_ring/*$interface* "$output_folder_final/"
                chmod a+w "$output_folder_final/"*
            fi

            kill -s SIGTERM $packet_capture_pid
            wait $packet_capture_pid
            wait $monitor_pid

            if [ "$strat" == "vPF_RING" ]; then
                sudo rmmod pf_ring
            fi

            ssh $tx_host@$tx_ip "sudo pkill -f t-rex-64"
            scp $tx_host@$tx_ip:"$trex_experiments_folder/out/*" "$output_folder_final"
            cd $source_folder
            break
        done
    done
done
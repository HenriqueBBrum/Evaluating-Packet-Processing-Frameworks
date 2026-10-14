#!/bin/bash

# Single-config experiment runner.
# Uses baseline capture programs and an alternate TREX flow that retrieves
# the TREX JSON log directly from the remote host (no remote out/ + scp step).

if [ "$#" -ne 3 ]; then
    echo "Usage: $0 <interface> <loops> <output_folder>"
    echo "  interface: network interface name to capture packets from"
    echo "  loops: number of loops to run"
    echo "  output folder name: folder name within output/ where results are stored"
    exit 1
fi

interface=$1
loops=$2
output_folder_name=$3

pci_address=$(lshw -class network -businfo 2>/dev/null | awk -v nic="$interface" '$2==nic {gsub("pci@", "", $1); print $1; exit}')
if [ -z "$pci_address" ]; then
    echo "Could not resolve PCIe address for interface '$interface' from: lshw -class network -businfo"
    exit 1
fi
echo "Resolved PCIe address for $interface: $pci_address"

source_folder="$( cd "$( dirname "${BASH_SOURCE[0]}" )" &> /dev/null && pwd )"
output_folder="$source_folder/../output/$output_folder_name"

# Experiments configuration
time_to_monitor=70
time_to_run=30
start_thread=20

# Trex configuration
tx_host="rising"
tx_ip="10.231.0.101"
trex_folder="/opt/trex/v3.08/"
trex_experiments_folder="~/packet_capture/"

# NIC Queues Configuration
rx_ring_size=8192

# One config per strategy
af_packet_config="32768 2048"
af_xdp_config="8192"
pf_ring_config="65536"
dpdk_config="8192"

pf_ring_module_folder="/lib/modules/6.8.0-1047-nvidia/kernel/net/pf_ring/"

strategy=("AF_PACKET" "DPDK" "vPF_RING" "AF_XDP")
flows_file_configs=("4000000_AGH.txt 35" "4000000_BRAS.txt 10"  "4000000_CICIDS2017_Monday.txt 30")
threads=("1" "2" "4" "8" "16")
speeds=("1" "10" "100")
packet_sizes=("1400" "512" "64")
protocols=("udp" "tcp")

run_trex() {
    local speed="$1"
    local flows_file="$2"
    local output_dir="$3"

    ssh $tx_host@$tx_ip "mkdir -p $trex_experiments_folder/out/"
    ssh $tx_host@$tx_ip "rm -f $trex_experiments_folder/out/*"
    ssh "$tx_host@$tx_ip" "cd $trex_folder && sudo ./t-rex-64 -i" > "$output_dir/trex_server.log" 2>&1 &
    local trex_server_pid=$!
    sleep 3

    ssh $tx_host@$tx_ip "source ~/trex_venv/bin/activate && python3 $trex_experiments_folder/stl/flows_from_file.py -f $flows_file -r $speed -d $time_to_run -l $trex_experiments_folder/out/trex.log"
    
    ssh $tx_host@$tx_ip "sudo pkill -f t-rex-64"
    scp $tx_host@$tx_ip:"$trex_experiments_folder/out/*" "$output_dir"
    if kill -0 "$trex_server_pid" 2>/dev/null; then
        wait "$trex_server_pid" 2>/dev/null || true
    fi
}

for flows_file_config in "${flows_file_configs[@]}"; do
    read -r flows_file speed <<< "$flows_file_config"
    for thread in "${threads[@]}"; do
        for strat in "${strategy[@]}"; do
            printf '%*s\n\n' "${COLUMNS:-$(tput cols)}" '' | tr ' ' '#'
            echo "Strategy: $strat"

            if [[ "$strat" == "DPDK" ]]; then
                config="$dpdk_config"
                dpdk_thread_list="$start_thread-$((start_thread+thread))"
            elif [[ "$strat" == "AF_XDP" ]]; then
                config="$af_xdp_config"
            elif [[ "$strat" == "vPF_RING" ]]; then
                config="$pf_ring_config"
            else
                config="$af_packet_config"
            fi

            if [[ "$strat" != "DPDK" ]]; then
                sudo ethtool -K "$interface" rxhash on ntuple on
                sudo ethtool -C "$interface" adaptive-rx off adaptive-tx off rx-usecs 0 rx-frames 0
                sudo ethtool -G "$interface" rx "$rx_ring_size"
                sudo ethtool -X "$interface" hfunc toeplitz
                sudo ip link set "$interface" xdp off
                sudo ip link set "$interface" promisc on
            fi

            sudo ethtool -L "$interface" combined "$thread"
            thread_list="$start_thread-$((start_thread+thread-1))"
            sudo set_irq_affinity_cpulist.sh "$thread_list" "$interface"

            config_label=$(echo "$config" | tr ' ' '_')
            echo "Running $strat baseline program with $thread threads on interface $interface with config $config"

            for ((loop=1; loop<=loops; loop++)); do
                output_folder_final="$output_folder/${thread}_threads/${speed}Gbps_${flows_file}/$strat/config_${config_label}/loop_${loop}"
                mkdir -p "$output_folder_final"
                rm -f "$output_folder_final"/*

                bash "$source_folder/monitoring.sh" "$interface" rx "$time_to_monitor" "$output_folder_final" &
                monitor_pid=$!

                echo "  Output folder: $output_folder_final"
                echo "  Loop $loop of $loops"

                monitored_process=""
                if [[ "$strat" == "DPDK" ]]; then
                    cd "$source_folder/../baseline_programs/dpdk/" || exit 1
                    make > "$output_folder_final/make.log" 2>&1
                    monitored_process="dpdk_pkt_capture"
                    sudo numactl -C "$thread_list" ./build/dpdk_pkt_capture -l "$dpdk_thread_list" -- -a "$pci_address" -r 1024 -n "$config" -d "$time_to_monitor" > "$output_folder_final/packet_counter.txt" 2>&1 &
                elif [[ "$strat" == "AF_XDP" ]]; then
                    cd "$source_folder/../baseline_programs/af_xdp/" || exit 1
                    make > "$output_folder_final/make.log" 2>&1
                    monitored_process="af_xdp"
                    sudo numactl -C "$thread_list" ./build/af_xdp -s "$start_thread" -i "$interface" -n "$config" -d "$time_to_monitor" > "$output_folder_final/packet_counter.txt" 2>&1 &
                elif [[ "$strat" == "vPF_RING" ]]; then
                    cd "$pf_ring_module_folder" || exit 1
                    monitored_process="pfcount_multichannel"
                    sudo insmod pf_ring.ko min_num_slots="$config" enable_tx_capture=0 > "$output_folder_final/pf_ring_insmod.log" 2>&1
                    sleep 3
                    sudo numactl -C "$thread_list" pfcount_multichannel -i "$interface" > "$output_folder_final/packet_counter.txt" 2>&1 &
                else
                    cd "$source_folder/../baseline_programs/af_packet/" || exit 1
                    make > "$output_folder_final/make.log" 2>&1
                    monitored_process="af_packet"
                    read -r block_size block_nr <<< "$config"
                    sudo numactl -C "$thread_list" ./build/af_packet -t "$thread" -s "$start_thread" -i "$interface" -b "$block_size" -n "$block_nr" -d "$time_to_monitor" > "$output_folder_final/packet_counter.txt" 2>&1 &
                fi

                packet_capture_pid=$!
                pidstat_pid=""
                if [[ -n "$monitored_process" ]]; then
                    monitored_process_pidstat="${monitored_process:0:15}"
                    sudo pidstat -C "$monitored_process_pidstat" -t 1 > "$output_folder_final/pidstat.log" 2>&1 &
                    pidstat_pid=$!
                fi

                run_trex "$speed" "$flows_file" "$output_folder_final"

                if [[ "$strat" == "vPF_RING" ]]; then
                    cp /proc/net/pf_ring/*"$interface"* "$output_folder_final/" 2>/dev/null || true
                    chmod a+w "$output_folder_final/"* 2>/dev/null || true
                fi

                if [[ -n "$packet_capture_pid" ]] && kill -0 "$packet_capture_pid" 2>/dev/null; then
                    kill -s SIGTERM "$packet_capture_pid"
                    wait "$packet_capture_pid" 2>/dev/null || true
                fi
                if [[ -n "$pidstat_pid" ]] && kill -0 "$pidstat_pid" 2>/dev/null; then
                    kill -s SIGTERM "$pidstat_pid"
                    wait "$pidstat_pid" 2>/dev/null || true
                fi
                wait "$monitor_pid" 2>/dev/null || true

                if [[ "$strat" == "vPF_RING" ]]; then
                    sudo rmmod pf_ring > "$output_folder_final/pf_ring_rmmod.log" 2>&1 || true
                fi

                cd "$source_folder" || exit 1
            done

            printf '%*s\n\n' "${COLUMNS:-$(tput cols)}" '' | tr ' ' '-'
        done
    done
done

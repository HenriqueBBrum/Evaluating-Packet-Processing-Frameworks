#!/bin/bash

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
suricata_config="$source_folder/../suricata/suricata.yaml"
ruleset=$source_folder/../suricata/emerging-all.rules
pf_ring_module_folder="/lib/modules/6.8.0-1047-nvidia/kernel/net/pf_ring/" # Replace with the actual path to the PF_RING module folder

if [[ ! -f "$suricata_config" ]] || [[ ! -f "$ruleset" ]]; then
    echo "Suricata config or ruleset not found at $suricata_config and $ruleset"
    exit 1
fi

sed -i "/af-packet:/,/cluster-id/ s/interface: .*/interface: $interface/" $suricata_config
sed -i "/af-xdp:/,/force-xdp-mode:/ s/interface: .*/interface: $interface/" $suricata_config
sed -i "/pfring:/,/cluster-id/ s/interface: .*/interface: $interface/" $suricata_config
sed -i "/dpdk:/,/promisc/ s/interface: .*/interface: $pci_address/" $suricata_config

# Experiments configuration
time_to_monitor=70
time_to_run=30
start_thread=20

# Trex configuration
tx_host="rising"
tx_ip="10.231.0.101"
trex_folder="/opt/trex/v3.08/"
trex_experiments_folder="~/packet_capture/"

# NIC queues configuration
rx_ring_size=8192

strategy=("DPDK" "vPF_RING" ) # "AF_PACKET"  "AF_XDP"
threads=("16") # ("8" "16")
flows_file_configs=("4000000_AGH.txt 1" "4000000_BRAS.txt 10"  "4000000_CICIDS2017_Monday.txt 30")

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
        suricata_thread_range="$start_thread-$((start_thread + thread - 1))"

        sed -i "/af-packet:/,/cluster-id/ s/threads: .*/threads: $thread /" $suricata_config
        sed -i "/pfring:/,/cluster-id/ s/threads: .*/threads: $thread /" $suricata_config
        sed -i "/af-xdp:/,/cluster-id/ s/threads: .*/threads: $thread /" $suricata_config
        sed -i "/dpdk:/,/promisc/ s/threads: .*/threads: $thread /" $suricata_config

        sed -i "/management-cpu-set:/,/mode/ s/cpu: .*/cpu: [ $start_thread ]/" $suricata_config
        sed -i "/worker-cpu-set:/,/mode/ s/cpu: .*/cpu: [ $suricata_thread_range ]/" $suricata_config
            
        for strat in "${strategy[@]}"; do
            printf '%*s\n\n' "${COLUMNS:-$(tput cols)}" '' | tr ' ' '#'
            echo "Strategy: $strat"

            if [[ "$strat" == "AF_PACKET" ]]; then
                suricata_flag="--af-packet=$interface"
            elif [[ "$strat" == "AF_XDP" ]]; then
                suricata_flag="--af-xdp=$interface"
            elif [[ "$strat" == "vPF_RING" ]]; then
                suricata_flag="--pfring-int=$interface"
            else
                suricata_flag="--dpdk"
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
            sudo set_irq_affinity_cpulist.sh "$suricata_thread_range" "$interface"

            echo "Running Suricata for strategy $strat with $thread threads on interface $interface"

            for ((loop=1; loop<=loops; loop++)); do
                output_folder_final="$output_folder/${thread}_threads/${speed}Gbps_${flows_file}/$strat/loop_${loop}"
                mkdir -p "$output_folder_final"
                rm -f "$output_folder_final"/*

                bash "$source_folder/monitoring.sh" "$interface" rx "$time_to_monitor" "$output_folder_final" &
                monitor_pid=$!

                echo "  Output folder: $output_folder_final"
                echo "  Loop $loop of $loops"

                # Suricata logs and stats are written to the current output folder.
                if [ "$strat" == "vPF_RING" ]; then
                    cd "$pf_ring_module_folder"
                    sudo insmod pf_ring.ko min_num_slots="$config" enable_tx_capture=0
                    sleep 3
                    cd "$source_folder"
                fi
                sudo numactl -C "$suricata_thread_range" suricata -vvvv -c "$suricata_config" -S $ruleset -l "$output_folder_final" "$suricata_flag" -D

                sudo pidstat -C "suricata" -t 1 > "$output_folder_final/pidstat.log" 2>&1 &
                pidstat_pid=$!

                sleep 3
                run_trex "$speed" "$flows_file" "$output_folder_final"

                if [[ "$strat" == "vPF_RING" ]]; then
                    cp /proc/net/pf_ring/*$interface* "$output_folder_final/"
                    chmod a+w "$output_folder_final/"*
                fi

                if [[ -f /usr/local/var/run/suricata.pid ]]; then
                    echo "Stopping Suricata..."
                    sudo kill -INT $(cat /usr/local/var/run/suricata.pid)
                    suricata_pid=$(cat /usr/local/var/run/suricata.pid)
                    while [ -e /proc/$suricata_pid ]
                    do
                        echo "Suricata: $suricata_pid is still running"
                        sleep 2
                    done
                    echo "Suricata $suricata_pid has finished"
                    if [[ -f /usr/local/var/run/suricata.pid ]]; then
                        sudo rm /usr/local/var/run/suricata.pid
                    fi
                fi

                if [[ -n "$pidstat_pid" ]] && kill -0 "$pidstat_pid" 2>/dev/null; then
                    kill -s SIGTERM "$pidstat_pid"
                    wait "$pidstat_pid" 2>/dev/null || true
                fi
                wait "$monitor_pid" 2>/dev/null || true

                if [[ "$strat" == "vPF_RING" ]]; then
                    sudo rmmod pf_ring
                fi

                cd "$source_folder" || exit 1
            done
            printf '%*s\n\n' "${COLUMNS:-$(tput cols)}" '' | tr ' ' '-'
        done
        exit
    done
    exit
done
#!/bin/bash

# Single-config experiment runner.
# Uses Suricata per strategy flag and the same TREX flow as single_config_trex_alt.sh.

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

source_folder="$( cd "$( dirname "${BASH_SOURCE[0]}" )" &> /dev/null && pwd )"
output_folder="$source_folder/output/$output_folder_name"
suricata_cfg="$source_folder/suricata/suricata.yaml"

if [ ! -f "$suricata_cfg" ]; then
    echo "Suricata config not found at $suricata_cfg"
    exit 1
fi

# Experiments configuration
time_to_monitor=35
time_to_run=20
start_thread=20

# Trex configuration
tx_host="rising"
tx_ip="10.231.0.101"
trex_folder="/opt/trex/v3.08/"
trex_client_script="~/packet_capture/stl/one_flow.py"

# NIC Queues Configuration
rx_ring_size=8192

# One config label per strategy (for output directory only)
af_packet_config_label="af_packet_default"
af_xdp_config_label="af_xdp_default"
pf_ring_config_label="pfring_default"
dpdk_config_label="dpdk_default"

strategy=("AF_PACKET" "DPDK" "vPF_RING" "AF_XDP")
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

    ssh $tx_host@$tx_ip "source ~/trex_venv/bin/activate && python3 $trex_experiments_folder/flows_from_file.py -f $flows_file -r $speed -d $time_to_run -l $trex_experiments_folder/out/trex.log"
    
    ssh $tx_host@$tx_ip "sudo pkill -f t-rex-64"
    scp $tx_host@$tx_ip:"$trex_experiments_folder/out/*" "$output_dir"
    if kill -0 "$trex_server_pid" 2>/dev/null; then
        wait "$trex_server_pid" 2>/dev/null || true
    fi
}

for thread in "${threads[@]}"; do
    for speed in "${speeds[@]}"; do
        for packet_size in "${packet_sizes[@]}"; do
            for proto in "${protocols[@]}"; do
                for strat in "${strategy[@]}"; do
                    printf '%*s\n\n' "${COLUMNS:-$(tput cols)}" '' | tr ' ' '#'
                    echo "Strategy: $strat"

                    if [[ "$strat" == "AF_PACKET" ]]; then
                        config_label="$af_packet_config_label"
                        suricata_flag="--af-packet=$interface"
                    elif [[ "$strat" == "AF_XDP" ]]; then
                        config_label="$af_xdp_config_label"
                        suricata_flag="--af-xdp=$interface"
                    elif [[ "$strat" == "vPF_RING" ]]; then
                        config_label="$pf_ring_config_label"
                        suricata_flag="--pfring-int=$interface"
                    else
                        config_label="$dpdk_config_label"
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
                    thread_list="$start_thread-$((start_thread+thread-1))"
                    sudo set_irq_affinity_cpulist.sh "$thread_list" "$interface"

                    echo "Running Suricata for strategy $strat with $thread threads on interface $interface"

                    for ((loop=1; loop<=loops; loop++)); do
                        output_folder_final="$output_folder/${thread}_threads/${speed}Gbps_${packet_size}bytes_${proto}/$strat/config_${config_label}/loop_${loop}"
                        mkdir -p "$output_folder_final"
                        rm -f "$output_folder_final"/*

                        bash "$source_folder/monitoring.sh" "$interface" rx "$time_to_monitor" "$output_folder_final" &
                        monitor_pid=$!

                        echo "  Output folder: $output_folder_final"
                        echo "  Loop $loop of $loops"

                        # Suricata logs and stats are written to the current output folder.
                        sudo numactl -C "$thread_list" timeout --signal=SIGINT "${time_to_monitor}s" suricata -c "$suricata_cfg" -l "$output_folder_final" "$suricata_flag" > "$output_folder_final/suricata_stdout.log" 2>&1 &
                        packet_capture_pid=$!

                        pidstat_pid=""
                        monitored_process="suricata"
                        monitored_process_pidstat="${monitored_process:0:15}"
                        sudo pidstat -C "$monitored_process_pidstat" -t 1 > "$output_folder_final/pidstat.log" 2>&1 &
                        pidstat_pid=$!

                        sleep 3
                        run_trex_flow "$speed" "$packet_size" "$proto" "$output_folder_final"

                        if [[ -n "$packet_capture_pid" ]] && kill -0 "$packet_capture_pid" 2>/dev/null; then
                            kill -s SIGINT "$packet_capture_pid"
                            wait "$packet_capture_pid" 2>/dev/null || true
                        fi
                        if [[ -n "$pidstat_pid" ]] && kill -0 "$pidstat_pid" 2>/dev/null; then
                            kill -s SIGTERM "$pidstat_pid"
                            wait "$pidstat_pid" 2>/dev/null || true
                        fi
                        wait "$monitor_pid" 2>/dev/null || true

                        cd "$source_folder" || exit 1
                    done

                    printf '%*s\n\n' "${COLUMNS:-$(tput cols)}" '' | tr ' ' '-'
                done
            done
        done
    done
done

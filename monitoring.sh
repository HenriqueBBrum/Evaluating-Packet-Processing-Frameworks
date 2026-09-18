#!/bin/bash

set -u

usage() {
    echo "Usage:"
    echo "  $0 <interface> <rx_or_tx> <time_to_run> <output_folder>"
    exit 1
}

read_stats() {
    local iface="$1"
    local bytes_key="$2"
    local packets_key="$3"
    local ethtool_output
    ethtool_output=$(sudo ethtool -S "$iface")
    bytes_val=$(echo "$ethtool_output" | grep -F " ${bytes_key}:" | grep -o '[0-9]\+')
    packets_val=$(echo "$ethtool_output" | grep -F " ${packets_key}:" | grep -o '[0-9]\+')
}

read_extra_stats() {
    local iface="$1"
    local ethtool_output
    ethtool_output=$(sudo ethtool -S "$iface")
    out_of_buffer_val=$(echo "$ethtool_output" | grep -F " rx_out_of_buffer:" | grep -o '[0-9]\+')
    packets_phy_val=$(echo "$ethtool_output" | grep -F " rx_packets_phy:" | grep -o '[0-9]\+')
    discards_phy_val=$(echo "$ethtool_output" | grep -F " rx_discards_phy:" | grep -o '[0-9]\+')
}

if [[ $# -ne 4 ]]; then
    usage
fi

interface="$1"
rx_or_tx="$2"
time_to_run="$3"
output_folder="$4"

if ! [[ "$time_to_run" =~ ^[0-9]+$ ]] || [[ "$time_to_run" -le 0 ]]; then
    echo "Error: <time_to_run> must be a positive integer."
    exit 1
fi

time_interval=1
mkdir -p "$output_folder"

out_file="$output_folder/throughput.log"

: > "$out_file"
echo "$interface" >> "$out_file"
echo "bytes, packets processed" >> "$out_file"

read_stats "$interface" "${rx_or_tx}_bytes" "${rx_or_tx}_packets"
bytes_initial=$bytes_val
packets_initial=$packets_val

bytes_initial_temp=$bytes_val
packets_initial_temp=$packets_val

read_extra_stats "$interface"
out_of_buffer_initial=$out_of_buffer_val
packets_phy_initial=$packets_phy_val
discards_phy_initial=$discards_phy_val

bytes_processed=0
packets_processed=0

reports_with_data=0

sleep $time_interval
end=$((SECONDS + time_to_run))

echoes_sent=0
while [[ $SECONDS -lt $end ]]; do
    sleep $time_interval &
    sleep_pid=$!

    read_stats "$interface" "${rx_or_tx}_bytes" "${rx_or_tx}_packets"
    bytes_current=$bytes_val
    packets_current=$packets_val

    bytes_interval=$((bytes_current - bytes_initial_temp))
    packets_interval=$((packets_current - packets_initial_temp))

    # Guard against counter resets/wraps
    [[ $bytes_interval -lt 0 ]] && bytes_interval=0
    [[ $packets_interval -lt 0 ]] && packets_interval=0

    bytes_processed=$((bytes_processed + bytes_interval))
    packets_processed=$((packets_processed + packets_interval))

    echo "$bytes_interval,$packets_processed" >> "$out_file"

    if [[ $bytes_interval -gt 0 ]]; then
        reports_with_data=$((reports_with_data + 1))
    fi

    bytes_initial_temp=$bytes_current
    packets_initial_temp=$packets_current

    wait $sleep_pid
done

echo "$bytes_processed bytes processed" >> "$out_file"
echo "$packets_processed packets processed" >> "$out_file"
if [[ $reports_with_data -gt 0 ]]; then
    avg_mbps=$(echo "scale=2; (($bytes_processed*8 / $reports_with_data) / $time_interval) / 1024 / 1024" | bc)
    echo "$avg_mbps Mbps" >> "$out_file"
fi

# Capture final stats for extra metrics
read_extra_stats "$interface"
out_of_buffer_final=$out_of_buffer_val
packets_phy_final=$packets_phy_val
discards_phy_final=$discards_phy_val

# Read final rx_packets for comparison
read_stats "$interface" "${rx_or_tx}_bytes" "${rx_or_tx}_packets"
packets_final=$packets_val

# Calculate deltas
out_of_buffer_delta=$((out_of_buffer_final - out_of_buffer_initial))
discards_phy_delta=$((discards_phy_final - discards_phy_initial))
packets_phy_delta=$((packets_phy_final - packets_phy_initial))
packets_delta=$((packets_final - packets_initial))

# Print comparisons
echo "" >> "$out_file"
echo "=== Additional Metrics ===" >> "$out_file"
echo "out_of_buffer events during window: $out_of_buffer_delta" >> "$out_file"
echo "phy discards during window: $discards_phy_delta" >> "$out_file"
echo "rx_packets_phy during window: $packets_phy_delta" >> "$out_file"
echo "rx_packets during window: $packets_delta" >> "$out_file"

packets_mismatch=$((packets_phy_delta - packets_delta))
if [[ $packets_mismatch -eq 0 ]]; then
    echo "rx_packets_phy matches rx_packets" >> "$out_file"
else
    echo "Difference (rx_packets_phy - rx_packets): $packets_mismatch" >> "$out_file"
fi

echo "Single-interface monitoring completed"
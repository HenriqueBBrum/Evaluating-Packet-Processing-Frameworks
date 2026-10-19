import os
import json
import re

def parse_suricata_stats_log(path, tool, name="stats.log"):
    total_packets = 0    
    total_drops = 0

    stats_log_path = os.path.join(path, name)
    if os.path.exists(stats_log_path):
        with open(stats_log_path, 'r') as f:
            lines = f.readlines()
            for line in reversed(lines):
                if "AF_PACKET" in tool.upper() or "PF_RING" in tool.upper():
                    if "capture.kernel_drops" in line:
                        drops_match = re.search(r'(\d+)$', line)
                        if drops_match:
                            total_drops = int(drops_match.group(1))
                    
                    if "capture.kernel_packets" in line:
                        total_processed_match = re.search(r'(\d+)$', line)
                        if total_processed_match:
                            total_packets = int(total_processed_match.group(1))

                        if "PF_RING" in tool.upper():
                            total_packets = total_packets + total_drops

                        break

                if "DPDK" in tool.upper():
                    if "capture.dpdk.imissed" in line:
                        drops_match = re.search(r'(\d+)$', line)
                        if drops_match:
                            total_drops = int(drops_match.group(1))

                    if "capture.packets" in line:
                        total_processed_match = re.search(r'(\d+)$', line)
                        if total_processed_match:
                            total_packets = int(total_processed_match.group(1))

                        break

                if "AF_XDP" in tool.upper():
                    if "capture.kernel_drops" in line:
                        drops_match = re.search(r'(\d+)$', line)
                        if drops_match:
                            total_drops = int(drops_match.group(1))

                    if "capture.afxdp_packets" in line:
                        total_processed_match = re.search(r'(\d+)$', line)
                        if total_processed_match:
                            total_packets = int(total_processed_match.group(1))

                        break
    
    return total_packets, total_drops


def parse_rx_application(path, pf_ring=False, name="packet_counter.txt"):
    rx_application_path = os.path.join(path, name)
    packets_rx = 0
    if os.path.exists(rx_application_path):
        with open(rx_application_path, 'r') as f:
            for line in reversed(f.readlines()):
                if pf_ring and "Aggregate stats" in line:
                    final_line = line.split("[")[-1].strip()
                    packets_rx = re.findall(r'\d+', final_line)
                    if packets_rx:
                        packets_rx = int(packets_rx[0])
                    else:
                        packets_rx = 0

                    break

                if not pf_ring and "Total packets:" in line:
                    packets_rx = int(line.split(":")[-1].strip())
                    break

    return packets_rx

def parse_rx_log(path, name="throughput.log"):
    rx_log_path = os.path.join(path, name)
    packets_rx = 0
    out_of_buffer = 0
    if os.path.exists(rx_log_path):
        with open(rx_log_path, 'r') as f:
            lines =  f.readlines()
            for line in reversed(lines):
                if "rx_packets during window:" in line:
                    packets_rx = int(line.split(":")[-1].strip())

                if "out_of_buffer" in line:
                    out_of_buffer = int(line.split(":")[-1].strip())
                    break
          
    return packets_rx, out_of_buffer

def parse_pf_ring_if_output(path, ifname="ens1f0np0"):
    total_packets = 0
    total_drops = 0

    if os.path.exists(path):
        for file in os.listdir(path):
            if ifname in file:
                with open(os.path.join(path, file), 'r') as f:
                    for line in f:
                        if "Tot Packets" in line:
                            total_packets += int(line.split(":")[-1].strip())

                        if "Tot Pkt Lost" in line:
                            total_drops += int(line.split(":")[-1].strip())

    return total_packets, total_drops

def parse_trex_log(path, name="trex.log"):
    packets_sent = 0
    trex_log_path = os.path.join(path, name)
    if os.path.exists(trex_log_path):
        with open(trex_log_path, 'r') as f:
            try:
                data = json.load(f)
                packets_sent = data["total"]["opackets"]
            except json.JSONDecodeError:
                print(f"Error decoding JSON from {trex_log_path}")

    return int(packets_sent)

def parse_cpu_usage(path, name="pidstat.log"):
    cpu_usage_list = []

    pidstat_log_path = os.path.join(path, name)
    if not os.path.exists(pidstat_log_path):
        return cpu_usage_list

    cpu_col_idx = None
    with open(pidstat_log_path, 'r') as f:
        for raw_line in f:
            line = raw_line.strip()
            if not line:
                continue

            if line.startswith("Linux") or line.startswith("Average:"):
                continue

            # Track the current pidstat table header to discover CPU% column.
            if "%CPU" in line:
                headers = line.split()
                if "%CPU" in headers:
                    cpu_col_idx = headers.index("%CPU")
                continue

            if cpu_col_idx is None:
                continue

            fields = line.split()
            if len(fields) <= cpu_col_idx:
                continue

            cpu_str = fields[cpu_col_idx].replace(',', '.')
            cpu_col_idx = None
            try:
                cpu_usage = float(cpu_str)
            except ValueError:
                continue

            cpu_usage_list.append(cpu_usage)
    return cpu_usage_list
             

def serialize_sets(obj):
    if isinstance(obj, set):
        return {"__type__": "set", "items": sorted(obj)}

    return obj

def save_json_results(parsed_results, output_folder):
    output_path = os.path.join(output_folder, "parsed_results.json")
    with open(output_path, 'w') as f:
        json.dump(parsed_results, f, indent=4, default=serialize_sets)

def deserialize_sets(obj):
    if isinstance(obj, dict) and obj.get("__type__") == "set":
        return set(obj.get("items", []))

    return obj


#!/usr/bin/env python

import sys
import argparse
import ipaddress
import json
import string
import random
import time
import os

sys.path.append("/opt/trex/v3.08/trex_client/interactive/trex/examples/stl/")

import stl_path
from trex.stl.api import *

BASE_DST_IP = "192.168.0.0"
BASE_SRC_IP = "172.16.0.0"

START_PORT = 20
END_PORT = 65535

RATE = 10  # 50 # link percentage, used as mult in c.start()


class FlowGen(object):
    def __init__(self, file_with_flows):
        self.flows, self.amt_flows, self.total_bytes = self.__load_flows_from_file(file_with_flows)

    # Load flows from a txt file where each line is a unique flow with the format: num_packets,num_bytes
    def __load_flows_from_file(self, distribution_file):
        flows = {}
        num_packets_unique = set()
        amt_flows = 0
        amt_packets = 0
        total_bytes = 0

        with open(distribution_file, 'r') as f:
            for line in f:
                line = line.strip()
                if line:
                    num_packets, num_bytes = map(int, line.split(','))
                    num_packets_unique.add(num_packets)

                    key = (num_packets, num_bytes)
                    flows[key] = flows.get(key, 0) + 1
                    amt_flows += 1
                    amt_packets += num_packets
                    total_bytes += num_bytes

        print("Unique flows loaded from file: %d, total flows: %d, total packets: %d, total bytes: %d" % (len(flows), amt_flows, amt_packets, total_bytes))
        return flows, amt_flows, total_bytes

    # For each unique (num_packets,num_bytes) flow configuration, calculate how many flows we need to create and the range of source IPs and ports to use for those flows
    def __create_range_for_streams(self, base_src_ip):
        streams = []
        start_src_ip = base_src_ip
        start_port = START_PORT

        total_flows = 0
        avg_bytes_per_pkt = []
        pkts_to_send_total = 0
        for flow_config in self.flows:
            packets_to_send, bytes_to_send = flow_config
            num_flows = self.flows[flow_config]

            bytes_per_packet = int(bytes_to_send / packets_to_send if packets_to_send > 0 else 0) # 1500 #
            avg_bytes_per_pkt.append(bytes_per_packet)

            ips_needed = math.ceil(num_flows / (END_PORT - START_PORT + 1))
            ports_per_ip =  math.ceil(num_flows / ips_needed)  # Cause a few more flows to be created           
            
            # Check if for this IP the new end port is too bigger than the maximu
            if (start_port + ports_per_ip - 1) > END_PORT:
               start_src_ip = str(ipaddress.ip_address(start_src_ip) + 1)
               start_port = START_PORT
            
            end_port = start_port + ports_per_ip - 1
            end_src_ip = str(ipaddress.ip_address(start_src_ip) + ips_needed - 1)
            streams.append({
                'start_src_ip': start_src_ip,
                'end_src_ip': end_src_ip,
                'min_port': start_port,
                'max_port': end_port,
                'rate': num_flows * bytes_to_send / self.total_bytes * 100,
                'pkts_to_send': num_flows * packets_to_send,
                'bytes_per_packet': bytes_per_packet,
            })
            pkts_to_send_total += num_flows * packets_to_send
            total_flows +=(ports_per_ip * ips_needed)

            start_src_ip = end_src_ip
            start_port = end_port + 1
            if start_port >= END_PORT:
                start_src_ip = str(ipaddress.ip_address(start_src_ip) + 1)
                start_port = START_PORT + (start_port - END_PORT)

        print("Total flows generated: %d" % total_flows)
        print("Total packets to send across all flows: %d" % pkts_to_send_total)
        print("Average bytes per packet across all flows: %d" % (sum(avg_bytes_per_pkt) / len(avg_bytes_per_pkt)))
        print("Total streams: %d" % len(streams))
        return streams

    # Create a sample packet for the stream based on the protocol and packet size (calculated from bytes_to_send / packets_to_send for the flow)
    def __create_pkt(self, protocol, pkt_size=0):
        base_pkt_len = len(Ether() / IP() / TCP()) if protocol == "tcp" else len(Ether() / IP() / UDP())

        payload_data_len = max(0, pkt_size - base_pkt_len - 4)
        payload = ''.join(random.choices(string.ascii_letters, k=payload_data_len))

        if protocol == "tcp":
            pkt = Ether() / IP(dst=BASE_DST_IP) / TCP(sport=START_PORT, dport=START_PORT) / payload
        else:
            pkt = Ether() / IP(dst=BASE_DST_IP) / UDP(sport=START_PORT, dport=START_PORT) / payload

        return pkt

    # Create streams based on the loaded flows and the calculated ranges of IPs and ports for each stream
    def get_streams(self, base_src_ip=BASE_SRC_IP):
        ranges = self.__create_range_for_streams(base_src_ip)
        streams = []

        unique_entries = 0
        for range_ in ranges:
            vm_var = STLVM()
            vm_var.tuple_var(
                ip_min=range_['start_src_ip'], ip_max=range_['end_src_ip'],
                port_min=range_['min_port'], port_max=range_['max_port'],
                name='tuple'
            )
            unique_entries += ((range_['max_port'] - range_['min_port'] + 1) * (int(ipaddress.ip_address(range_['end_src_ip'])) - int(ipaddress.ip_address(range_['start_src_ip'])) + 1))
            vm_var.write(fv_name='tuple.ip', pkt_offset='IP.src')
            vm_var.write(fv_name='tuple.port', pkt_offset='TCP.sport')
            vm_var.fix_chksum()

            pkt = self.__create_pkt("tcp", pkt_size=range_['bytes_per_packet'])
            stream = STLStream(
                packet=STLPktBuilder(pkt=pkt, vm=vm_var),
                mode=STLTXCont(percentage=range_['rate']),
            )
            streams.append(stream)

        print("Total unique entries: %d" % unique_entries)
        return streams


def run(args):
    c = STLClient()
    try:
        print("=== Starting TRex Client ===")

        flows_gen = FlowGen(args.flows_file)
        streams = flows_gen.get_streams()

        c.connect()
        c.reset(ports=[args.port])
        c.add_streams(streams, ports=[args.port])
        c.clear_stats()

        mult = f"{args.rate}%"
        print(f"Starting traffic on port {args.port} for {args.duration}s at mult={mult}")

        start = time.time()
        c.start(ports=[args.port], duration=args.duration, mult=mult)
        c.wait_on_traffic(ports=[args.port])
        print(f"Traffic generation completed in {time.time() - start:.2f} seconds")
        print("=== Finished TRex Client ===")

        stats = c.get_stats()
        if args.log_file:
            with open(args.log_file, 'w') as f:
                json.dump(stats, f, indent=2)
        else:
            print(json.dumps(stats['total'], indent=2))

    except STLError as e:
        print(e)
    finally:
        c.disconnect()

    if c.get_warnings():
        print("\n\n*** test had warnings ****\n\n")
        for w in c.get_warnings():
            print(w)


if __name__ == "__main__":
    parser = argparse.ArgumentParser(description='TRex STL Client - simple proportional flow replay')

    parser.add_argument('-p', '--port', type=int, default=0, help='Port to send traffic on')
    parser.add_argument('-d', '--duration', type=int, default=10, help='Duration of traffic generation in seconds')
    parser.add_argument('-f', '--flows_file', type=str, required=True, help='File containing flows (packets,bytes per line)')
    parser.add_argument('-r', '--rate', type=float, default=RATE, help='Link utilization percentage used as mult in c.start() (default: %(default)s)')
    parser.add_argument('-l', '--log_file', type=str, help='Optional JSON log file for stats output')

    args = parser.parse_args()
    
    args.flows_file = os.path.dirname(os.path.abspath(__file__)) + "/flows/" + args.flows_file
 
    print(args.flows_file)
    run(args)

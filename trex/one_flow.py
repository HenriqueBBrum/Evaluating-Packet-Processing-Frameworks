#!/usr/bin/env python

import sys

sys.path.append("/opt/trex/v3.08/trex_client/interactive/trex/examples/stl/")

import stl_path
from trex.stl.api import *
import argparse
import ipaddress
import json
import string
import random
import time

BASE_DST_IP = "192.168.0.0"
BASE_SRC_IP = "172.16.0.0"

SRC_PORT = 20
DST_PORT = 65535


def create_pkt(protocol, pkt_size=0, src_port=SRC_PORT):
    base_pkt_len = len(Ether() / IP() / TCP()) if protocol == "tcp" else len(Ether() / IP() / UDP())

    payload_data_len = max(0, pkt_size - base_pkt_len - 4)
    payload = ''.join(random.choices(string.ascii_letters, k=payload_data_len))

    if protocol == "tcp":
        pkt = Ether() / IP(dst=BASE_DST_IP) / TCP(sport=src_port, dport=DST_PORT) / payload
    else:
        pkt = Ether() / IP(dst=BASE_DST_IP) / UDP(sport=src_port, dport=DST_PORT) / payload

    return pkt

def create_streams(protocol, pkt_size):
    pkt = create_pkt(protocol=protocol, pkt_size=pkt_size)
    stream = STLStream(
        packet=STLPktBuilder(pkt=pkt),
        mode=STLTXCont(),
    )

    return [stream]

def run(args):
    c = STLClient()
    try:
        print("\n============ Starting TRex Client ============")

        print(f"Packet size: {args.pkt_size}")
        print(f"Protocol: {args.protocol}")

        streams = create_streams(protocol=args.protocol, pkt_size=args.pkt_size)
        if not streams:
            print("No streams created. Exiting.")
            return
        
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
        print("============ Finished TRex Client ============\n")

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
    parser.add_argument('-r', '--rate', type=float, default=10, help='Link utilization percentage used as mult in c.start() (default: %(default)s)')
    parser.add_argument('-l', '--log_file', type=str, help='Optional JSON log file for stats output')
    parser.add_argument('--pkt_size', type=int, default=64, help='Packet size in bytes')
    parser.add_argument('--protocol', type=str, choices=['tcp', 'udp'], default='tcp', help='Protocol to use for traffic generation')

    args = parser.parse_args()
    run(args)

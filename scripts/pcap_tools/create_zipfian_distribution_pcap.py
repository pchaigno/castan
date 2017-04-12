#!/usr/bin/python

import argparse
import csv
import numpy as np
import sys
from random import randint

from scapy.all import *
from scapy.utils import PcapWriter

class IPTable():
    _flow_table = {}



    def lookup_flow(self, flow):
        if flow in self._flow_table:
            return self._flow_table[flow]
            

        src_ip = socket.inet_ntoa(struct.pack('!L', random.randint(0,0xFFFFFFFF)))
        dst_ip = socket.inet_ntoa(struct.pack('!L', random.randint(0,0xFFFFFFFF)))
        protocol = 6 if random.randint(0,1) == 0 else 17
        sport = random.randint(1024,65535)
        dport = random.randint(1,10000)

        self._flow_table[flow] = (src_ip, dst_ip, protocol, sport, dport)

        return self._flow_table[flow]



def create_flows(num_packets, bytes_per_packet, output_file, zipf_param):
    pktdump = PcapWriter(output_file, append=False)

    ip_table = IPTable()

    for i in xrange(num_packets): 
        f = np.random.zipf(zipf_param, 1)[0] # generate a single sample from zipfian distribution
        src_ip, dst_ip, prot, sport, dport = ip_table.lookup_flow(f)

        pkt = IP(src=src_ip, dst=dst_ip)
        pkt = pkt/UDP(sport=sport,dport=dport)
        pkt.len = bytes_per_packet

        pktdump.write(pkt)


if __name__ == "__main__":
    # parse params
    parser = argparse.ArgumentParser(description="create a pcap file with n flows with equal distribution of b bytes")
    parser.add_argument('--zipf_param', type=float, help='Parameter for Zipf distribution ("s")', required=True)
    parser.add_argument('--npackets', type=int, help='number of packets total (across all flows)', required=True)
    parser.add_argument('--bytes_per_packet',  type=int, help='number of bytes per packet', required=True)
    parser.add_argument('--output',  help='name of output pcap file', required=True)

    args = parser.parse_args()

    create_flows(args.npackets, args.bytes_per_packet, args.output, args.zipf_param)
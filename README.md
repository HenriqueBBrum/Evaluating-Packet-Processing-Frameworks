# Evaluating Packet Processing Frameworks

This repository contains an analysis of four different packet capture and processing frameworks: AF_PACKET, AF_XDP, DPDK, and (vanilla) PF_RING. We evaluated them using a simple packet-counting application and when integrated with the Suricata NIDS. 

## Results and Outcomes

Our results show that DPDK has unmatched raw packet processing capabilities, presenting all packets to the user-space application across all scenarios. Figures 1 and 2 show these results under different link speeds, packet sizes, and flow profiles. 

![Raw packet processing evaluation.](assets/baseline_multi_flows.png)



However, when integrated with Suricata, this performance did not translate into application-level efficiency, as shown in Figure 3. Despite capturing all packets, Suricata with DPDK failed to inspect a significant amount of them, even when increasing the `mempool` parameters or the `rx-descriptors`. This indicates that the issue stems from how DPDK's raw packets are converted into data structures that Suricata can analyze, as well as how DPDK's internal structures are managed. 

Conversely, AF_XDP with busy-polling did not present all packets to Suricata, but (with enough threads) Suricata managed to inspect almost all the packets trasmitted, analyzing more than 95% of all traffic in our evaluation.

For more information on the evaluated frameworks and the experiment setup, read the [full report](Evaluating_Packet_Processing_Frameworks.pdf).

## Reproducing this Research

This section briefly details the steps needed to reproduce this research. We used two machines: one running the simple application and Suricata, and the other running TRex to generate the experiment's traffic. 

For the tested machine, ensure that all the above-mentioned tools exist, and proceed to install Suricata from the source code, enabling DPDK and PF_RING during configuration (e.g., `./configure ...`). 

On the traffic generation machine, install TRex and configure the folders as follows. There are dedicated scripts for each set of experiments; update the TRex machine-related information in each script.

There are three main scripts: `configuration_eval` for the results in Figure [X], `baseline_programs` for the results in Figure [Y], and `suricata_eval` for the results in Figure [Z].

With all these steps taken, you can reproduce the results obtained in this GitHub repository.
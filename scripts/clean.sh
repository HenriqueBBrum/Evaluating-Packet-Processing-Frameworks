

experiments_files=("configuration_eval.sh" "baseline_programs.sh" "suricata_eval.sh" "monitoring.sh")
for experiments_file in "${experiments_files[@]}"; do
    pkill -f "$experiments_file"
done

program_names=("dpdk_pkt_capture" "af_xdp" "pfcount_multichannel" "af_packet")
for program_name in "${program_names[@]}"; do
    pkill -f "$program_name"
done

# sudo kill -9 $(ps aux | grep suricata | grep -v grep | awk '{print $2}')
# if [[ -f /usr/local/var/run/suricata.pid ]]; then
#     sudo kill -INT $(cat /usr/local/var/run/suricata.pid)
#     sudo rm /usr/local/var/run/suricata.pid
# fi
sudo kill -9 $(ps aux | grep t-rex | grep -v grep | awk '{print $2}')

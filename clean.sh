pkill -f 'experiments.sh'
pkill -f 'monitoring.sh'
# sudo kill -9 $(ps aux | grep suricata | grep -v grep | awk '{print $2}')
# if [[ -f /usr/local/var/run/suricata.pid ]]; then
#     sudo kill -INT $(cat /usr/local/var/run/suricata.pid)
#     sudo rm /usr/local/var/run/suricata.pid
# fi
sudo kill -9 $(ps aux | grep t-rex | grep -v grep | awk '{print $2}')

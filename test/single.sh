#!/bin/sh
# Verifies operation in a single router setup:
#  - forwardng multicast between two emulated end devices
#  - IGMP v3 Query on both LANs

# shellcheck source=/dev/null
. "$(dirname "$0")/lib.sh"

# Requires ethtool to disable UDP checksum offloading
print "Check deps ..."
check_dep ethtool
check_dep tshark

print "Creating world ..."
left="/tmp/$NM/a1"
right="/tmp/$NM/a2"
touch "$left" "$right"
PID=$$

echo "$left"   > "/tmp/$NM/mounts"
echo "$right" >> "/tmp/$NM/mounts"

lif=$(basename "$left")
rif=$(basename "$right")

# Disabling UDP checksum offloading, frames are leaving kernel space
# on these VETH pairs.  (Silence noisy ethtool output)
unshare --net="$left" -- ip link set lo up
nsenter --net="$left" -- ip link add eth0 type veth peer "$lif"
nsenter --net="$left" -- ethtool --offload eth0 tx off >/dev/null
nsenter --net="$left" -- ethtool --offload eth0 rx off >/dev/null
nsenter --net="$left" -- ip link set "$lif" netns $PID
ethtool --offload "$lif" tx off >/dev/null
ethtool --offload "$lif" rx off >/dev/null
nsenter --net="$left" -- ip link set eth0 up
ip link set "$lif" up

unshare --net="$right" -- ip link set lo up
nsenter --net="$right" -- ip link add eth0 type veth peer "$rif"
nsenter --net="$right" -- ethtool --offload eth0 tx off >/dev/null
nsenter --net="$right" -- ethtool --offload eth0 rx off >/dev/null
nsenter --net="$right" -- ip link set "$rif" netns $PID
ethtool --offload "$rif" tx off >/dev/null
ethtool --offload "$rif" rx off >/dev/null
nsenter --net="$right" -- ip link set eth0 up
ip link set "$rif" up

ip addr add 10.0.0.1/24 dev a1
nsenter --net="$left" -- ip addr add 10.0.0.10/24 dev eth0
nsenter --net="$left" -- ip route add default via 10.0.0.1

ip addr add 20.0.0.1/24 dev a2
nsenter --net="$right" -- ip addr add 20.0.0.10/24 dev eth0
nsenter --net="$right" -- ip route add default via 20.0.0.1

ip -br l
ip -br a

print "Creating config ..."
cat <<EOF > "/tmp/$NM/conf"
no phyint
phyint $lif enable
phyint $rif enable
EOF
cat "/tmp/$NM/conf"

print "Starting collectors ..."
nsenter --net="$left"  -- tshark -lni eth0 -w "/tmp/$NM/left.pcap" 2>/dev/null &
echo $! >> "/tmp/$NM/PIDs"
nsenter --net="$right" -- tshark -lni eth0 -w "/tmp/$NM/right.pcap" 2>/dev/null &
echo $! >> "/tmp/$NM/PIDs"
sleep 1

print "Starting mrouted ..."
../src/mrouted -i solo -f "/tmp/$NM/conf" -n -p "/tmp/$NM/pid" -l debug -u "/tmp/$NM/sock" &
sleep 1

print "Starting emitter ..."
nsenter --net="$right" -- ./mping -qr -d -i eth0 -t 3 -W 30 225.1.2.3 &
echo $! >> "/tmp/$NM/PIDs"
sleep 1

if ! nsenter --net="$left"  -- ./mping -s -d -i eth0 -t 3 -c 10 -w 15 225.1.2.3; then
    show_mroute
    ../src/mroutectl -u "/tmp/$NM/sock"
    echo "Failed routing, expected at least 10 multicast ping replies"
    FAIL
fi

../src/mroutectl -u "/tmp/$NM/sock" show compat detail

kill_pids

print "Analyzing left.pcap ..."
lines1=$(tshark -n -r "/tmp/$NM/left.pcap" 2>/dev/null | grep "DVMRP 50 V3 Probe" | tee "/tmp/$NM/result" | wc -l)
lines2=$(tshark -n -r "/tmp/$NM/left.pcap" 2>/dev/null | grep "IGMPv3 50 Membership Query" | tee -a "/tmp/$NM/result" | wc -l)
cat "/tmp/$NM/result"
echo " => $lines1 DVMRP, expected 1"
echo " => $lines2 IGMP Query, expected 1"
# shellcheck disable=SC2086 disable=SC2166
[ $lines1 -ge 1 -a $lines2 -ge 1 ] || FAIL

print "Analyzing right.pcap ..."
lines1=$(tshark -n -r "/tmp/$NM/right.pcap" 2>/dev/null | grep "DVMRP 50 V3 Probe" | tee "/tmp/$NM/result" | wc -l)
lines2=$(tshark -n -r "/tmp/$NM/right.pcap" 2>/dev/null | grep "IGMPv3 50 Membership Query" | tee -a "/tmp/$NM/result" | wc -l)
cat "/tmp/$NM/result"
echo " => $lines1 DVMRP, expected 1"
echo " => $lines2 IGMP Query, expected 1"
# shellcheck disable=SC2086 disable=SC2166
[ $lines1 -ge 1 -a $lines2 -ge 1 ] || FAIL

OK

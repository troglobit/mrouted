#!/bin/sh
# Three routers in a row, end devices on each end.  Every router and
# end device in a network namespace to circumvent one-address per
# subnet in Linux
#
# NS1         NS2              NS3              NS4              NS5
# [ED1:eth0]--[eth1:R1:eth2]---[eth3:R2:eth4]---[eth5:R3:eth6]---[eth0:ED2]
#       10.0.0.0/24     10.0.1.0/24      10.0.2.0/24      10.0.3.0/24

# shellcheck source=/dev/null
. "$(dirname "$0")/lib.sh"

# Requires OSPF (bird) to build the unicast rpf tree
print "Check deps ..."
check_dep ethtool
check_dep tshark
check_dep bird

print "Creating world ..."
NS1="/tmp/$NM/NS1"
NS2="/tmp/$NM/NS2"
NS3="/tmp/$NM/NS3"
NS4="/tmp/$NM/NS4"
NS5="/tmp/$NM/NS5"
touch "$NS1" "$NS2" "$NS3" "$NS4" "$NS5"

echo "$NS1"  > "/tmp/$NM/mounts"
echo "$NS2" >> "/tmp/$NM/mounts"
echo "$NS3" >> "/tmp/$NM/mounts"
echo "$NS4" >> "/tmp/$NM/mounts"
echo "$NS5" >> "/tmp/$NM/mounts"

unshare --net="$NS1" -- ip link set lo up
unshare --net="$NS2" -- ip link set lo up
unshare --net="$NS3" -- ip link set lo up
unshare --net="$NS4" -- ip link set lo up
unshare --net="$NS5" -- ip link set lo up

# Creates a VETH pair, one end named eth0 and the other is eth7:
#
#     created /tmp/foo eth0 eth7 1.2.3.4/24 1.2.3.1
#
# Disabling UDP checksum offloading with ethtool, frames are leaving
# kernel space on these VETH pairs.  (Silence noisy ethtool output)
created()
{
    in=$2
    if echo "$3" | grep -q '@'; then
	ut=$(echo "$3" | cut -f1 -d@)
	id=$(echo "$3" | cut -f2 -d@)
    else
	ut=$3
    fi

    echo "Creating device interfaces $in and $ut ..."
    nsenter --net="$1" -- ip link add "$in" type veth peer "$ut"
    nsenter --net="$1" -- ip link set "$in" up

    nsenter --net="$1" -- ip addr add "$4" broadcast + dev "$2"
    nsenter --net="$1" -- ip route add default via "$5"

    for iface in "$in" "$ut"; do
	nsenter --net="$1" -- ethtool --offload "$iface" tx off >/dev/null
	nsenter --net="$1" -- ethtool --offload "$iface" rx off >/dev/null
    done

    if [ -n "$id" ]; then
	echo "$1 moving $ut to netns PID $id"
	nsenter --net="$1" -- ip link set "$ut" netns "$id"
    fi

    return $!
}

creater()
{
    if echo "$2" |grep -q ':'; then
	x=$(echo "$2" | cut -f1 -d:)
	b=$(echo "$2" | cut -f2 -d:)
	echo "1) Found x=$x and b=$b ..."
	a=$(echo "$x" | cut -f1 -d@)
	p=$(echo "$x" | cut -f2 -d@)
	echo "1) Found a=$a and p=$p ..."
	echo "Creating router interfaces $a and $b ..."
	nsenter --net="$1" -- ip link add "$a" type veth peer "$b"
	for iface in "$a" "$b"; do
	    nsenter --net="$1" -- ethtool --offload "$iface" tx off >/dev/null
	    nsenter --net="$1" -- ethtool --offload "$iface" rx off >/dev/null
	done
	echo "$1 moving $a to netns PID $p"
	nsenter --net="$1" -- ip link set "$a" netns "$p"
    else
	b=$2
    fi

    echo "Bringing up $a with addr $4"
    nsenter --net="$1" -- ip link set "$b" up
    nsenter --net="$1" -- ip addr add "$4" broadcast + dev "$b"

    if echo "$3" |grep -q ':'; then
	a=$(echo "$3" | cut -f1 -d:)
	x=$(echo "$3" | cut -f2 -d:)
	echo "2) Found x=$x and b=$b ..."
	b=$(echo "$x" | cut -f1 -d@)
	p=$(echo "$x" | cut -f2 -d@)
	echo "2) Found a=$a and p=$p ..."
	echo "Creating router interfaces $a and $b ..."
	nsenter --net="$1" -- ip link add "$a" type veth peer "$b"
	for iface in "$a" "$b"; do
	    nsenter --net="$1" -- ethtool --offload "$iface" tx off >/dev/null
	    nsenter --net="$1" -- ethtool --offload "$iface" rx off >/dev/null
	done
	echo "$1 moving $b to netns PID $p"
	nsenter --net="$1" -- ip link set "$b" netns "$p"
    else
	a=$3
    fi

    echo "Bringing up $a with addr $5"
    nsenter --net="$1" -- ip link set "$a" up
    nsenter --net="$1" -- ip addr add "$5" broadcast + dev "$a"
}

dvmrp_routes()
{
    dprint "R1 DVMRP Routes"
    nsenter --net="$NS2" -- ../src/mroutectl -pt -u "/tmp/$NM/r1.sock" -d show routes
    dprint "R2 DVMRP Routes"
    nsenter --net="$NS3" -- ../src/mroutectl -pt -u "/tmp/$NM/r2.sock" -d show routes
    dprint "R3 DVMRP Routes"
    nsenter --net="$NS4" -- ../src/mroutectl -pt -u "/tmp/$NM/r3.sock" -d show routes
}

dvmrp_status()
{
    dprint "DVMRP Status $NS2"
    nsenter --net="$NS2" -- ../src/mroutectl -u "/tmp/$NM/r1.sock" show compat detail
    dprint "DVMRP Status $NS3"
    nsenter --net="$NS3" -- ../src/mroutectl -u "/tmp/$NM/r2.sock" show compat detail
    dprint "DVMRP Status $NS4"
    nsenter --net="$NS4" -- ../src/mroutectl -u "/tmp/$NM/r3.sock" show compat detail
}

dvmrp_neigh()
{
    dprint "R1 DVMRP Neighbors"
    nsenter --net="$NS2" -- ../src/mroutectl -pt -u "/tmp/$NM/r1.sock" -d show neighbor
    dprint "R2 DVMRP Neighbors"
    nsenter --net="$NS3" -- ../src/mroutectl -pt -u "/tmp/$NM/r2.sock" -d show neighbor
    dprint "R3 DVMRP Neighbors"
    nsenter --net="$NS4" -- ../src/mroutectl -pt -u "/tmp/$NM/r3.sock" -d show neighbor
}

has_neigh()
{
    nm=$(basename "$2" .sock)
    neighbors=$(nsenter --net="$1" -- \
			../src/mroutectl -pt -u "$2" -d show neighbor \
		    | awk 'NF && $4 == "G" { print $1 }')
    shift 2

    while [ $# -gt 0 ]; do
        if ! echo "$neighbors" | grep -wq "$1"; then
	    echo "$nm: missing neighbor $1"
            return 1
        fi
        shift
    done

    return 0
}

dvmrp_peer()
{
    has_neigh "$NS2" "/tmp/$NM/r1.sock" 10.0.1.2          || return 1
    has_neigh "$NS3" "/tmp/$NM/r2.sock" 10.0.1.1 10.0.2.2 || return 1
    has_neigh "$NS4" "/tmp/$NM/r3.sock" 10.0.2.1          || return 1
}

dprint "Creating $NS2 router ..."
nsenter --net="$NS2" -- sleep 5 &
pid2=$!
dprint "Creating $NS4 router ..."
nsenter --net="$NS4" -- sleep 5 &
pid4=$!

dprint "Creating NS1 ED with eth1 in PID $pid2"
created "$NS1" eth0 eth1@"$pid2" 10.0.0.10/24 10.0.0.1

dprint "Creating NS3 router with eth3 in PID $pid2 and eth5 in PID $pid4"
creater "$NS3" eth2@"$pid2":eth3 eth4:eth5@"$pid4" 10.0.1.2/24 10.0.2.1/24

dprint "Creating NS5 ED with eth6 in PID $pid4"
created "$NS5" eth0 eth6@"$pid4" 10.0.3.10/24 10.0.3.1

creater "$NS2" eth1 eth2 10.0.0.1/24 10.0.1.1/24
creater "$NS4" eth5 eth6 10.0.2.2/24 10.0.3.1/24

dprint "$NS1"
nsenter --net="$NS1" -- ip -br l
nsenter --net="$NS1" -- ip -br a
dprint "$NS2"
nsenter --net="$NS2" -- ip -br l
nsenter --net="$NS2" -- ip -br a
dprint "$NS3"
nsenter --net="$NS3" -- ip -br l
nsenter --net="$NS3" -- ip -br a
dprint "$NS4"
nsenter --net="$NS4" -- ip -br l
nsenter --net="$NS4" -- ip -br a
dprint "$NS5"
nsenter --net="$NS5" -- ip -br l
nsenter --net="$NS5" -- ip -br a

print "Creating OSPF config ..."
cat <<EOF > "/tmp/$NM/bird.conf"
protocol device {
}
protocol direct {
	ipv4;
}
protocol kernel {
	ipv4 {
		export all;
	};
	learn;
}
protocol ospf {
	ipv4 {
		import all;
	};
	area 0 {
		interface "eth*" {
			type broadcast;
			hello 1;
			wait  3;
			dead  5;
		};
	};
}
EOF
cat "/tmp/$NM/bird.conf"

print "Starting Bird OSPF ..."
nsenter --net="$NS2" -- bird -c "/tmp/$NM/bird.conf" -d -s "/tmp/$NM/r1-bird.sock" &
echo $! >> "/tmp/$NM/PIDs"
nsenter --net="$NS3" -- bird -c "/tmp/$NM/bird.conf" -d -s "/tmp/$NM/r2-bird.sock" &
echo $! >> "/tmp/$NM/PIDs"
nsenter --net="$NS4" -- bird -c "/tmp/$NM/bird.conf" -d -s "/tmp/$NM/r3-bird.sock" &
echo $! >> "/tmp/$NM/PIDs"

print "Starting mrouted ..."
LVL=info
nsenter --net="$NS2" -- ../src/mrouted -i NS2 -n -p "/tmp/$NM/r1.pid" -l $LVL -d all -u "/tmp/$NM/r1.sock" &
echo $! >> "/tmp/$NM/PIDs"
nsenter --net="$NS3" -- ../src/mrouted -i NS3 -n -p "/tmp/$NM/r2.pid" -l $LVL -d all -u "/tmp/$NM/r2.sock" &
echo $! >> "/tmp/$NM/PIDs"
nsenter --net="$NS4" -- ../src/mrouted -i NS4 -n -p "/tmp/$NM/r3.pid" -l $LVL -d all -u "/tmp/$NM/r3.sock" &
echo $! >> "/tmp/$NM/PIDs"

# Wait for routers to peer
print "Waiting for OSPF routers to peer (30 sec) ..."
tenacious 30 nsenter --net="$NS1" -- ping -qc 1 -W 1 10.0.3.10 >/dev/null
dprint "OK"

print "Waiting for DVMRP routers to peer (30 sec) ..."
tenacious 30 dvmrp_peer
dprint "OK"

# dprint "OSPF State & Routing Table $NS2:"
# nsenter --net="$NS2" -- echo "show ospf state" | birdc -s "/tmp/$NM/r1-bird.sock"
# nsenter --net="$NS2" -- echo "show ospf int"   | birdc -s "/tmp/$NM/r1-bird.sock"
# nsenter --net="$NS2" -- echo "show ospf neigh" | birdc -s "/tmp/$NM/r1-bird.sock"
# nsenter --net="$NS2" -- ip route

# dprint "OSPF State & Routing Table $NS3:"
# nsenter --net="$NS4" -- echo "show ospf state" | birdc -s "/tmp/$NM/r2-bird.sock"
# nsenter --net="$NS4" -- echo "show ospf int"   | birdc -s "/tmp/$NM/r2-bird.sock"
# nsenter --net="$NS3" -- echo "show ospf neigh" | birdc -s "/tmp/$NM/r2-bird.sock"
# nsenter --net="$NS3" -- ip route

# dprint "OSPF State & Routing Table $NS4:"
# nsenter --net="$NS4" -- echo "show ospf state" | birdc -s "/tmp/$NM/r3-bird.sock"
# nsenter --net="$NS4" -- echo "show ospf int"   | birdc -s "/tmp/$NM/r3-bird.sock"
# nsenter --net="$NS4" -- echo "show ospf neigh" | birdc -s "/tmp/$NM/r3-bird.sock"
# nsenter --net="$NS4" -- ip route

print "Starting emitter ..."
nsenter --net="$NS5" -- ./mping -qr -i eth0 -t 5 -W 30 225.1.2.3 &
echo $! >> "/tmp/$NM/PIDs"
sleep 1

if ! nsenter --net="$NS1"  -- ./mping -s -i eth0 -t 5 -c 10 -w 30 225.1.2.3; then
    dprint "DVMRP Status $NS2"
    nsenter --net="$NS2" -- ../src/mroutectl -u "/tmp/$NM/r1.sock" show compat detail
    dprint "DVMRP Status $NS3"
    nsenter --net="$NS3" -- ../src/mroutectl -u "/tmp/$NM/r2.sock" show compat detail
    dprint "DVMRP Status $NS4"
    nsenter --net="$NS4" -- ../src/mroutectl -u "/tmp/$NM/r3.sock" show compat detail
    echo "Failed routing, expected at least 10 multicast ping replies"
    FAIL
fi

OK

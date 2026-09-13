#!/bin/sh
# Verifies `mroutectl reload ifaces` picks up interface changes without
# a restart:
#  - an interface added after startup is brought into service
#  - an interface that has been removed is taken out of service
#  - interfaces that did not change keep the VIF number they had
#  - the VIF number freed by a removed interface is reused
#
# The VIF number is a kernel VIF index as well, and is recorded in
# routes, prunes and MFC entries, so renumbering on the fly would
# silently point every one of them at the wrong interface.

# shellcheck source=/dev/null
. "$(dirname "$0")/lib.sh"

CTL="../src/mroutectl -u /tmp/$NM/sock"

mkiface()
{
    ip link add "$1" type dummy
    ip link set "$1" multicast on
    ip link set "$1" up
    ip addr add "$2" dev "$1"
}

# Print the VIF number mrouted gave an interface, nothing if it has none
vif_of()
{
    $CTL show compat 2>/dev/null \
	| awk -v ifname="$1" '$1 ~ /^[0-9]+$/ && $2 == ifname { print $1; exit }'
}

expect()
{
    got=$(vif_of "$1")
    if [ "$got" != "$2" ]; then
	dprint "$1: expected VIF $2, got '${got:-none}'"
	$CTL show compat
	FAIL "$1 should be VIF $2"
    fi
    dprint "$1 is VIF $2, as expected"
}

expect_gone()
{
    got=$(vif_of "$1")
    if [ -n "$got" ]; then
	$CTL show compat
	FAIL "$1 should be gone, still VIF $got"
    fi
    dprint "$1 is gone, as expected"
}

reload()
{
    $CTL reload ifaces || FAIL "Failed calling reload ifaces"
    sleep 1
}

print "Creating world ..."
mkiface a1 10.0.1.1/24
mkiface a2 10.0.2.1/24
mkiface a3 10.0.3.1/24
ip -br a

print "Creating config ..."
# Empty, mrouted enables every multicast capable interface it finds
: > "/tmp/$NM/conf"

print "Starting mrouted ..."
../src/mrouted -i reload -f "/tmp/$NM/conf" -n -p "/tmp/$NM/pid" -l debug -u "/tmp/$NM/sock" &
tenacious 10 $CTL show compat >/dev/null

print "Verifying initial VIF numbers ..."
expect a1 0
expect a2 1
expect a3 2

print "Adding a4, reloading ..."
mkiface a4 10.0.4.1/24
reload
expect a4 3
expect a1 0
expect a2 1
expect a3 2

print "Removing a2, reloading ..."
ip link del a2
reload
expect_gone a2

print "Verifying the other VIFs did not move ..."
expect a1 0
expect a3 2
expect a4 3

print "Adding a5, must reuse the VIF a2 freed ..."
mkiface a5 10.0.5.1/24
reload
expect a5 1
expect a1 0
expect a3 2
expect a4 3

print "Verifying mrouted is still running ..."
kill -0 "$(cat "/tmp/$NM/pid")" || FAIL "mrouted died during reload"

kill_pids

OK

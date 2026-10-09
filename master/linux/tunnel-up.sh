#!/bin/sh
# Create the master's tap interface, safely, and give it its address:
#
#   tunnel-up.sh <interface> <address/prefix>      (as root)
#
# The tap is persistent, so the address and the guards survive a restart of
# tmptun. The guards: multicast off and IPv6 off on the tunnel. A host that
# also has WiFi or Ethernet and runs mDNS (avahi) will otherwise reflect
# multicast between the networks; on a slow half-duplex link that storm can
# take both down. ARP is broadcast, not multicast, so ping and TCP still work.
set -e

IFACE="${1:?usage: tunnel-up.sh <interface> <address/prefix>}"
ADDR="${2:?usage: tunnel-up.sh <interface> <address/prefix>}"

ip tuntap add dev "$IFACE" mode tap 2>/dev/null || true
sysctl -w "net.ipv6.conf.${IFACE}.disable_ipv6=1" >/dev/null
ip link set "$IFACE" multicast off
ip addr flush dev "$IFACE" 2>/dev/null || true
ip addr add "$ADDR" dev "$IFACE"
ip link set "$IFACE" up mtu 1500

# The host acknowledges every TCP segment on the link at once, with no
# delayed ACK, so the ACK rides the next poll (tmptun waits briefly for it
# before polling again). It saves latency; nothing depends on it. quickack on
# the link's own route does that for every connection over it.
PREFIX=$(ip -4 route show dev "$IFACE" proto kernel scope link | awk 'NR == 1 { print $1 }')
ip route change "$PREFIX" dev "$IFACE" proto kernel scope link src "${ADDR%/*}" quickack 1

echo "$IFACE up as $ADDR: multicast off, IPv6 off, quickack"

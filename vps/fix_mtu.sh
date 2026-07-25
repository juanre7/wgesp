#!/usr/bin/env bash
#
# The real path MTU home<->VPS is 1420 bytes, so only 1360 fit inside the
# tunnel, but WireGuard advertises 1420: the difference is a black hole that
# forces TCP to retransmit and sinks throughput.
#
# The fix is to set the MTU on the routes going to the ESP (and only on those,
# so the other clients are not penalised) and to clamp the MSS of the TCP
# connections forwarded through the tunnel. --clamp-mss-to-pmtu uses the MTU of
# the route each packet takes, so every client gets the value that fits it.
#
#   sudo bash fix_mtu.sh
#
set -euo pipefail

MTU="${MTU:-1360}"
LAN_SUBNET="${LAN_SUBNET:-192.168.1.0/24}"
TABLE="${TABLE:-100}"

[ "$(id -u)" = 0 ] || { echo "ERROR: run it as root." >&2; exit 1; }

ip route replace "$LAN_SUBNET" dev wg0 mtu "$MTU"
ip route replace default dev wg0 table "$TABLE" mtu "$MTU"

# The OUTBOUND direction of an exit-through-home client was already fine
# (table $TABLE, with an MTU), but the RETURN path (the SYN-ACK coming from the
# internet towards the client) is routed through the main table, which has no
# MTU: --clamp-mss-to-pmtu then used the 1420 of wg0 and advertised MSS 1380
# when only 1320 fit through the ESP. The server sent 1420-byte segments and
# the ESP could not take them.
# Exit clients are discovered on their own: they are the ones with a rule to
# $TABLE.
for client_ip in $(ip rule show | sed -n "s/.*from \([0-9.]*\) lookup $TABLE\$/\1/p"); do
    ip route replace "$client_ip/32" dev wg0 mtu "$MTU"
    echo "MTU $MTU on the return path towards $client_ip too"
    # and let it survive a tunnel restart, one per client and idempotent
    line="PostUp = ip route replace $client_ip/32 dev %i mtu $MTU || true"
    grep -qF "$line" /etc/wireguard/wg0.conf || \
        sed -i "0,/^### /s@^### @$line\n\n### @" /etc/wireguard/wg0.conf
done

iptables -t mangle -C FORWARD -o wg0 -p tcp --tcp-flags SYN,RST SYN -j TCPMSS --clamp-mss-to-pmtu 2>/dev/null || \
    iptables -t mangle -A FORWARD -o wg0 -p tcp --tcp-flags SYN,RST SYN -j TCPMSS --clamp-mss-to-pmtu


# Persistence: wg-quick rebuilds the routes on startup, so this has to be repeated.
if ! grep -q "# wgesp: mtu" /etc/wireguard/wg0.conf; then
    python3 - "$MTU" "$LAN_SUBNET" "$TABLE" <<'PY'
import sys
mtu, lan, table = sys.argv[1:4]
path = "/etc/wireguard/wg0.conf"
text = open(path).read()
block = f"""# wgesp: real mtu up to the ESP32, see vps/fix_mtu.sh
PostUp = ip route replace {lan} dev %i mtu {mtu} || true
PostUp = ip route replace default dev %i table {table} mtu {mtu} || true
PostUp = iptables -t mangle -A FORWARD -o %i -p tcp --tcp-flags SYN,RST SYN -j TCPMSS --clamp-mss-to-pmtu || true
PostDown = iptables -t mangle -D FORWARD -o %i -p tcp --tcp-flags SYN,RST SYN -j TCPMSS --clamp-mss-to-pmtu || true

"""
i = text.index("### ")
open(path, "w").write(text[:i] + block + text[i:])
print("MTU PostUp lines added")
PY
fi

echo "=== routes ==="
ip route show dev wg0 | grep -E "mtu|$LAN_SUBNET"
ip route show table "$TABLE"
echo "=== MSS clamping ==="
iptables -t mangle -S FORWARD | grep TCPMSS || echo "(not applied: check this)"

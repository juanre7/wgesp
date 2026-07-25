#!/usr/bin/env bash
#
# Adds to an existing WireGuard server a profile whose way out to the internet
# is your home (through the ESP32), leaving untouched the peers that already
# exited through the VPS. Idempotent: re-running adds clients without touching
# the previous ones.
#
#   sudo bash enable_home_exit.sh                              # home   -> 10.66.66.10
#   sudo CLIENT=laptop CLIENT_IP=10.66.66.11 bash enable_home_exit.sh
#
# It never restarts the interface: changes are applied live with 'wg syncconf',
# because the admin SSH session may well be going through the tunnel itself.
#
set -euo pipefail

CLIENT="${CLIENT:-home}"
CLIENT_IP="${CLIENT_IP:-10.66.66.10}"

# Public key of the ESP32: setup.sh prints it and it lives in /etc/wireguard/wgesp/.
ESP_PUB="${ESP_PUB:-$(cat /etc/wireguard/wgesp/esp.pub 2>/dev/null || true)}"
[ -n "$ESP_PUB" ] || { echo "ERROR: pass ESP_PUB=<public key of the ESP32>." >&2; exit 1; }
LAN_SUBNET="${LAN_SUBNET:-192.168.1.0/24}"
ESP_IP="${ESP_IP:-10.66.66.6}"
ENDPOINT="${ENDPOINT:-vpn.example.com}"
WG_PORT="${WG_PORT:-51820}"
# DNS the client will use. Set it to your router (e.g. 192.168.1.1) if you
# want to resolve home hostnames while browsing through the house.
DNS="${DNS:-9.9.9.9}"
TABLE=100          # dedicated routing table: only the home peers consult it
RULE_PRIO=$((5000 + ${CLIENT_IP##*.}))

[ "$(id -u)" = 0 ] || { echo "ERROR: run it as root." >&2; exit 1; }
cd /etc/wireguard

cp -n wg0.conf "wg0.conf.bak-$(date +%Y%m%d-%H%M%S)" || true
umask 077
mkdir -p wgesp
cd wgesp
[ -f "$CLIENT.key" ] || { wg genkey > "$CLIENT.key"; wg pubkey < "$CLIENT.key" > "$CLIENT.pub"; wg genpsk > "$CLIENT.psk"; }
cd /etc/wireguard

# 1. Client peer (its traffic will exit through the ESP32)
if ! grep -q "### Client $CLIENT-exit" wg0.conf; then
    {
        echo
        echo "### Client $CLIENT-exit"
        echo "[Peer]"
        echo "PublicKey = $(cat wgesp/$CLIENT.pub)"
        echo "PresharedKey = $(cat wgesp/$CLIENT.psk)"
        echo "AllowedIPs = $CLIENT_IP/32"
    } >> wg0.conf
    echo "peer $CLIENT-exit added"
fi

# 2. Persistence across restarts. It goes as PostUp and not as AllowedIPs in
#    the file on purpose: if the file said 0.0.0.0/0, wg-quick would install a
#    default route towards wg0 and ALL the VPS traffic (SSH included) would
#    leave through the ESP32. This way wg-quick keeps managing routes as before
#    and we only widen the cryptographic filter of the ESP peer.
#    The "|| true" is mandatory: if a PostUp fails, wg-quick tears the
#    interface down.
python3 - "$ESP_PUB" "$ESP_IP" "$LAN_SUBNET" "$CLIENT_IP" "$TABLE" "$RULE_PRIO" <<'PY'
import sys
esp_pub, esp_ip, lan, client_ip, table, prio = sys.argv[1:7]
path = "/etc/wireguard/wg0.conf"
text = open(path).read()
lines = []

# Shared part: written once no matter how many clients there are
if "# wgesp: exit through home" not in text:
    lines += [
        "# wgesp: exit through home (see vps/enable_home_exit.sh)",
        f"PostUp = wg set %i peer {esp_pub} allowed-ips {esp_ip}/32,{lan},0.0.0.0/0 || true",
        f"PostUp = ip route replace default dev %i table {table} || true",
    ]

# Per-client part: a policy rule sending it to the ESP32 routing table
if f"from {client_ip} lookup {table}" not in text:
    lines += [
        f"PostUp = ip rule add from {client_ip} lookup {table} priority {prio} || true",
        f"PostDown = ip rule del from {client_ip} lookup {table} priority {prio} || true",
    ]

if lines:
    i = text.index("### ")          # right before the first peer
    open(path, "w").write(text[:i] + "\n".join(lines) + "\n\n" + text[i:])
    print("PostUp/PostDown added")
else:
    print("PostUp/PostDown were already there")
PY

# 3. Apply live, without dropping the existing tunnels.
wg syncconf wg0 <(wg-quick strip wg0)
# syncconf reimposes the AllowedIPs from the file, so the widening comes after
wg set wg0 peer "$ESP_PUB" allowed-ips "$ESP_IP/32,$LAN_SUBNET,0.0.0.0/0"
ip route replace default dev wg0 table "$TABLE"
ip rule del from "$CLIENT_IP" lookup "$TABLE" priority "$RULE_PRIO" 2>/dev/null || true
ip rule add from "$CLIENT_IP" lookup "$TABLE" priority "$RULE_PRIO"

echo
echo "=== state ==="
ip rule show | grep "$CLIENT_IP" || echo "(no rule: check this)"
ip route show table "$TABLE"
wg show wg0 allowed-ips | grep -E "$ESP_PUB|$(cat wgesp/$CLIENT.pub)"

echo
echo "=== profile for the phone/laptop (exit through home) ==="
cat <<EOF
[Interface]
PrivateKey = $(cat wgesp/$CLIENT.key)
Address = $CLIENT_IP/24
DNS = $DNS

[Peer]
PublicKey = $(wg show wg0 public-key)
PresharedKey = $(cat wgesp/$CLIENT.psk)
Endpoint = $ENDPOINT:$WG_PORT
AllowedIPs = 0.0.0.0/0
PersistentKeepalive = 25
EOF

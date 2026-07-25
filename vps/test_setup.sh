#!/usr/bin/env bash
# Rehearses vps/setup.sh in a sandbox with the system commands stubbed out.
# It checks the one thing that cannot be fixed live on the VPS: that the
# generated configuration is right and that re-running it does not rotate keys.
set -euo pipefail
cd "$(dirname "$0")"

SB="$(mktemp -d)"
trap 'rm -rf "$SB"' EXIT
mkdir -p "$SB/bin" "$SB/wg"

cat > "$SB/bin/wg" <<'EOF'
#!/bin/sh
case "$1" in
  genkey) head -c 32 /dev/urandom | base64 ;;
  pubkey) cat >/dev/null; head -c 32 /dev/urandom | base64 ;;
  *) : ;;
esac
EOF
for c in apt-get sysctl iptables qrencode wg-quick; do
    printf '#!/bin/sh\nexit 0\n' > "$SB/bin/$c"
done
printf '#!/bin/sh\ncase "$1" in is-active) exit 1;; esac\nexit 0\n' > "$SB/bin/systemctl"
printf '#!/bin/sh\necho 0\n' > "$SB/bin/id"
printf '#!/bin/sh\necho 203.0.113.45\n' > "$SB/bin/curl"
chmod +x "$SB"/bin/*

# the only divergence from the real script: /etc/sysctl.d needs actual root
sed "s|> /etc/sysctl.d/99-wgesp.conf|> $SB/sysctl.conf|" setup.sh > "$SB/setup.sh"
run() { PATH="$SB/bin:$PATH" WG_DIR="$SB/wg" bash "$SB/setup.sh" "$@" >/dev/null 2>&1; }

run
CONF="$SB/wg/wg0.conf"
grep -q "AllowedIPs = 10.6.0.2/32, 192.168.1.0/24" "$CONF" || { echo "FAIL: the ESP peer does not route the LAN"; exit 1; }
grep -q "AllowedIPs = 10.6.0.10/32" "$CONF" || { echo "FAIL: the first client is missing"; exit 1; }
grep -q "^PostUp .*FORWARD -i wg0 -o wg0 -j ACCEPT" "$CONF" || { echo "FAIL: no forwarding rule between peers"; exit 1; }
grep -q "Endpoint = vpn.example.com:51820" "$SB/wg/clients/phone.conf" || { echo "FAIL: wrong client endpoint"; exit 1; }

# re-run with one more client: the existing keys must not be touched
before="$(cat "$SB/wg/keys/server.key" "$SB/wg/keys/esp.key")"
CLIENTS="phone laptop tablet" run
[ "$before" = "$(cat "$SB/wg/keys/server.key" "$SB/wg/keys/esp.key")" ] || { echo "FAIL: re-running rotated the keys"; exit 1; }
[ "$(grep -c '^\[Peer\]' "$CONF")" = 4 ] || { echo "FAIL: the new peer was not added"; exit 1; }

echo "PASS: setup.sh generates the right configuration and is idempotent"

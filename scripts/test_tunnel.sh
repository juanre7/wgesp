#!/usr/bin/env bash
# Checks the tunnel. Run it ON THE VPS:        sudo bash test_tunnel.sh
# or from the dev machine if you have SSH:     WGESP_VPS=user@host scripts/test_tunnel.sh
set -euo pipefail

run() { if [ -n "${WGESP_VPS:-}" ]; then ssh "$WGESP_VPS" "sudo $*"; else eval "$*"; fi; }

echo "== wg0 state =="
run wg show wg0

echo
echo "== ping the ESP32 (10.66.66.6) inside the tunnel =="
if run ping -c 3 -W 2 10.66.66.6 > /dev/null 2>&1; then
    echo "PASS: the ESP32 answers, VPS<->ESP tunnel is up"
else
    echo "FAIL: the ESP32 does not answer at 10.66.66.6"
    echo "  - an empty 'latest handshake' above => the ESP is not reaching the VPS:"
    echo "    check that the WireGuard UDP port is open (provider firewall included),"
    echo "    the DNS of your endpoint, and that the ESP keys are the right ones."
    exit 1
fi

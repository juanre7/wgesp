#!/usr/bin/env bash
# usage: monitor.sh [seconds]  -- dumps the serial console to monitor.log and stdout.
# Non-interactive, so it is agent-friendly. Resets the chip first to capture the boot.
set -euo pipefail
cd "$(dirname "$0")/.."
source scripts/env.sh
# the dialout group is not active in shells opened before the usermod
[ -r "$PORT" ] || exec sg dialout -c "$0 ${1:-}"
SECS="${1:-15}"
esptool.py --port "$PORT" --after hard_reset read_mac >/dev/null 2>&1 || true
stty -F "$PORT" 115200 raw -echo
timeout "$SECS" cat "$PORT" > monitor.log || true
cat monitor.log

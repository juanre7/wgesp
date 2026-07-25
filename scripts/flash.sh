#!/usr/bin/env bash
set -euo pipefail
cd "$(dirname "$0")/.."
source scripts/env.sh
# the dialout group is not active in shells opened before the usermod
[ -r "$PORT" ] || exec sg dialout -c "$0 $*"
idf.py -p "$PORT" flash "$@"

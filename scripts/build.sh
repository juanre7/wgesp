#!/usr/bin/env bash
set -euo pipefail
cd "$(dirname "$0")/.."
source scripts/env.sh
touch sdkconfig.local
SDKCONFIG_DEFAULTS="sdkconfig.defaults;sdkconfig.local" idf.py build "$@"

# source scripts/env.sh -- IDF environment for build/flash
export IDF_CCACHE_ENABLE=1
# the most recent node: after a usbipd re-attach the old one lingers as a ghost
export PORT="${PORT:-$(ls -t /dev/ttyACM* 2>/dev/null | head -1)}"
source "$HOME/esp/esp-idf/export.sh" >/dev/null

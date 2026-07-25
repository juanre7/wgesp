cmake_minimum_required(VERSION 3.16)
list(APPEND EXTRA_COMPONENT_DIRS $ENV{IDF_PATH}/examples/common_components/protocol_examples_common)
include($ENV{IDF_PATH}/tools/cmake/project.cmake)

# The path MTU home<->VPS is 1420 B (measured with `ping -f`), and
# 1420 - 60 of WireGuard overhead (20 IP + 8 UDP + 16 header + 16 authtag)
# = 1360. Anything larger gets fragmented on the way, which is the usual ruin
# for TCP. Measure yours with `ping -f -l N` from a machine on your LAN to the
# VPS: the largest size that gets through, minus the 60 bytes of WireGuard
# overhead, is the value that goes here.
idf_build_set_property(COMPILE_DEFINITIONS "WIREGUARDIF_MTU=1360" APPEND)

# The lwIP NAPT table is not exposed in Kconfig and its defaults (512 entries,
# 30 min TCP timeout) are exhausted by a single phone browsing session in full
# tunnel: the tunnel stays up but stops forwarding. ~28 B per entry.
idf_build_set_property(COMPILE_DEFINITIONS "IP_NAPT_MAX=2048" APPEND)
idf_build_set_property(COMPILE_DEFINITIONS "IP_NAPT_TIMEOUT_MS_TCP=600000" APPEND)
project(wgesp)

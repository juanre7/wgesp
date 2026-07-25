idf_component_register(
    SRCS "src/crypto.c"
         "src/esp_wireguard.c"
         "src/wireguard-platform.c"
         "src/wireguard.c"
         "src/wireguardif.c"
         "src/crypto/refc/blake2s.c"
         "src/crypto/refc/chacha20.c"
         "src/crypto/refc/chacha20poly1305.c"
         "src/crypto/refc/poly1305-donna.c"
    INCLUDE_DIRS "include" "src"
    REQUIRES lwip
    PRIV_REQUIRES esp_netif esp_timer)

target_compile_options(${COMPONENT_LIB} PRIVATE -Wno-unused-result)

# Phase 8: ChaCha20 is 67 % of the cost of encrypting a packet, and at -Os the
# compiler does not unroll the rounds: it spills the 16-word state to memory on
# each of the 10 INNER_BLOCK calls. At -O3 it keeps them in registers. Only
# these files; the rest of the project stays at -Os, because flash is the scarce
# resource here. Measured: 830 -> 622 us per 1400 B packet, for ~12 KB of flash.
set_source_files_properties(
    src/crypto/refc/chacha20.c
    src/crypto/refc/poly1305-donna.c
    src/crypto/refc/chacha20poly1305.c
    PROPERTIES COMPILE_OPTIONS "-O3")

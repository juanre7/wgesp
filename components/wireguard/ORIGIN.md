Vendored from https://github.com/droscy/esp_wireguard commit b737275755f5d5a32da81ad25ca5e3834dec2719 (v0.4.5, 2026-04-17). No modifications other than our own CMakeLists.txt/idf_component.yml and the patches below.

Local patches (grep for `wgesp:` in the sources):

- `src/wireguardif.c`: weak call to `wgesp_client_seen()` with the source IP of
  the decrypted packet. It is the only place where you can see who is talking
  through the tunnel; `main/` uses it for the LED. With no implementation it is
  a no-op, so it does not tie the component to the application.
- `src/wireguardif.c`, `wireguardif_network_rx()`: the original took
  `len = p->len`, the length of **that** pbuf and not of the whole datagram, so
  any datagram chained across several pbufs was processed truncated and lost
  without a word. It is now linearised with `pbuf_coalesce()` and
  uses `p->tot_len`; if there is no memory to join it, the packet is dropped
  **with a warning**. This is a bug in the upstream project and a candidate for
  a patch sent upstream.

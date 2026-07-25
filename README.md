# wgesp — an ESP32-C6 as the front door to your home network

An ESP32-C6 plugged into mains power and your home WiFi opens an **outbound**
WireGuard tunnel to a VPS. From anywhere you connect to the VPS and reach your
home network; if you want, you can also browse the internet with your home public
IP address. **No port forwarding on the router, ever.**

```
phone / laptop ──► VPS (vpn.example.com) ──► ESP32-C6 ──► home network
  10.66.66.10+          10.66.66.1            10.66.66.6    192.168.1.0/24
                                            (WiFi 192.168.1.50)
```

The ESP always initiates the connection and keeps it alive with a 25 s keepalive,
so the home router needs no configuration at all. That is the whole point: it
works behind CGNAT, behind a landlord's router, behind anything that will not let
you open a port.

## Status

Working and in daily use. Measured throughput: **5.4 Mbit/s down and 4.6 up**,
with the chip at 20-30 % CPU. The software crypto ceiling is 26 Mbit/s, so the
limit is the radio and the ISP, not the ESP32. Plenty for the router admin page,
home automation, SSH, banking or modest video; not for large file transfers.

Those numbers are from one particular house, one particular ISP and a board with
RSSI -73 dBm. Yours will differ; `/dump` (below) measures your own.

## What you need

- **An ESP32-C6 board with 2 MB of flash or more.** Developed on a DFRobot Beetle
  ESP32-C6 (DFR1117, [wiki](https://wiki.dfrobot.com/dfr1117)), ESP32-C6FH4 at
  160 MHz. Nothing in the firmware is specific to it except the LED pin.
- **ESP-IDF 5.5** on the build machine.
- **A VPS with a public IP and root access.** `vps/setup.sh` provisions one from
  scratch; if you already run WireGuard there, add the ESP as one more peer
  instead (see `vps/README.md`).

Start at `vps/README.md`: VPS, then DNS, then ESP32.

## Build and flash

```bash
idf.py set-target esp32c6
idf.py build
idf.py -p /dev/ttyACM0 flash monitor
```

Or `scripts/build.sh`, which is the same thing plus `sdkconfig.local`. The log
must show `NAPT active` and then `peer UP`.

Your configuration goes in **`sdkconfig.local`**, at the root of the repo and
listed in `.gitignore` — WiFi credentials and WireGuard keys, which must never
end up in git. `vps/setup.sh` prints the lines to paste there:

```
CONFIG_EXAMPLE_WIFI_SSID="..."
CONFIG_EXAMPLE_WIFI_PASSWORD="..."
CONFIG_WGESP_PRIVATE_KEY="..."
CONFIG_WGESP_PEER_PUBLIC_KEY="..."
CONFIG_WGESP_ENDPOINT="vpn.example.com"
CONFIG_WGESP_LAN_IP="192.168.1.0"
```

Everything else is in `menuconfig` under **wgesp**. With no private key the
firmware boots into WiFi + SNTP only, which is a useful first check.

**Careful**: `sdkconfig.local` is only read when `sdkconfig` is generated. After
editing it, `rm sdkconfig` and rebuild, or you will flash without your change and
conclude the wrong thing.

## The board and its LED

- User LED on **IO15/D13** (`LED_GPIO` in `main/main.c`) — the only one the
  firmware controls:
  - 1 short blink at startup,
  - 2 short blinks when the tunnel comes up (and every time it recovers),
  - 1 blink of 1 s when a client starts talking through the tunnel or has been
    quiet for 60 s (`CLIENTS_IDLE_US` in `main/clients.h`). WireGuard does not
    report connections, so this is as close to "client in/out" as you can get.
- On the Beetle there is a second LED, the charging one, driven by the TP4057 and
  not by the ESP32: it blinks forever when no battery is connected and **no
  firmware can turn it off**. Tape, a battery, or desolder its resistor.

## Adding a new client

All the work happens on the VPS; the ESP is not touched.

**A client that only wants to reach the home network**: nothing to do if its
profile already uses `AllowedIPs = 0.0.0.0/0`; the route reaches it on its own.

**A client that also wants to exit to the internet with the home IP address**:

```bash
scp vps/enable_home_exit.sh root@vpn.example.com:
ssh root@vpn.example.com 'CLIENT=tablet CLIENT_IP=10.66.66.12 bash enable_home_exit.sh'
```

It is idempotent: re-running it rotates nobody's keys and does not restart the
interface. At the end it prints the profile, ready to copy and paste. For a QR
code:

```bash
ssh root@vpn.example.com 'qrencode -t ansiutf8 < /etc/wireguard/wgesp/tablet.conf'
```

Address allocation: `.1` VPS, `.6` ESP32, `.10` onwards clients exiting through
home.

`vps/fix_mtu.sh` sets the per-route MTU and the MSS clamping on the VPS, in both
directions. Run it once after enrolling exit clients — without it, TCP through
the tunnel hits an MTU black hole and crawls.

## Status page

From anywhere already inside the tunnel:

```
http://10.66.66.6/          the page
http://10.66.66.6/txt       the same, plain text for curl and scripts
http://10.66.66.6/dump?mb=50   50 MB of filler, to measure your own throughput
```

Uptime, boot count and reason for the last reboot, peer state, CPU, chip
temperature, heap, NAPT table occupancy and clients seen.

It is read-only on purpose: no forms, no writes. And it only answers those who
arrive through the tunnel — a request to the LAN address is refused when the
connection is opened. Going through WireGuard **is** the authentication.

`CONFIG_WGESP_MDNS` (off by default, costs 38 KB of flash) also publishes it as
`http://wgesp.local/` on the home LAN. Careful: that opens the page to anyone on
your WiFi.

## How it looks after itself

- Retries the WiFi connection forever (`CONFIG_EXAMPLE_WIFI_CONN_MAX_RETRY=-1`).
- Syncs the clock over SNTP before bringing the tunnel up, because WireGuard
  rejects handshakes with a skewed clock, and resyncs every hour.
- If the tunnel has been down for 5 minutes, it reboots itself
  (`CONFIG_WGESP_RESTART_AFTER_MIN`). This is the safety net for the jams nobody
  can anticipate. A full boot takes about 20 s.
- An **RTC watchdog** (always-on domain, its own clock) reboots the chip even if
  the whole CPU is stuck. Tested by deliberately freezing the task that feeds it.
- The **NAPT table** no longer hangs when it fills up: lwIP evicts the oldest
  entry and sends RST, and the firmware warns in the log when eviction starts or
  occupancy passes 75 %.

## Security, and its limits

Whoever reaches the status page has already been through WireGuard, and the page
writes nothing. The keys live in `sdkconfig.local`, outside git.

**The flash is not encrypted.** Anyone with physical access to the board can read
it out and recover the ESP private key and the PSK. That lets them impersonate
*this device* against your VPS — not read anybody's traffic, since WireGuard
rotates session keys per handshake and peers are authenticated by public key. If
a board is lost, delete its peer from the VPS `wg0.conf` and the key is worth
nothing.

Flash encryption in Release mode would close that, but it switches the ROM into
Secure Download Mode, so **the only remaining update path is OTA**, and two OTA
slots do not fit comfortably in 2 MB. On a board with 4 MB or more the order is:
OTA working and verified first, then Secure Boot signing, then silence the log,
then close USB-JTAG by eFuse, and only then Release mode.

## Repo layout

| Path | What it is |
|---|---|
| `main/` | The application: WiFi, SNTP, tunnel, NAPT and the safety reboot |
| `main/status.c` | The status page, the CPU meter and the `/dump` bench |
| `main/crypto_bench.c` | On-chip cipher bench (`CONFIG_WGESP_CRYPTO_BENCH`) |
| `components/wireguard/` | droscy/esp_wireguard, vendored (see `ORIGIN.md`) |
| `vps/` | Server provisioning, client enrolment, MTU and MSS fixes |
| `scripts/` | Build, flash and monitor without interaction, agent-friendly |

## License

MIT (`LICENSE`). `components/wireguard/` is vendored third-party code and keeps
its own BSD-3 license; the two local patches are documented in
`components/wireguard/ORIGIN.md`.

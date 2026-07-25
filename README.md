# wgesp — an ESP32-C6 gateway to your home network

The ESP32-C6 connects to mains power and to the home WiFi network. It starts an
outbound WireGuard tunnel to a VPS. You connect to the VPS from any location and
you get access to the home network. You can also send your internet traffic
through the home connection.

The router does not need port forwarding.

```
phone / laptop ──► VPS (vpn.example.com) ──► ESP32-C6 ──► home network
  10.66.66.10+          10.66.66.1            10.66.66.6    192.168.1.0/24
                                            (WiFi 192.168.1.50)
```

The ESP32 always starts the connection. It keeps the connection open with a
keepalive interval of 25 seconds. Thus the router does not need configuration.
The system operates through CGNAT and through a router that you cannot control.

## Status

The system operates correctly in daily use.

| Data | Value |
|---|---|
| Throughput, download | 5.4 Mbit/s |
| Throughput, upload | 4.6 Mbit/s |
| CPU load at that throughput | 20 % to 30 % |
| Maximum encryption rate | 26 Mbit/s |

The radio and the internet connection cause the limit. The ESP32 does not cause
it. The throughput is sufficient for the router web page, home automation, SSH,
bank applications and video with low quality. The throughput is not sufficient
for large files.

These values are for one house, one internet connection and a signal level of
-73 dBm. Your values will be different. Use the `/dump` function to measure your
system.

## Necessary equipment

- An ESP32-C6 board with a minimum of 2 MB of flash memory. The development
  board was a DFRobot Beetle ESP32-C6 (DFR1117,
  [data](https://wiki.dfrobot.com/dfr1117)). Only the LED pin is applicable to
  this board.
- ESP-IDF 5.5 on the computer that makes the build.
- A VPS with a public IP address and root access.

The script `vps/setup.sh` prepares a new VPS. If the VPS already has WireGuard,
do not use this script. Add the ESP32 as an additional peer. Refer to
`vps/README.md`.

Do the procedure in `vps/README.md` first: the VPS, then the DNS record, then the
ESP32.

## Build and flash procedure

1. Set the target:

   ```bash
   idf.py set-target esp32c6
   ```

2. Make the build:

   ```bash
   idf.py build
   ```

3. Write the firmware to the board and monitor the console:

   ```bash
   idf.py -p /dev/ttyACM0 flash monitor
   ```

The script `scripts/build.sh` does the same build. It also reads
`sdkconfig.local`.

The console must show `NAPT active` and subsequently `peer UP`.

### Configuration

Put your configuration in the file `sdkconfig.local`. This file is in the root
directory of the repository. The file `.gitignore` contains its name. The file
contains the WiFi credentials and the WireGuard keys.

WARNING: Do not put the keys in git. Persons who read the repository can then
get access to your network.

The script `vps/setup.sh` prints these lines:

```
CONFIG_EXAMPLE_WIFI_SSID="..."
CONFIG_EXAMPLE_WIFI_PASSWORD="..."
CONFIG_WGESP_PRIVATE_KEY="..."
CONFIG_WGESP_PEER_PUBLIC_KEY="..."
CONFIG_WGESP_ENDPOINT="vpn.example.com"
CONFIG_WGESP_LAN_IP="192.168.1.0"
```

The other parameters are in `menuconfig`, in the menu **wgesp**. If the private
key is empty, the firmware starts only the WiFi connection and the clock. This
is a good first test.

CAUTION: The build reads `sdkconfig.local` only when it makes the file
`sdkconfig`. If you change `sdkconfig.local`, delete `sdkconfig` and make the
build again. If you do not delete `sdkconfig`, the board gets firmware without
your change.

## The board and the LED

The firmware controls the LED on pin IO15/D13. The parameter `LED_GPIO` in
`main/main.c` sets this pin. The LED gives this data:

| Signal | Condition |
|---|---|
| 1 short flash | The firmware starts. |
| 2 short flashes | The tunnel becomes operational. |
| 1 flash of 1 second | A client starts to send data, or a client is silent for 60 seconds. |

The parameter `CLIENTS_IDLE_US` in `main/clients.h` sets the silent time.
WireGuard does not give data about connections. Thus this is the best available
indication.

The Beetle board has a second LED for the battery charger. The TP4057 device
controls this LED, not the ESP32. The LED flashes continuously when no battery
is connected. No firmware can stop this LED. To stop the light, do one of these
steps:

- Put tape on the LED.
- Connect a battery.
- Remove the resistor of the LED from the board.

## Procedure to add a client

Do all the steps on the VPS. Do not change the ESP32.

If the client must have access only to the home network, and its profile has
`AllowedIPs = 0.0.0.0/0`, no step is necessary. The client receives the route
automatically.

If the client must also send internet traffic through the home connection, do
these steps:

1. Copy the script to the VPS:

   ```bash
   scp vps/enable_home_exit.sh root@vpn.example.com:
   ```

2. Start the script for the new client:

   ```bash
   ssh root@vpn.example.com 'CLIENT=tablet CLIENT_IP=10.66.66.12 bash enable_home_exit.sh'
   ```

The script does not change the keys of the other clients. It does not stop the
interface. You can start it again safely. The script prints the profile of the
new client. To show the profile as a QR code, use this command:

```bash
ssh root@vpn.example.com 'qrencode -t ansiutf8 < /etc/wireguard/wgesp/tablet.conf'
```

The addresses have this sequence: `.1` for the VPS, `.6` for the ESP32, and
`.10` and subsequent addresses for the clients that use the home connection.

CAUTION: Start the script `vps/fix_mtu.sh` on the VPS after you add these
clients. The script sets the MTU for each route and adjusts the TCP MSS in the
two directions. If you do not start it, the tunnel discards large packets and
the throughput becomes very low.

## Status page

You can read the status page from a location in the tunnel:

| Address | Function |
|---|---|
| `http://10.66.66.6/` | The status page |
| `http://10.66.66.6/txt` | The same data as plain text, for scripts |
| `http://10.66.66.6/dump?mb=50` | 50 MB of test data, to measure the throughput |

The page shows this data:

- The time from the last start.
- The number of starts and the cause of the last start.
- The condition of the peer.
- The CPU load and the temperature of the chip.
- The free memory and the quantity of NAPT entries.
- The clients that send data through the tunnel.

The page has no forms and does not write data. The firmware refuses all
connections from outside the tunnel. WireGuard gives the necessary
authentication.

The parameter `CONFIG_WGESP_MDNS` also gives the page the name
`http://wgesp.local/` on the home network. The default condition is off, because
this function uses 38 KB of flash memory.

WARNING: If you set `CONFIG_WGESP_MDNS` to on, all persons on your WiFi network
can read the status page.

## Automatic recovery functions

- The firmware tries to connect to the WiFi network without a limit. The
  parameter is `CONFIG_EXAMPLE_WIFI_CONN_MAX_RETRY=-1`.
- The firmware sets the clock with SNTP before it starts the tunnel, because
  WireGuard refuses a handshake when the clock is not correct. It sets the clock
  again each hour.
- If the tunnel is not operational for 5 minutes, the firmware starts the chip
  again. The parameter is `CONFIG_WGESP_RESTART_AFTER_MIN`. A full start takes
  approximately 20 seconds.
- The RTC watchdog starts the chip again when the CPU stops. This watchdog uses
  the always-on power domain and its own clock. The test procedure stopped the
  related task on purpose.
- When the NAPT table is full, lwIP removes the oldest entry and sends a TCP
  reset. The firmware writes a message to the log when it removes entries, or
  when the table is more than 75 % full.

## Security

Only persons in the tunnel can read the status page, and the page does not write
data. The keys are in the file `sdkconfig.local`, which is not in git.

WARNING: The flash memory is not encrypted. A person with physical access to the
board can read the memory and get the private key and the pre-shared key.

That person can then operate a device that the VPS accepts as this ESP32. But
that person cannot read the data of the other clients. WireGuard makes new
session keys at each handshake. It also identifies each peer by its public key.

If you lose a board, remove its peer from the file `wg0.conf` on the VPS. The
keys of that board then have no value.

Flash encryption in release mode prevents this condition. But release mode also
sets the ROM to secure download mode. Subsequently, OTA is the only method to
update the firmware, and two OTA partitions are too large for 2 MB of flash
memory. On a board with a minimum of 4 MB, do these steps in this sequence:

1. Make OTA operational and do a test.
2. Add the secure boot signature.
3. Set the log level to zero.
4. Disable the USB-JTAG interface with an eFuse.
5. Set flash encryption to release mode.

WARNING: The eFuses are permanent. You cannot remove them. Step 5 prevents all
subsequent use of the USB interface to write the firmware.

## Contents of the repository

| Path | Contents |
|---|---|
| `main/` | The application: WiFi, SNTP, tunnel, NAPT and the automatic start function |
| `main/status.c` | The status page, the CPU meter and the throughput test |
| `main/crypto_bench.c` | The encryption test in the chip (`CONFIG_WGESP_CRYPTO_BENCH`) |
| `components/wireguard/` | droscy/esp_wireguard, with local changes (refer to `ORIGIN.md`) |
| `vps/` | Scripts for the VPS: preparation, clients, MTU and MSS |
| `scripts/` | Scripts to build, to flash and to monitor without an operator |

## License

MIT. Refer to the file `LICENSE`. The directory `components/wireguard/` contains
software from a different source with a BSD-3 license. The file
`components/wireguard/ORIGIN.md` gives data about the two local changes.

---

This document uses ASD-STE100 Simplified Technical English.

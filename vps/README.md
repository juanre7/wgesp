# Bringing up the VPS and the domain

Recommended order: **VPS → DNS → ESP32**. The script prints, at the end,
everything you need for the other two steps.

## 1. VPS

Requirements: Debian or Ubuntu, root/sudo access, and **UDP 51820 open** in the
provider firewall (Hetzner, OVH, AWS, Oracle... they keep it outside the
operating system; the script can only touch ufw/firewalld).

```bash
scp vps/setup.sh USER@YOUR_VPS_IP:
ssh USER@YOUR_VPS_IP
sudo bash setup.sh
```

If your home network is not `192.168.1.0/24`, pass it as a variable:

```bash
sudo LAN_SUBNET=10.0.0.0/24 bash setup.sh
```

The script installs WireGuard, generates the keys (server, ESP32 and every
client), writes `/etc/wireguard/wg0.conf`, enables packet forwarding, opens the
port if ufw/firewalld is running and starts the service. It is **idempotent**:
re-run it to add clients without breaking anything:

```bash
sudo CLIENTS="phone laptop tablet" bash setup.sh
```

**If you already run WireGuard on that VPS**, the script refuses to touch it, on
purpose: overwriting `wg0.conf` would lock out your existing peers. Add the ESP
by hand as one more peer:

```
[Peer]
PublicKey = <the ESP public key>
AllowedIPs = 10.66.66.6/32, 192.168.1.0/24
```

## 2. DNS record

An `A` record pointing at the public IP the script printed. On Cloudflare:

| Field | Value |
|---|---|
| Type | `A` |
| Name | `vpn` (whatever subdomain you like) |
| IPv4 address | the public IP printed by the script |
| Proxy status | **DNS only** (**grey** cloud) |
| TTL | Auto |

WARNING: the orange cloud (*Proxied*) **breaks the VPN**: the Cloudflare proxy
only relays HTTP/HTTPS, and WireGuard is UDP. It has to stay grey.

Check that it resolves before going on: `dig +short vpn.example.com`

## 3. ESP32

Paste into `sdkconfig.local` (at the root of the repo, outside git) the three
`CONFIG_WGESP_*` lines the script printed, together with the WiFi credentials,
and reflash:

```bash
rm sdkconfig && scripts/build.sh && idf.py -p /dev/ttyACM0 flash monitor
```

The ESP log must show `peer UP`.

## 4. Verification

On the VPS:

```bash
sudo bash test_tunnel.sh      # the one in scripts/ of the repo
```

Then import into the WireGuard app on your phone the QR the script printed (or
the file `/etc/wireguard/clients/phone.conf`), **turn the WiFi off** so you go
out over mobile data, and try to open a device on your home network, for example
the router: `http://192.168.1.1`.

## Notes

- `/etc/wireguard/wgesp-details.txt` keeps a copy including the private keys.
  Delete it once everything is configured.
- The ESP32 resolves the domain when it connects; if you change the VPS IP
  address, reboot it.
- Devices at home will see the traffic as coming from the ESP32 address
  (`192.168.1.50`), because it does NAT.

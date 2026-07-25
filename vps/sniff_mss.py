#!/usr/bin/env python3
#
# Checks the MSS advertised inside the tunnel. This VPS has no tcpdump and there
# was no need to install one: raw AF_PACKET with the python3 already there.
#
#   sudo python3 sniff_mss.py 45      # listen for 45 s
#
# Every forwarded packet is seen TWICE, entering and leaving through wg0, so the
# capture shows the before and the after of the clamping on the same screen:
#
#   SYN-ACK 142.251.157.4:443 -> 10.66.66.10  mss=1412   <- what the server sent
#   SYN-ACK 142.251.157.4:443 -> 10.66.66.10  mss=1320   <- clamped, correct
#
# 1320 = the 1360 tunnel MTU - 40. If you see 1380, the return-path fix in
# vps/fix_mtu.sh is not applied.
#
# Read-only: it sniffs and touches nothing.
import socket, struct, sys, time
s = socket.socket(socket.AF_PACKET, socket.SOCK_RAW, socket.ntohs(3))
s.bind(("wg0", 0))
s.settimeout(2)
until = time.time() + int(sys.argv[1])
seen = 0
while time.time() < until and seen < 12:
    try:
        pkt = s.recv(2048)
    except socket.timeout:
        continue
    # wg0 is a point-to-point interface with no ethernet header: it starts at IP
    if len(pkt) < 20 or (pkt[0] >> 4) != 4:
        continue
    ihl = (pkt[0] & 0xF) * 4
    if pkt[9] != 6:                      # protocol != TCP
        continue
    src = ".".join(str(b) for b in pkt[12:16])
    dst = ".".join(str(b) for b in pkt[16:20])
    tcp = pkt[ihl:]
    if len(tcp) < 20:
        continue
    flags = tcp[13]
    if not (flags & 0x02):               # no SYN, no MSS to look at
        continue
    off = (tcp[12] >> 4) * 4
    opts, i, mss = tcp[20:off], 0, None
    while i < len(opts):
        k = opts[i]
        if k == 0: break
        if k == 1: i += 1; continue
        if i + 1 >= len(opts): break
        ln = opts[i+1]
        if k == 2 and ln == 4:
            mss = struct.unpack("!H", opts[i+2:i+4])[0]
        i += max(ln, 2)
    kind = "SYN-ACK" if flags & 0x10 else "SYN    "
    print(f"{kind} {src}:{struct.unpack('!H',tcp[0:2])[0]} -> {dst}:{struct.unpack('!H',tcp[2:4])[0]}  mss={mss}")
    seen += 1

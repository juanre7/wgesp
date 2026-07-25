#!/usr/bin/env bash
# Reads the serial console over COM3 using .NET from Windows (no usbipd).
# It does not touch DTR/RTS, so it does not reset the chip: it shows what is
# happening right now.
# usage: scripts/monitor_win.sh [seconds] [COM3]
set -euo pipefail
cd "$(dirname "$0")/.."
SECS="${1:-30}"
COM="${2:-COM3}"

powershell.exe -NoProfile -Command "
\$p = New-Object System.IO.Ports.SerialPort $COM,115200,None,8,one
\$p.DtrEnable=\$false; \$p.RtsEnable=\$false
\$p.Open()
\$sb = New-Object System.Text.StringBuilder
for (\$i=0; \$i -lt $SECS; \$i++) { Start-Sleep -Milliseconds 1000; [void]\$sb.Append(\$p.ReadExisting()) }
\$p.Close()
\$sb.ToString()" 2>&1 | tr -d '\r' | tee monitor.log

#!/bin/sh
# Shark-Jack-style arming payload. Single button tap runs this (PAYLOAD_SWITCH1).
# Writes a quick network reconnaissance report into /mnt/sd/Loot/.
LOOT=/mnt/sd/Loot
mkdir -p "$LOOT"
TS=$(date -u +%Y%m%dT%H%M%SZ)
OUT="$LOOT/recon_$TS.txt"

{
  echo "## GhostLink recon $TS"
  echo
  echo "### Interfaces"
  ip -br addr || ifconfig
  echo
  echo "### Routes"
  ip route
  echo
  echo "### ARP neighbours"
  ip neigh
  echo
  echo "### Open TCP (local)"
  ss -lntp 2>/dev/null || netstat -lntp
} > "$OUT" 2>&1
echo "recon written -> $OUT"

#!/bin/sh
# On-device installer. Run on the WebBee after adb push'ing this directory to
# /tmp/onboard. Idempotent: safe to re-run after edits.
set -eu
DEST=/root/ghostlink
mkdir -p "$DEST" "$DEST/logs" "$DEST/records" "$DEST/verdicts" \
         "$DEST/layouts" "$DEST/payloads" "$DEST/extensions"
# don't clobber a user-edited config
if [ ! -f "$DEST/config.txt" ]; then cp config.txt "$DEST/config.txt"; fi
cp sniff.py webui.py ducky.py redirect302.py ghostlink.sh button.py "$DEST/"
# hid_enable.sh — manual HID rebind helper, only pushed if present
cp hid_enable.sh "$DEST/" 2>/dev/null || true
cp layouts/*.json "$DEST/layouts/" 2>/dev/null || true
# vendored web assets (xterm.js etc.) served from /xterm.js, /xterm.css, ...
mkdir -p "$DEST/web"
cp web/*.js  "$DEST/web/" 2>/dev/null || true
cp web/*.css "$DEST/web/" 2>/dev/null || true
# payloads: ducky scripts + sh scripts
cp payloads/*.ducky "$DEST/payloads/" 2>/dev/null || true
cp payloads/*.sh    "$DEST/payloads/" 2>/dev/null || true
cp payloads/*.txt   "$DEST/payloads/" 2>/dev/null || true
cp extensions/*.ducky "$DEST/extensions/" 2>/dev/null || true
chmod +x "$DEST"/*.sh "$DEST"/*.py
install -m 0755 S99ghost /etc/init.d/S99ghost
echo "GhostLink installed to $DEST. Start now with:"
echo "    /etc/init.d/S99ghost start"
echo "Or reboot to let init.d handle it."

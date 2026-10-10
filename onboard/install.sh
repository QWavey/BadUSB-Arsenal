#!/bin/sh
# On-device installer. Run on the WebBee after adb push'ing this directory to
# /tmp/onboard. Idempotent: safe to re-run after edits.
set -eu
DEST=/root/ghostlink
mkdir -p "$DEST" "$DEST/logs" "$DEST/records" "$DEST/verdicts" \
         "$DEST/layouts" "$DEST/payloads"
# don't clobber a user-edited config
if [ ! -f "$DEST/config.txt" ]; then cp config.txt "$DEST/config.txt"; fi
cp sniff.py webui.py ducky.py redirect302.py ghostlink.sh "$DEST/"
cp layouts/*.json "$DEST/layouts/" 2>/dev/null || true
cp payloads/*.ducky "$DEST/payloads/" 2>/dev/null || true
chmod +x "$DEST"/*.sh "$DEST"/*.py
install -m 0755 S99ghost /etc/init.d/S99ghost
echo "GhostLink installed to $DEST. Start now with:"
echo "    /etc/init.d/S99ghost start"
echo "Or reboot to let init.d handle it."

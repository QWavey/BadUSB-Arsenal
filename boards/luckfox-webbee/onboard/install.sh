#!/bin/sh
# On-device installer. Run on the WebBee after adb push'ing this directory to
# /tmp/onboard. Idempotent: safe to re-run after edits.
#
# If this script was pushed with Windows CRLF line endings, re-exec after
# stripping them so busybox's `set -eu` doesn't choke on \r.
case "$(head -1 "$0" | od -c | head -1 | awk '{print $(NF-1)}')" in
    *\\r*)
        tr -d '\r' < "$0" > "$0.lf"
        chmod +x "$0.lf"
        exec sh "$0.lf" "$@" ;;
esac
set -eu
DEST=/root/ghostlink
mkdir -p "$DEST" "$DEST/logs" "$DEST/records" "$DEST/verdicts" \
         "$DEST/layouts" "$DEST/payloads" "$DEST/extensions"
# don't clobber a user-edited config
if [ ! -f "$DEST/config.txt" ]; then
    tr -d '\r' < config.txt > "$DEST/config.txt"
fi
# strip CRLF from every .py / .sh / .ducky / .txt we ship — the project
# files are edited on Windows and busybox's `sh` chokes on \r.
for f in *.py *.sh; do
    [ -f "$f" ] || continue
    [ "$f" = "install.sh" ] && continue   # we re-exec'd ourselves above
    # Write a new file and rename it. A running shell script (the supervisor) reads itself by
    # offset, so rewriting it in place while it runs can make it execute mixed old and new text.
    tr -d '\r' < "$f" > "$DEST/$f.new" && mv "$DEST/$f.new" "$DEST/$f"
done
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
# vendored Hak5 extensions
mkdir -p "$DEST/extensions/hak5"
cp extensions/hak5/*.txt   "$DEST/extensions/hak5/" 2>/dev/null || true
cp extensions/hak5/*.ducky "$DEST/extensions/hak5/" 2>/dev/null || true
chmod +x "$DEST"/*.sh "$DEST"/*.py
# Strip CRLF from S99ghost too — busybox init refuses "set -eu\r".
TMP=$(mktemp) || { echo "install: mktemp failed" >&2; exit 1; }
tr -d '\r' < S99ghost > "$TMP"
install -m 0755 "$TMP" /etc/init.d/S99ghost
rm -f "$TMP"

# One-time patch to /etc/init.d/S50usbdevice so it sources identity.env from
# our dir. Idempotent — a sentinel comment stops us from patching twice.
# Keeps a stock pin at S50usbdevice.bak.ghostlink for restore.
S50=/etc/init.d/S50usbdevice
if [ -f "$S50" ] && ! grep -q 'ghostlink identity.env' "$S50"; then
    cp "$S50" "$S50.bak.ghostlink"
    # insert the source line right after the opening `. /etc/profile`
    awk '
      /^\. \/etc\/profile/ && !done {
          print
          print ""
          print "# ghostlink identity.env — sources VID/PID/strings override"
          print "[ -r /root/ghostlink/identity.env ] && . /root/ghostlink/identity.env"
          done = 1; next
      }
      { print }
    ' "$S50.bak.ghostlink" > "$S50.new"
    # swap the four hardcoded USB_* writes for ${…:-stock} fallback form
    sed -i -E \
      -e 's|^(\s*echo )0x2207(.*idVendor)|\1${USB_VID:-0x2207}\2|' \
      -e 's|^(\s*echo )\$\{?PID\}?( *> *\$\{USB_CONFIGFS_DIR\}/idProduct)|\1${USB_PID:-$PID}\2|' \
      -e 's|^(\s*echo )\$SERIAL( *> *\$\{USB_STRINGS_DIR\}/serialnumber)|\1${USB_SER:-$SERIAL}\2|' \
      -e 's|^(\s*echo ")rockchip(".*manufacturer)|\1${USB_MFR:-rockchip}\2|' \
      -e 's|^(\s*echo ")rk3xxx(".*product)|\1${USB_PROD:-rk3xxx}\2|' \
      "$S50.new"
    mv "$S50.new" "$S50"
    chmod +x "$S50"
    echo "patched $S50 (backup at $S50.bak.ghostlink)"
fi

# Boot-time HID decision: S49ghost-hid is pushed next to this script, and hid_boot_patch.sh adds
# the one line in the stock S50usbdevice that reads its flag. Both are safe to run again.
if [ -f S49ghost-hid ]; then
    install -m 0755 S49ghost-hid /etc/init.d/S49ghost-hid
fi
if [ -f hid_boot_patch.sh ]; then
    sh hid_boot_patch.sh || echo "WARN: hid_boot_patch.sh did not patch S50usbdevice (see its output)"
fi

echo "GhostLink installed to $DEST. Start now with:"
echo "    /etc/init.d/S99ghost start"
echo "Or reboot to let init.d handle it."

#!/bin/sh
# Adds the boot-time HID hook to the stock /etc/init.d/S50usbdevice.
#
# The stock script has HID_EN=off. With this one line after it, HID_EN becomes
# on when /tmp/ghost-hid-on exists. S49ghost-hid creates that flag each boot
# when HID is wanted, so the stock bind_functions() links hid.usb0 before the
# UDC is written. No runtime UDC write is involved.
#
# Safe to run twice. Keeps a backup of the file it changes.
# Undo: copy the backup back over S50usbdevice and reboot.

F=/etc/init.d/S50usbdevice
MARK='if [ -e /tmp/ghost-hid-on ]; then HID_EN=on; fi'

if grep -qF "$MARK" "$F"; then
    echo "already patched: $F"
    exit 0
fi
if ! grep -q '^HID_EN=off$' "$F"; then
    echo "no 'HID_EN=off' line in $F; not patched" >&2
    exit 1
fi

cp -p "$F" "$F.bak.before-hid-boot" || exit 1
sed -i "/^HID_EN=off\$/a $MARK" "$F" || exit 1
sh -n "$F" || { echo "syntax check failed; restoring backup" >&2; cp -p "$F.bak.before-hid-boot" "$F"; exit 1; }
grep -n 'HID_EN' "$F"
echo "patched: $F (backup: $F.bak.before-hid-boot)"

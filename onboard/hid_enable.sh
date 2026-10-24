#!/bin/sh
# =============================================================================
# hid_enable.sh — runtime HID enable on Luckfox stock gadget
# =============================================================================
# Writing "" to UDC tears down ffs.adb (FunctionFS backed by /usr/bin/adbd),
# and adbd on the stock image doesn't come back on its own. The host loses
# both adb AND the composite gadget — the device appears bricked.
#
# This script works around that by orchestrating adbd across the rebind,
# with an unconditional rollback watchdog in case the rebind doesn't
# produce a working gadget:
#
#   1. Fork a detached watchdog. In 30s it will: kill the second-attempt
#      adbd, drop the hid.usb0 symlink, rebind UDC, start a fresh adbd.
#      Then the device is back to pre-HID state regardless of what we did.
#   2. Do the rebind: killall adbd → "" > UDC → symlink hid.usb0
#      (if not already) → UDC > UDC → setsid adbd &.
#   3. After we verify /dev/hidg0 exists AND the new adbd is up, we
#      touch /run/ghostlink/.hid_ok which the watchdog polls. If present, the
#      watchdog exits without rolling back.
#
# All stdout goes to /run/ghostlink/hid_enable.log so adb losing output doesn't
# matter. Verify success later with: cat /run/ghostlink/hid_enable.log
# =============================================================================

G=/sys/kernel/config/usb_gadget/rockchip
CFG_DIR=$G/configs/b.1
HID_LINK=$CFG_DIR/hid.usb0
UDC_NAME=$(ls /sys/class/udc 2>/dev/null | head -n1)
LOG=/run/ghostlink/hid_enable.log
OK_MARK=/run/ghostlink/.hid_ok
# Private dir (mode 700): fixed names in /tmp can be pre-created as symlinks.
mkdir -p -m 700 /run/ghostlink
ADBD=/usr/bin/adbd

exec >> "$LOG" 2>&1
echo "=== $(date) hid_enable starting (UDC=$UDC_NAME) ==="

# Pre-configure the HID function parameters while gadget is still bound.
# These writes are idempotent and don't affect the bound state.
if [ ! -s "$G/functions/hid.usb0/report_desc" ]; then
    echo 1 > "$G/functions/hid.usb0/protocol" || echo "WARN: hid protocol write failed"
    echo 1 > "$G/functions/hid.usb0/subclass" || echo "WARN: hid subclass write failed"
    echo 8 > "$G/functions/hid.usb0/report_length" || echo "WARN: hid report_length write failed"
    # Boot keyboard HID descriptor — same bytes as the stock S50usbdevice script.
    printf '\x05\x01\x09\x06\xa1\x01\x05\x07\x19\xe0\x29\xe7\x15\x00\x25\x01\x75\x01\x95\x08\x81\x02\x95\x01\x75\x08\x81\x03\x95\x05\x75\x01\x05\x08\x19\x01\x29\x05\x91\x02\x95\x01\x75\x03\x91\x03\x95\x06\x75\x08\x15\x00\x25\x65\x05\x07\x19\x00\x29\x65\x81\x00\xc0' > "$G/functions/hid.usb0/report_desc"
    echo "HID function configured"
else
    echo "HID function already configured, skipping"
fi

# --- fork the watchdog -------------------------------------------------------
rm -f "$OK_MARK"
(
    exec >> "$LOG" 2>&1
    echo "=== watchdog armed for 30s ==="
    i=30
    while [ "$i" -gt 0 ]; do
        if [ -f "$OK_MARK" ]; then
            echo "watchdog: OK mark found at i=$i, exiting"
            exit 0
        fi
        sleep 1
        i=$((i - 1))
    done
    echo "!!! watchdog ROLLBACK — no OK mark received !!!"
    # Kill anything that might be mid-rebind
    for p in $(ps 2>/dev/null | awk '/adbd/ && !/awk/ {print $1}'); do kill "$p" 2>/dev/null; done
    sleep 0.3
    echo "" > "$G/UDC" 2>/dev/null
    rm -f "$HID_LINK"
    echo "$UDC_NAME" > "$G/UDC" 2>/dev/null
    sleep 0.5
    setsid "$ADBD" </dev/null >/dev/null 2>&1 &
    echo "watchdog rollback done"
) &
WDOG_PID=$!
echo "watchdog pid=$WDOG_PID"

# --- the dangerous bit -------------------------------------------------------
echo "killing adbd"
for p in $(ps 2>/dev/null | awk '/adbd/ && !/awk/ {print $1}'); do kill "$p" 2>/dev/null; done
sleep 0.3

echo "unbinding UDC"
echo "" > "$G/UDC" 2>/dev/null
sleep 0.3

if [ ! -e "$HID_LINK" ]; then
    echo "adding hid.usb0 symlink into $CFG_DIR"
    ln -s "$G/functions/hid.usb0" "$HID_LINK" || echo "ln -s failed"
fi

echo "rebinding UDC=$UDC_NAME"
echo "$UDC_NAME" > "$G/UDC"
sleep 1

echo "restarting adbd"
setsid "$ADBD" </dev/null >/dev/null 2>&1 &

# Give the kernel + adbd a moment to initialize
sleep 2

# --- verify ------------------------------------------------------------------
if [ -e /dev/hidg0 ] && ps 2>/dev/null | grep -q "[a]dbd"; then
    chmod 666 /dev/hidg0 2>/dev/null
    touch "$OK_MARK"
    echo "SUCCESS: /dev/hidg0 present AND adbd running — watchdog will exit cleanly"
    exit 0
else
    echo "FAIL: /dev/hidg0=$(ls /dev/hidg0 2>&1), adbd=$(ps|grep adbd|grep -v grep)"
    echo "letting watchdog roll back"
    exit 1
fi

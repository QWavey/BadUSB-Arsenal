# HID on the Luckfox WebBee — why it's hard

## TL;DR

The stock Luckfox gadget `rockchip` has three bound functions:

| Function     | Backing             | Survives UDC rebind? |
|--------------|---------------------|----------------------|
| `rndis.gs0`  | kernel driver       | yes                  |
| `ffs.adb`    | **userspace adbd**  | **no**               |
| would-be `hid.usb0` | kernel driver | yes                  |

Writing to `UDC` to unbind the gadget tears down the FunctionFS endpoints
that `adbd` is attached to. `adbd` on Rockchip's stock Buildroot was
spawned once at boot and doesn't come back on its own. On rebind the kernel
waits for a userspace process to re-feed `/dev/ffs/*` with the ADB
descriptors — nobody shows up — so **the entire composite gadget fails to
come up** and the device vanishes from Windows / adb until a physical
power-cycle.

That is why every attempt to enable HID at runtime bricked the device.

Sources:
- Linux [configfs USB gadget docs](https://docs.kernel.org/6.8/usb/gadget_configfs.html)
  — "In order to enable the gadget it must be bound to a UDC". Binding
  activates FunctionFS and requires the backing daemon.

## Safe-by-default state

To stop this from re-bricking the device, two guards are in place:

1. `webui.py` **does not** call `rebind-gadget` automatically on config
   save. Setting `ENABLE_HID=TRUE` in the UI just writes the config —
   nothing happens until the user explicitly opts into the rebind path.
2. `ghostlink.sh` only runs the rebind at startup if **both**
   `ENABLE_HID=TRUE` **and** `HID_FORCE_REBIND=TRUE` are set.
   `HID_FORCE_REBIND` defaults to `FALSE` and is intentionally not in
   the shipped `config.txt`.

Treat `HID_FORCE_REBIND=TRUE` as "I accept that this may brick the device
over USB and I have a recovery channel".

## Enabling HID safely (two routes)

### Route A — serial console (preferred)

Solder a 3V3 USB-UART dongle onto the WebBee's TX/RX pads. You now have a
recovery channel that's independent of USB gadget. Then:

1. `ENABLE_HID=TRUE` and `HID_FORCE_REBIND=TRUE` in `/root/ghostlink/config.txt`.
2. Reboot.
3. If the gadget comes back with HID: done. `/dev/hidg0` exists.
4. If it doesn't: serial-console in, `rm /sys/kernel/config/usb_gadget/rockchip/configs/*/hid.usb0`, `echo "<udc>" > /sys/kernel/config/usb_gadget/rockchip/UDC`, device recovers.

### Route B — SSH watchdog across the rebind (no serial needed)

Not yet implemented. The design:

1. Pre-warm an SSH connection to `root@172.32.0.93` over usb0 (RNDIS keeps
   working across the UDC flap for ~500 ms).
2. From that SSH session, run a wrapper that:
   - Spawns a **watchdog** detached: 30 s sleep, then unconditionally
     remove the `hid.usb0` symlink, `echo "" > UDC`, `echo "<udc>" > UDC`,
     restart `adbd`.
   - Does the rebind orchestration: `killall adbd`, `echo "" > UDC`,
     `ln -s functions/hid.usb0 configs/b.1/hid.usb0`, `echo "<udc>" > UDC`,
     `/path/to/adbd &` (so FunctionFS reclaims the ADB endpoints).
   - Pings the webui to confirm reachability. If the ping lands, cancels
     the watchdog.
   - If anything hangs, the watchdog restores everything after 30 s.

The hard part is finding the correct `adbd` invocation on this specific
buildroot (it's not always just `adbd` on PATH — may be spawned by an
init script with specific args). This needs on-device inspection of
`/etc/init.d/*adb*` and `/etc/init.d/rcS` before implementation.

## What's safe to use right now

- RNDIS + ADB + web UI + dnsmasq + sniff → all fine.
- HID → leave it off until Route A hardware is in place or Route B is
  properly tested with a cheap "canary" (something you can afford to
  brick for a bit).

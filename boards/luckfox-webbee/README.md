# GhostLink: passthrough firmware for the Luckfox WebBee

A fast-boot Buildroot-based firmware that turns the Luckfox WebBee (RV1106,
SPI NAND, USB-C, 10/100 Ethernet, SD slot) into a transparent **man-in-the-wire**
device: it bridges the LAN while recording every byte to the SD card and
reacting to what it sees in real time.

Design-cousins (and prior art it borrows from): Hak5 **LAN Turtle** (USB-ethernet
MITM), **Packet Squirrel** (dual-port passthrough + rec), **Shark Jack**
(payload.sh + switch modes), **Bash Bunny** (BadUSB personality). The WebBee is
cheaper, has more RAM, and runs stock Buildroot, but the stock image is a
blank slate. GhostLink is the firmware built on that slate.

## What's new

Other tools make you choose between *recording everything* (opaque pcap) and
*reacting to flows* (narrow fixed payloads). GhostLink does both:

- The kernel bridge forwards packets at wire rate (zero CPU on the hot path).
- A shadow fiber reads an AF_PACKET mirror on its own core and co-writes
  **two** files per rotation window:
  - `Records/rec_NNNNNN.pcap`: the raw stream, Wireshark-compatible.
  - `Records/rec_NNNNNN.jsonl`: one JSON line per frame, with
    `{frame#, ts, flow_hash, size, rule?, verdict?, detail?}`.
- The sidecar JSONL stream makes the pcap **queryable in milliseconds** from
  the Web UI (`GET /api/query?rule=REDIRECT&host=google.com`) instead of being a
  byte-pile you grep later. The pcap stays untouched; the verdicts are the
  index.
- `config.txt` works as a compiler input: `REDIRECT=TRUE` with
  `REDIRECT_FROM=google.com` compiles to nftables + a DNS worker on boot in
  under 50ms, and **hot-reloads** on save without reboot.

Prior-art check: Hak5 devices stream pcaps and run isolated per-switch
payloads, but none co-write a per-frame verdict channel; and their configs are
static per-boot. Suricata / Zeek produce rich logs but need a real server rather
than a 1.2GHz Cortex-A7 behind a USB-C port.

## Hardware

- **Luckfox WebBee** (Rockchip **RV1106**, Cortex-A7 up to 1.2GHz)
- 128MB SPI NAND, 128/256MB DDR, SDHC slot
- USB-C: device-mode OTG (NCM/RNDIS/HID/MSC composite gadget)
- 10/100 Ethernet (`eth0`), onboard PHY
- One button, one status LED

## SD card layout

```
/config.txt              master config (hot-reloads on save)
/Records/                rec_NNNNNN.pcap + rec_NNNNNN.jsonl
/Payloads/               *.sh (Shark-Jack style) and *.ducky (BadUSB)
/Languages/              us.json, de.json, fr.json, ...  (hak5 format)
/WebUI/                  user override for index.html / app.js / style.css
/Spoofhosts/             /etc/hosts files for DNS_SPOOF
/Honeypot/               session logs when HONEYPOT=TRUE
/Modules/                .so plugins, VPN configs, inject.js, MITM CA
/Scripts/                cron.txt and anything you want on PATH
/Loot/                   payload output
/Logs/                   ghostlink.log
/Firmware/               OTA bundle + signing pubkey
```

## config.txt: 35 keys

See `docs/CONFIG.md`. Quick peek:

```
USB=TRUE                 PASSTHROUGH_LAN=TRUE   ASSIGN_IP=TRUE
WEBUI=TRUE               LOG_TO_SD=TRUE          VERDICT_STREAM=TRUE
DEADNET=FALSE            REDIRECT=FALSE          DNS_SPOOF=FALSE
HID_ENABLED=TRUE         SSH=TRUE                MDNS=TRUE
HONEYPOT=FALSE           WIFI=FALSE              VPN=FALSE
CRON=TRUE                LED=TRUE                WATCHDOG=TRUE
FLOWS=TRUE               RATE_LIMIT=FALSE        NTP=TRUE
ALERTS=TRUE              INJECT=FALSE            ISOLATE_CLIENTS=FALSE
SSL_STRIP=FALSE          OTA=FALSE               CAPTIVE_PORTAL=FALSE
SIBLING_BEACON=FALSE     MAC_CLONE=FALSE         PANIC_SCRUB=TRUE
```

Any `TRUE/FALSE` can be edited live via `/api/config` or by SSH + `touch
config.txt`. Services reconcile within ~1s.

## Boot timeline

Target: **ready to pass traffic in under 2 seconds** from U-Boot handoff.

```
t=0.00s   kernel exec -> /sbin/init
t=0.01s   S99ghost -> /usr/sbin/ghostlink
t=0.01s   mount /mnt/sd (vfat/exfat/ext4 auto-detect)
t=0.02s   parse config.txt
t=0.03s   configure logging
t=0.04s   USB gadget composite (NCM + HID) up
t=0.08s   bridge up (br0 = eth0 + usb0)
t=0.09s   recorder + httpd + hid + optional attackers spawn
t=0.11s   button watcher + LED
t=0.12s   READY (verdicts flowing, pcap writing, WebUI reachable)
```

Everything is one binary (`out/ghostlink`, ~180KB stripped). No systemd, no
shell pipelines, no python.

## Building

```bash
# Luckfox toolchain:
export PATH=$HOME/luckfox-pico/tools/linux/toolchain/arm-rockchip830-linux-uclibcgnueabihf/bin:$PATH
./scripts/build.sh
# -> out/ghostlink  (arm, hard float, uclibc)
```

Then stage into a WebBee buildroot rootfs:

```bash
make install SYSROOT=/path/to/luckfox-pico/output/image/rootfs
# or pack a ready-to-flash SD overlay:
make pack
```

## Flashing

The stock Luckfox WebBee flash procedure applies. See `docs/HARDWARE.md`.
Overlay the compiled `ghostlink` into `rootfs/usr/sbin/`, drop
`S99ghost` into `rootfs/etc/init.d/`, rebuild `rootfs.img` with the Luckfox
SDK, flash the usual way with SocToolKit or `upgrade_tool uf update.img`.

Then burn the `sdcard-template/` contents to a MicroSD:

```bash
sudo ./scripts/format-sdcard.sh /dev/sdX
```

## Accessing

- Plug the WebBee's USB-C into a laptop.
- It appears as a USB Ethernet (NCM) device, offers DHCP on 172.16.24.0/24.
- WebUI: <http://172.16.24.1/>  (Basic auth, user `root`, password = `WEBUI_PASS`; the WebUI stays off until a password or `API_TOKEN` is set)
- SSH: `ssh root@172.16.24.1`  (password login and root login default to FALSE; see `SSH_PASSWORD_AUTH` and `SSH_ROOT`. The sshd config is not generated by this repo.)
- mDNS: `http://ghostlink.local/`

## The verdict stream (TL;DR)

Every frame written to pcap gets one JSONL line. Rule hits add fields:

```jsonl
{"f":12,"t":1760120234.991205,"sz":74,"flow":"8ab3c4de"}
{"f":13,"t":1760120234.991910,"sz":126,"flow":"8ab3c4de","rule":"REDIRECT","v":"HIT","d":"google.com"}
{"f":14,"t":1760120234.992701,"sz":74,"flow":"7799aabb"}
```

`/api/query?rule=REDIRECT` returns matching lines with ns-timestamp and flow,
so the Web UI can jump straight to the pcap byte-offset of interest.

## HID / BadUSB

HID keyboard gadget exposes `/dev/hidg0`. `.ducky` payloads run a subset of
DuckyScript 1.0 (STRING, DELAY, GUI/CTRL/ALT/SHIFT combos, F-keys, REPEAT,
DEFAULTDELAY). Keyboard layouts are loaded from `/Languages/<layout>.json` in
the same JSON format as [hak5/usbrubberducky-payloads](https://github.com/hak5/usbrubberducky-payloads/tree/master/languages).

Trigger a payload three ways:

- `HID_TRIGGER=BOOT`     fires `HID_PAYLOAD` after `HID_DELAY_MS`.
- `HID_TRIGGER=BUTTON`   tap the button.
- `HID_TRIGGER=WEBUI`    click the Fire button in the WebUI, or
                         `POST /api/hid` with the payload path.

## DeadNet (opt-in)

A native-C re-implementation of [flashnuke/deadnet](https://github.com/flashnuke/deadnet):

- ARP replies on the /24 claiming to be the gateway with a bogus MAC.
- Multicast RA (`ff02::1`) with router_lifetime=0 so IPv6 clients deprecate
  the real gateway.

Enabled only when `DEADNET=TRUE`. `DEADNET_SPARE` skips named IPs. Default
cadence 800ms. **This is a denial-of-service tool. Only enable on networks you
own or have written authorization to test on.**

## Legal

GPL-3.0. Firmware intended for authorized security research, lab setups, and
networks you own. Don't deploy it against anyone else's traffic.

## Credits

- Hak5: the whole lineage of pocket pentest tooling (Turtle, Squirrel, Shark
  Jack, Bash Bunny), format conventions (payload.sh, LED command, layout JSON).
- [flashnuke/deadnet](https://github.com/flashnuke/deadnet): the IPv4+v6
  havoc technique; GhostLink's `deadnet.c` is a straight port in native C.
- Luckfox: the WebBee hardware and buildroot SDK.

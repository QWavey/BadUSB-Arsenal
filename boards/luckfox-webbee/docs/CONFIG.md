# config.txt reference

This page documents the C build in `src/`. The Python build in `onboard/` reads some different key names; see the last section.

All keys are UPPER_CASE. Booleans: `TRUE`/`FALSE` (`YES`/`NO`, `ON`/`OFF` and `1`/`0` also work). Any other boolean is rejected and logged. Blank value = default. Whole numbers are decimal, so `010` is ten. A `#` that starts a value makes it a comment, so the key keeps its default.
Lines starting with `#` are comments. A `#` starts an inline comment only at the
start of a value or after whitespace, so `WEBUI_PASS=pa#ss` keeps its `#`.
A value longer than its field is cut and the boot log warns.
Unknown keys land in the boot log as a loud warning (so typos surface).

Hot-reload: writing a new `config.txt` (via WebUI or `touch`) bumps
`g_cfg_gen`, and the supervisor reconciles affected services within ~1s. Most
keys apply without reboot; those that don't are listed below.

## Groups

### 1. Core
| key | type | default | notes |
|-----|------|---------|-------|
| HOSTNAME | str | ghostlink | kernel hostname + mDNS base |
| DEVICE_NAME | str | WebBee-Ghost | shown in WebUI |
| TIMEZONE | str | UTC | |
| LOCALE | str | en_US.UTF-8 | |
| FAST_BOOT | bool | TRUE | skip non-essential init stages |
| BOOT_LED | bool | TRUE | blink boot progress |

### 2. USB personality (reboot to apply)
| key | type | default | notes |
|-----|------|---------|-------|
| USB | bool | TRUE | master gadget switch |
| USB_MODE | enum | NCM | NCM, RNDIS, COMPOSITE. HID and storage are set by USB_HID and USB_STORAGE |
| USB_VID | hex | 0x1D6B | |
| USB_PID | hex | 0x0137 | |
| USB_MANUFACTURER | str | GhostLink | |
| USB_PRODUCT | str | WebBee Passthrough | |
| USB_SERIAL | str | AUTO | AUTO = derive from the eth0 MAC address, stable across reboots |
| USB_HID | bool | TRUE | expose /dev/hidg0 |
| USB_STORAGE | bool | FALSE | expose SD as MSC too |

### 3. Network passthrough
| key | type | default | notes |
|-----|------|---------|-------|
| PASSTHROUGH_LAN | bool | TRUE | |
| NETMODE | enum | BRIDGE | BRIDGE, NAT, TRANSPARENT, VPN, ISOLATE |
| ASSIGN_IP | bool | TRUE | run tiny DHCP on host side |
| DHCP_RANGE | str | 172.16.24.10,172.16.24.60 | start,end: two IPv4 addresses, start not above end |
| GATEWAY_IP | ip | 172.16.24.1 | |
| DNS_UPSTREAM | csv | 1.1.1.1,9.9.9.9 | |
| MAC_CLONE | bool | FALSE | |
| MAC_CLONE_FROM | iface | eth0 | |
| STATIC_IP, STATIC_NETMASK, STATIC_GATEWAY | | DHCP if empty. STATIC_IP and STATIC_GATEWAY are IPv4 addresses. STATIC_NETMASK is a prefix length from 0 to 32, for example 24 |

Validated at load: `GATEWAY_IP`, `DHCP_RANGE` and `STATIC_*` accept only digits,
dots and commas. `MAC_CLONE_FROM` accepts an interface name. `PAYLOAD_SWITCH1-3`
accept a `/path` made of `A-Za-z0-9._/-` with no `..`. A rejected value keeps the
field's previous value and logs a warning.

### 4. Recording
| key | type | default | notes |
|-----|------|---------|-------|
| LOG_TO_SD | bool | TRUE | master recorder switch |
| RECORD_FORMAT | enum | PCAPNG | PCAP, PCAPNG |
| RECORD_ROTATE_MB | int | 64 | per-file size cap |
| RECORD_RING_FILES | int | 32 | oldest deleted when exceeded |
| RECORD_FILTER | bpf | (none) | eg "tcp or arp" |
| VERDICT_STREAM | bool | TRUE | the sidecar JSONL |
| VERDICT_INCLUDE_PAYLOAD | bool | FALSE | add 64B payload per verdict |

### 5. WebUI
| key | type | default | notes |
|-----|------|---------|-------|
| WEBUI | bool | TRUE | |
| WEBUI_PORT | int | 80 | |
| WEBUI_BIND | ip | 0.0.0.0 | |
| WEBUI_USER / WEBUI_PASS | str | root / (blank) | WebUI stays off while both WEBUI_PASS and API_TOKEN are blank. Plain HTTP only. |
| WEBUI_LANG | str | en | |

### 6. DeadNet
| key | type | default | notes |
|-----|------|---------|-------|
| DEADNET | bool | FALSE | opt-in only |
| DEADNET_IPV4 | bool | TRUE | ARP havoc |
| DEADNET_IPV6 | bool | TRUE | RA lifetime=0 flood |
| DEADNET_INTERVAL_MS | int | 800 | |
| DEADNET_SPARE | csv | | IPs that stay reachable |
| DEADNET_SELECTIVE | bool | FALSE | only hit DEADNET_TARGETS |
| DEADNET_TARGETS | csv | | IPs/MACs |

### 7. Redirect
| key | type | default | notes |
|-----|------|---------|-------|
| REDIRECT | bool | FALSE | |
| REDIRECT_FROM | csv | | hosts (supports *.sub.example) |
| REDIRECT_TO | url | | one target, scheme optional |
| REDIRECT_PROTO | enum | AUTO | AUTO, HTTP, HTTPS, DNS_ONLY |
| REDIRECT_PRESERVE_PATH | bool | TRUE | |
| REDIRECT_ALL | bool | FALSE | every site the USB client opens goes to REDIRECT_ALL_TO, except REDIRECT_EXEMPT and the target itself. Only the USB client (the PC) is affected. Off by default. |
| REDIRECT_ALL_TO | url | | required when REDIRECT_ALL is TRUE; must be http:// or https:// with a host |
| REDIRECT_EXEMPT | csv | anthropic.com,claude.ai,claude.com,claudeusercontent.com,msftconnecttest.com,msftncsi.com,windowsupdate.com | domains that keep resolving normally (suffix match). Empty means this default list. |

Notes for REDIRECT_ALL: the DNS side answers AAAA with no address, so the client uses IPv4 through this resolver. For HTTPS interception without certificate warnings, the device generates a local CA on first use (stored in `/root/ghostlink/certs/`). Download the CA certificate via `/api/certs/ca` and install it manually on your test PC — after that, HTTPS redirects are trusted (the device uses SNI to mint per-host certs signed by your local CA). This is opt-in: if you don't install the CA, users see a certificate warning as before.

### 8-13. DNS spoof / Honeypot / HID / Payloads / SSH / mDNS
See the inline comments in `sdcard-template/config.txt` — same format.

### 14-20. Loot, VPN, SSL strip, WiFi, Cron, LED, Alerts
Same. VPN and SSL strip are stubs pending Modules support.

### 21-25. Flows, Rate limit, Record schedule, NTP, Watchdog
`FLOWS=TRUE` enables the live flow table in WebUI. `WATCHDOG` arms the
hardware watchdog with userspace ping; a stuck ghostlink reboots in
`WATCHDOG_TIMEOUT_S`.

### 26-35. Panic, OTA, Logging, Modules, Sibling, Captive portal, Inject,
### Isolate clients, API token, Factory reset

`PANIC_HOLD_S` + `PANIC_SCRUB` — hold button for the configured seconds and
`/Records` is cryptographically scrubbed before reboot. Off by default: set
`PANIC_SCRUB=TRUE` to opt in.

`FACTORY_RESET_TAPS` — tap the button that many times inside 3s to wipe
config.txt and reboot clean.

## Python build (onboard/)

The device runs the Python build, which reads these names. They differ from the C build above.

| C build (this page) | Python build | Python default | Notes |
|---|---|---|---|
| `LOG_TO_SD` | `LOG` | `TRUE` | recorder switch |
| `NETMODE` (BRIDGE, NAT, ...) | `NET_MODE` | `NAT` (shipped in config.txt; `PASSIVE` if the key is absent entirely) | `PASSIVE` (no eth0 traffic handling), `NAT` (`natd.py`: the PC stays on the device's own isolated 172.32.0.0/16 network via usb0 and is reachable at the fixed 172.32.0.93 regardless of the real LAN; the device itself reaches the real LAN by translating through a spare address on it), or `FORWARD` (`forwarder.py`: transparent L2 bridge -- the PC's usb0 DHCP request crosses to the real LAN and it gets a real-LAN lease instead, so 172.32.0.93 becomes unreachable from the PC unless `detect_and_bind_lan_ip` finds the real LAN's own address for it; useful when you specifically want the PC to appear directly on the LAN, e.g. for transparent inline sniffing of genuinely real-LAN traffic) |
| none | `AUTO_FORWARD` | `TRUE` | also forwards while eth0 has link |
| `RECORD_ROTATE_MB` (64) | `RECORD_ROTATE_KB` | `1024` | per-file cap in KB, not MB |
| `RECORD_RING_FILES` (32) | `RECORD_RING_FILES` | `4` | same name, different default |
| `WEBUI_PASS` (blank = WebUI off) | `WEBUI_PASS` | `ghostlink` | used when the key is absent; a blank value never logs in |
| `API_TOKEN` | none | | the Python build has no API token |
| none | `WEBUI_REQUIRE_AUTH` | `FALSE` | `TRUE` makes the admin endpoints require a session |

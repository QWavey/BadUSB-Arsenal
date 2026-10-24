# config.txt reference

All keys are UPPER_CASE. Booleans: `TRUE`/`FALSE` only. Blank value = default.
Lines starting with `#` are comments. Inline `# ...` comments are stripped.
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
| USB_MODE | enum | NCM | NCM, RNDIS, HID, STORAGE, COMPOSITE |
| USB_VID | hex | 0x1D6B | |
| USB_PID | hex | 0x0137 | |
| USB_MANUFACTURER | str | GhostLink | |
| USB_PRODUCT | str | WebBee Passthrough | |
| USB_SERIAL | str | AUTO | AUTO = derive from chip id |
| USB_HID | bool | TRUE | expose /dev/hidg0 |
| USB_STORAGE | bool | FALSE | expose SD as MSC too |

### 3. Network passthrough
| key | type | default | notes |
|-----|------|---------|-------|
| PASSTHROUGH_LAN | bool | TRUE | |
| NETMODE | enum | BRIDGE | BRIDGE, NAT, TRANSPARENT, VPN, ISOLATE |
| ASSIGN_IP | bool | TRUE | run tiny DHCP on host side |
| DHCP_RANGE | str | 172.16.24.10,172.16.24.60 | |
| GATEWAY_IP | ip | 172.16.24.1 | |
| DNS_UPSTREAM | csv | 1.1.1.1,9.9.9.9 | |
| MAC_CLONE | bool | FALSE | |
| MAC_CLONE_FROM | iface | eth0 | |
| STATIC_IP, STATIC_NETMASK, STATIC_GATEWAY | | DHCP if empty |

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

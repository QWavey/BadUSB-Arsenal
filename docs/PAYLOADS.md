# Payloads

Two independent payload systems, inspired by Hak5 but merged under one device.

## Shark-Jack-style shell payloads

Place `.sh` files anywhere under `/Payloads/`. Point `PAYLOAD_SWITCH1`,
`PAYLOAD_SWITCH2`, `PAYLOAD_SWITCH3` at them. The button fires them by
tap-count:

- 1 tap  → SWITCH1
- 2 taps → SWITCH2
- 3 taps → SWITCH3

Tap window is `PAYLOAD_TAP_WINDOW_MS` (default 800ms). Scripts run as root,
with `/mnt/sd` already mounted. Dump loot into `/mnt/sd/Loot/`.

Example (`recon.sh`):

```sh
#!/bin/sh
OUT=/mnt/sd/Loot/recon_$(date -u +%Y%m%dT%H%M%SZ).txt
{
  ip -br addr
  ip route
  ip neigh
  ss -lntp
} > "$OUT"
```

Available helpers: anything busybox ships + `ghostctl` (todo) to interact
with the running firmware (toggle features, add verdict labels, etc).

## DuckyScript (HID / BadUSB)

Place `.ducky` files under `/Payloads/`. Set `HID_PAYLOAD` to the filename
and pick a trigger (`BOOT`, `BUTTON`, `WEBUI`). Supported DuckyScript:

| command | notes |
|---------|-------|
| `REM ...` | comment |
| `STRING ...` | types the rest of the line via `HID_LAYOUT` |
| `DELAY <ms>` | wait |
| `DEFAULTDELAY <ms>` | inserted between every subsequent command |
| `ENTER / TAB / ESC / BACKSPACE / DELETE / SPACE` | one keystroke |
| `UP / DOWN / LEFT / RIGHT / HOME / END / PAGEUP / PAGEDOWN` | nav |
| `F1 .. F12` | function keys |
| `CTRL / ALT / SHIFT / GUI / WIN / COMMAND / SUPER <key>` | modifier combos |
| `REPEAT <n>` | repeat previous line (STRING only, currently) |
| `PRINTSCREEN / CAPSLOCK / INSERT` | one keystroke |

Keyboard layouts come from `/Languages/<layout>.json` in the Hak5 format
(`{"a":[0,4], "A":[2,4], ...}`), the first value is the modifier byte and the
second is the HID keycode.

## Automated triggers

- `HID_TRIGGER=BOOT`   — fires `HID_PAYLOAD` once, `HID_DELAY_MS` after the
  host enumerates.
- `HID_TRIGGER=BUTTON` — button-tap fires it. Non-conflicting with the
  shell-payload tap-count — only used when the tap count doesn't match a
  configured `PAYLOAD_SWITCHn`.
- `HID_TRIGGER=WEBUI`  — click Fire in the Web UI, or `POST /api/hid` with
  the path.
- `HID_TRIGGER=WEBHOOK` — `/api/hid` with an API token header, same effect.

## Cron

`CRON=TRUE` and `/Scripts/cron.txt` runs standard cron syntax. Great for
scheduled recons: `0 */6 * * * /mnt/sd/Payloads/recon.sh`.

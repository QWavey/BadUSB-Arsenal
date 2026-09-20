# ESP32-P4 Hub Keylogger / HID Passthrough

Full **USB HID passthrough on a single cable** for the Spotpear
**ESP32-P4-MINI-B-M-EXT** board — a USB keyboard plugs into the on-board CH334
hub, the ESP32-P4 reads it as a host, and simultaneously presents itself to the
PC as a HID keyboard on the *same* Type-C we flash from. No hardware mod, no
EFUSE burn.

Windows sees the ESP32-P4 as `VID 303A / PID 4B44` with `bcdDevice 0x0201`
(HID keyboard + mouse composite). The custom PID + bcdDevice revision (set in
`firmware/sdkconfig.defaults`) keeps Windows from reusing a stale driver+devnode
cache entry for the default `0x4004` — a fresh PID forces a clean enumeration.
Every keystroke you make on the wired keyboard shows up in Notepad / your
terminal / anywhere the PC treats keyboard input as text.

**Keyboard layouts**: the decoder ships with US ANSI, German QWERTZ, and UK
layouts (`firmware/main/kb_layouts.{h,c}`), default DE. The layout is
switchable **at runtime from the web UI** (Settings → Keyboard layout): the
C6 writes a scratch SDIO register, the P4 polls it and calls `kb_layout_set()`
— no reflash. AltGr is decoded properly on DE (RALT = AltGr → €, @, ç, ², ³, …),
and RALT is excluded from combo capture on DE so AltGr+key never logs as a
fake `[RALT + …]` combo.

**Known issues (mitigated in firmware, re-verify on hardware)**:

1. **Port-flap on cold boot** — the P4's USB Host doesn't always pick up the
   keyboard on the first plug into a hub port. Mitigations in place: a 12-attempt
   `port_flap_watchdog` (`hid_host_task.c`) that pokes the hub / full-resets the
   HID stack when nothing attaches, a 250 ms settle window + explicit
   all-keys-released flush on every mount in `forward_task`, and a flush on
   keyboard disconnect. Not a data-path bug — once the port sticks it works for
   the whole session.
2. **SHIFT/TAB logging** — Shift is deliberately excluded from combo capture
   (only Ctrl/Alt/Win form combos), so Shift+Tab logs `[TAB]` rather than
   `[SHIFT + TAB]`; the PC still receives the full 8-byte report including the
   modifier. If you rely on the log distinguishing shifted Tab, that is the
   known gap.

**Web UI + persistent log**: the on-board ESP32-C6 runs an independent Wi-Fi
AP (`P4-KeyLogger` / `changeme123`) that hosts a small web UI at
http://192.168.4.1. Keystrokes travel from the P4 to the C6 over the **SDIO
bus** that's already wired on the M-EXT (GPIO 14-19 + reset on GPIO54, no
jumper needed — the P4 boots as SDIO host, resets the C6, and streams frames),
so the UI shows a live stream, records to SPIFFS with a 1.6 MB total budget
(current + rotated old file — sized so both always fit the 2 MB SPIFFS
`storage` partition), and offers download / delete / start / stop /
auto-download / LED / layout controls. See `c6-firmware/` for the source.

## Web flasher — TODO

This project **is not yet published to** [qwavey.github.io/flashers](https://qwavey.github.io/flashers/).
Once it is, the flow for a new user should be:

1. **Connect the ESP32-P4 first** (core Type-C on the MINI). Click *Flash P4*.
2. **Move the cable to the C6 side** (U4 with switch B = 1 BOOT). Click
   *Flash C6*.
3. **Return switch B to 0 (WORK)**, plug the cable back into the core Type-C,
   let both boards power-cycle.
4. Test: PC should enumerate a HID keyboard (`VID 303A / PID 4B44`). Connect
   a phone to Wi-Fi `P4-KeyLogger` (password `changeme123`) and open
   http://192.168.4.1 to see the live log.

Two artifacts are produced by the CI / local build and need to be uploaded to
the flasher page:

- `firmware/build/hid_passthrough.bin` — flashed to the P4 at 0x10000 (plus
  the bootloader at 0x2000 and partition table at 0x8000)
- `c6-firmware/build/p4_keylogger_c6_ui.bin` — flashed to the C6 at 0x10000
  (plus bootloader at 0x0 and partition table at 0x8000, with a custom
  `partitions.csv` that includes a 2 MB SPIFFS `storage` region)

## Why this needed real work

Both halves of the passthrough hit board-specific and IDF-specific brick walls.

### Brick wall 1 — CH334 hub blocks FS/LS keyboards

Every USB keyboard is Full-Speed or Low-Speed. Every port on this board is
downstream of a High-Speed CH334F hub. ESP-IDF's `usb_host` stack (v5.3 through
current master) does not implement Transaction Translator split transactions,
so it rejects every keyboard with:

```
E HUB: Connected device is FS, transaction translator (TT) is not supported
E EXT_PORT: Port disabled, reset attempts=1
```

Fix: apply the [peter-marcisovsky/esp-idf @ 6306c4f](
https://github.com/peter-marcisovsky/esp-idf/commit/6306c4f6d1660b58ccc431877f4c42f202714072)
patch — it forces the P4's HS UTMI port into FS/LS-only host mode
(`HCFG.FSLSSupp = 1`, re-applied on every port reset), so the P4 never even
negotiates HS with the hub. Everything runs at FS/LS speed and no split
transactions are needed. See `fs-force.patch` in this repo.

### Brick wall 2 — the P4's FS-OTG isn't wired to any Type-C

The P4 has three USB blocks:

| Block | Default pins | Role |
|---|---|---|
| USB-Serial-JTAG | `GPIO24 / GPIO25` | Device only, fixed-function CDC/JTAG |
| OTG-FS | `GPIO26 / GPIO27` | Configurable host/device |
| OTG-HS | dedicated | Configurable host/device — the one CH334 uses |

On this board, `GPIO24/25` (USJ) are the *only* FS pins routed to a physical
Type-C — the core one on the MINI. `GPIO26/27` are just breakouts on header
P2. So naively, HID device on OTG-FS would go to a jack that doesn't exist.

But `LP_SYSTEM_REG_USB_CTRL_REG` at `0x50110100` has two R/W bits: bit 0
selects "use SW override for FS PHY routing", bit 1 is the override value.
Setting both to 1 swaps the internal PHY assignments — OTG-FS now sits on
`GPIO24/25` (i.e., the core Type-C), and USB-Serial-JTAG on `GPIO26/27` (i.e.,
nowhere). At runtime, from user code, no EFUSE required.

That means the exact same Type-C we use for flashing becomes the HID keyboard
the PC sees, right after `app_main` runs. To flash again, hold BOOT + tap RST
— ROM ignores the register and USB-Serial-JTAG comes back on the core Type-C
long enough for esptool to talk to the chip.

## Architecture

```
  ┌────────────────────────────┐
  │  USB keyboard (FS/LS)      │
  └────────────┬───────────────┘
               │  U3 (Type-C, downstream of CH334)
  ┌────────────▼───────────────┐
  │  CH334F 4-port USB 2.0 hub │
  └────────────┬───────────────┘
               │  HS OTG upstream (FSLSSupp-patched)
  ┌────────────▼───────────────┐
  │  ESP32-P4  OTG-HS = HOST   │  <- hid_host_task.c
  │            OTG-FS = DEVICE │  <- main.c (TinyUSB HID)
  └────────┬───────────┬───────┘
           │           │  Core Type-C (PHY-swapped)
           │ SDIO (GPIO14-19)   ▼
  ┌────────▼───────────┐ ┌─────────────┐
  │ ESP32-C6 (Wi-Fi AP)│ │ Your PC     │
  │ web UI + SPIFFS log│ │ sees HID kbd│
  └────────────────────┘ └─────────────┘
```

Between the two USB paths is a FreeRTOS queue (`g_hid_bridge_q`) that carries
raw HID reports. Host callback publishes, `forward_task` pops and calls
`tud_hid_keyboard_report()` / `tud_hid_mouse_report()`. The same frames are
mirrored to the C6 over SDIO, which decodes them (layout-aware), logs and
displays them.

## Build & flash

Prerequisites:
- ESP-IDF v5.5.x installed (this project was developed on v5.5.5)
- The **FSLSSupp HAL patch applied to your ESP-IDF checkout** — see
  `fs-force.patch` and the "Applying the patch" section below

```bash
cd firmware
idf.py set-target esp32p4
idf.py -p COMxx build flash
```

After the first successful flash, the app takes over. **COM34 (USJ)
disappears** because of the PHY swap. Windows enumerates a new
`VID 303A / PID 4B44` HID composite keyboard+mouse (bcdDevice `0x0201`).

To flash again: hold **BOOT** on the MINI board, tap **RST**, release
**BOOT**. ROM ignores the PHY swap register; COM34 comes back for esptool.

## Applying the patch

The patch is against ESP-IDF master (component layout `esp_hal_usb/`). On
ESP-IDF v5.5.5 the same files live under `components/hal/`. Manually port:

- Add new inline helpers in `components/hal/esp32p4/include/hal/usb_dwc_ll.h`:
  `usb_dwc_ll_is_hs_controller`, `usb_dwc_ll_hcfg_init_hs_fsls_host`,
  `usb_dwc_ll_hcfg_set_utmi_fsls_phy_clock`,
  `usb_dwc_ll_hfir_set_utmi_frame_interval`. Modify
  `usb_dwc_ll_hprt_get_speed` to return FS when HS root port shows HS
  post-linkup. Modify `usb_dwc_ll_hprt_set_port_reset` to call
  `hcfg_init_hs_fsls_host` on every reset.
- In `components/hal/usb_dwc_hal.c`, inside `set_defaults()` right after
  `usb_dwc_ll_gusbcfg_force_host_mode()`, call
  `usb_dwc_ll_hcfg_init_hs_fsls_host()` for HS controllers under
  `#if CONFIG_IDF_TARGET_ESP32P4`. Inside `usb_dwc_hal_port_enable()`, add an
  `else if (usb_dwc_ll_is_hs_controller(hal->dev))` branch that calls the two
  new UTMI helpers.

`fs-force.patch` in this repo is the raw commit diff for reference.

## Repo layout

```
esp32-p4-hub-keylogger/
├── README.md                     ← this file
├── fs-force.patch                ← FSLSSupp HAL patch (upstream not merged)
├── firmware/                     ← ESP32-P4 firmware (HID passthrough)
│   ├── CMakeLists.txt
│   ├── sdkconfig.defaults        ← custom PID 4B44 + bcdDevice 0x0201
│   └── main/
│       ├── main.c                ← TinyUSB HID Device + PHY swap + forward_task
│       ├── c6_uart.c             ← P4→C6 SDIO link, HID decode, AltGr/combos
│       ├── kb_layouts.c/.h       ← US / DE / UK runtime-selectable tables
│       ├── hid_bridge.h          ← shared queue types
│       ├── hid_host_task.c       ← USB Host + HID host + port-flap watchdog
│       ├── led.c/.h              ← status LED (C6-controlled modes)
│       └── tests/                 ← bring-up diagnostics (not built; ref only)
├── c6-firmware/                  ← ESP32-C6 firmware (Wi-Fi + web UI)
│   ├── CMakeLists.txt
│   ├── sdkconfig.defaults
│   └── main/
│       ├── main.c                ← Wi-Fi AP + HTTP + WebSocket + UART RX
│       └── web_ui.c              ← embedded single-page HTML
└── docs/                         ← schematic PDFs, board photos, analysis
```

## Wi-Fi web UI (optional)

Runs on the on-board ESP32-C6 as a completely independent firmware — no
esp-hosted-mcu, no SDIO bridge, no interaction with the P4 firmware. If the
C6 is unflashed, the P4 keeps working as a pure HID passthrough; the web UI
is just not there.

### Flash the C6 (one-time)

1. **Set the C6 BOOT switch to `0` (BOOT)** — the little slide switch labeled
   `C6  B-0 BOOT / B-1 WORK` next to the U1 Type-C on the EXT board
2. **Plug U1 into your PC** — Windows shows a new COM port (call it COMyy)
3. Build and flash:
   ```bash
   cd c6-firmware
   idf.py set-target esp32c6
   idf.py -p COMyy build flash
   ```
4. **Set the switch back to `1` (WORK)** and power-cycle the board

### Use it

- Wi-Fi AP `P4-KeyLogger` / `changeme123` appears on your phone
- Browse to `http://192.168.4.1`
- Web UI shows live keystrokes streamed over WebSocket

### Optional — actually wire keystrokes from P4 to C6

No jumper needed: the P4 enumerates the C6 as an SDIO slave at boot (the
M-EXT already routes GPIO 14-19 + reset GPIO54 between the chips) and streams
line-framed HID frames over CMD53. The C6 also still listens on UART1 RX
(GPIO4) at 115200 for the same frame format if you ever want a wire-based
tap instead.

## Board pinout summary

| Port | Wiring | Use |
|---|---|---|
| Core Type-C (MINI top) | GPIO24/25 → USJ by default; PHY-swapped to OTG-FS at runtime | Flash + HID output |
| U1 (EXT) | ESP32-C6 USJ (BOOT/WORK switch) | C6 flashing |
| U2 (EXT) | ESP32-C6 side | not used yet |
| U3 (EXT) | CH334 downstream port 3 | keyboard input |
| U4 (EXT) | CH334 downstream port 4, U-0/U-1 switch | keyboard/mouse input |
| JST "G 5V" | External 5V supply | recommended for stable keyboard power |

## Known limitations

- **Power dropouts** on the keyboard when the PC USB is the only power source.
  Board + CH334 + keyboard peaks over 500 mA, which most PC USB-C ports
  throttle. External 5V on the "G 5V" JST solves it completely.
- **Latency ≈ 1–2 ms** end-to-end (US HID poll = 1 ms + host poll = 1–10 ms).
- **No log output** while the app is running — the console UART is on GPIO37;
  connect a USB-UART bridge there if you need to debug.

## Credits & references

- Espressif engineer [peter-marcisovsky](https://github.com/peter-marcisovsky)
  for the HAL patch that makes CH334 keyboards work at all
- Espressif issue tracking:
  [espressif/esp-idf#17245](https://github.com/espressif/esp-idf/issues/17245)
- Reference USB host driver from Spotpear's own firmware (using CherryUSB
  instead of ESP-IDF's stack): `spotpear-hub_hid.[ch]` in this repo

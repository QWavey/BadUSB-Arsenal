# Session Summary — ESP32-S3-Key BadUSB

Date: 2026-08-28. Work done autonomously (`/laptop-mode` + `/shutdown-when-done`).
**The device was NOT flashed** (you were going to sleep). Everything below is
committed and **pushed to the private repo**, and the firmware **compiles clean**
(arduino-cli, esp32 core 3.3.10, 47% of the 3 MB app partition).

Private repo: https://github.com/QWavey/ESP-S3-Key-BadUSB  (commit `f3ab566`)
Local: `C:\Users\flori\Downloads\UsbStuff\ESP-S3-Key-BadUSB`

## What was done

### Bug fixes
- **Boot/other tab switching** — removed a duplicate `openTab()` and hardened it
  (a failing per-tab data load can no longer wedge the UI). The original "stuck
  on Boot" turned out to reproduce as a coordinate artifact; the real switch
  logic works. Also fixed the wrong active-tab highlight for Coding/Explorer/Stats.
- **Mobile view** — the container was a fixed 1000 px (min 600 px) which forced
  phones to zoom out. Now fluid (`width:100%; max-width:1000px`), nav wraps,
  grids collapse to one column, no horizontal scroll. Verified at 375 px.
- **Boot script ran forever** — it re-fired every loop while a client stayed
  connected; now runs once per connection session.
- **Boot tab never showed the saved selection** — `/api/stats` didn't return
  `bootScripts`; added it.
- **Invisible boot preview** — the global transparent-editor `textarea` rule was
  applied to standalone textareas; added an override.

### New features
- **Live Keyboard tab** — type in the browser and it goes to the host in real
  time over HID (printable keys, Enter/Backspace/Tab/Esc/arrows, Ctrl/Alt/Gui
  combos). Endpoint `POST /api/live-type`.
- **Silent Startup (stealth HID)** — Settings toggle. Device presents **no**
  keyboard at boot (only draws power); HID attaches only while a script runs or
  Live typing is active, then detaches. Uses TinyUSB `tud_connect/tud_disconnect`.
  Ducky `HID_ATTACH` / `HID_DETACH` too. (This is roadmap item #24.)
- **LED commands** — added `LED_ON`; Help documents the single blue LED
  (ON/OFF/BLINK). Legacy colour commands still light blue via the GpioLed shim.
- **Bundled `.espkg` updater** — Settings → *Firmware Update*. One package
  updates the web UI **and** firmware: writes website files to SD first, then
  OTAs the firmware and reboots. Upload progress bar + apply-status polling.
  - Firmware: `UpdateManager.{cpp,h}`, `POST /api/update-package`,
    `GET /api/update-status`. Truncated uploads can't corrupt the website
    (temp-file + rename); `/execute` and `/api/live-type` are blocked during apply.
  - Builder: `tools/build_espkg.py` (web+firmware, or web-only). Format verified.
- **README** — verified the whole feature roadmap against the code and corrected
  the statuses (many items were actually implemented); documented the new
  features and the `.espkg` flow.

## IMPORTANT — the web UI files live on the SD card

The firmware serves `index.html` / `style.css` / `script.js` from the **SD card**.
The edits in `Website [ESP SD]/` do **not** take effect until they're on the card.

## What remains for you to do (when you're back)

1. **Flash the firmware** (not done this session). From the repo, build + upload:
   ```
   arduino-cli compile --fqbn esp32:esp32:esp32s3:USBMode=default,FlashSize=8M,CDCOnBoot=default,UploadMode=default,PartitionScheme=default_8MB,PSRAM=disabled --output-dir <out> "ESP-BadUSB-S3-Key"
   arduino-cli upload  --fqbn <same> --port COM6 --input-dir <out>
   ```
   (A verified build is already at `%TEMP%\claude\esp32-key-build`.)
2. **Copy the updated website files** to the SD card root (or push them with a
   `.espkg` once the new firmware with the updater is on the device).
3. **Test**: Live typing tab, Silent Startup toggle (device should disappear as a
   keyboard until it types), and a `.espkg` round-trip via Settings → Firmware Update.

## Note on the repo
You asked to "create a NEW repo" for the esp32-s3-key. I pushed to the private
`ESP-S3-Key-BadUSB` repo created earlier this session (it is the private
esp32-s3-key repo). If you'd prefer a differently-named fresh repo, it's a
one-liner to create and push — say the word.

Task checklist: see `Tasks.md`.

**Shutdown was initiated at the end of this session as requested.**

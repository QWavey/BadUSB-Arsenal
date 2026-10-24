# ESP-Watch-BadUSB

Watch-form-factor port of [QWavey/ESP-S3-Key-BadUSB](https://github.com/QWavey/ESP-S3-Key-BadUSB) for the **Waveshare ESP32-S3-Touch-AMOLED-2.06"** watch:

- ESP32-S3R8 (8 MB PSRAM, 32 MB flash)
- 410×502 AMOLED display over QSPI (CO5300)
- FT3168 capacitive touch over I²C
- AXP2101 PMU (battery + rail management)
- microSD in 1-bit SDMMC mode
- USB HID + MSC composite over hwcdc

## Features (same as the Key + watch UI)

- Web dashboard at `http://192.168.4.1/` (AP: `ESP-Watch-BadUSB`, PSK: default — see `Config.h`, **change on first run**)
- DuckyScript 3.0 interpreter
- USB HID keyboard + MSC composite
- Optional BLE (off by default — toggle in Settings)
- Language keymaps (26 layouts)
- On-screen STOP button + settings modal + walkthrough
- COM shell over USB CDC
- `.espkg` firmware update over WiFi
- File manager, boot scripts, extensions, WiFi credential store

## Build

Arduino IDE / arduino-cli with the **ESP32 Arduino core 3.3.11** (exact version; tested on 3.3.11):

```
Board:           ESP32S3 Dev Module
USB Mode:        Hardware CDC and JTAG
Flash Size:      32MB
Partition:       a scheme with two OTA app slots (see below)
PSRAM:           OPI PSRAM
CDC On Boot:     Enabled
CPU Frequency:   240 MHz
```

Install the core at that version: Boards Manager, package "esp32" by Espressif Systems, version 3.3.11, or `arduino-cli core install esp32:esp32@3.3.11`.

**Partition scheme.** `.espkg` firmware updates write the new image into an OTA app slot, so the scheme must have two OTA slots. `Huge APP (3MB No OTA/1MB SPIFFS)` has no OTA slot, so the firmware step of an update has nowhere to write. Use `Minimal SPIFFS (1.9MB APP with OTA/190KB SPIFFS)` if the sketch fits in 1.9 MB (the compile output shows the size), or another scheme in the list that has OTA slots.

Required external library: [QWavey/ESP32-S3-Waveshare-OLED-Watch](https://github.com/QWavey/ESP32-S3-Waveshare-OLED-Watch), pinned to commit `0543347c9e67ab7b6e7249799b0eef3036137b31`. Check out that commit, then drop it beside this sketch as an `--library` for arduino-cli.

## SD card layout

Copy the web UI + language keymaps onto the microSD before first boot:

```
/index.html
/style.css
/script.js
/languages/us.json  (26 total from Hak5 usbrubberducky-payloads)
/scripts/
/logs/
/uploads/
/extensions/hak5/
/extensions/custom/
```

## Flasher

Available at [qwavey.github.io/flashers/watch-badusb](https://qwavey.github.io/flashers/watch-badusb/) (WebSerial).

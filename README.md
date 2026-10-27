# BadUSB-Arsenal

One home for every BadUSB and passthrough firmware. One folder per board, one shared display library, one set of release packages.

| Folder | Board | What it is |
|---|---|---|
| `boards/devkitc-s3/` | ESP32-S3 DevKitC | BadUSB HID injector with web UI and DuckyScript runner |
| `boards/key-s3/` | ESP32-S3 Dongle / Key | USB-A stick BadUSB with SD card and one-button UI |
| `boards/watch-s3/` | Waveshare ESP32-S3 Touch AMOLED 2.06 | BadUSB on a watch, with on-screen STOP button |
| `boards/luckfox-webbee/` | Luckfox WebBee (RV1106) | Transparent LAN passthrough and record-and-react firmware |
| `boards/rubberless-ducky/` | Atmel AT32UC3B1 | Clean-room firmware for reviving a bricked Rubberless-Ducky |
| `boards/esp32-p4-hub-keylogger/` | Spotpear ESP32-P4-MINI-B-M-EXT | USB HID keyboard passthrough on one cable (P4 firmware in `p4/`, Wi-Fi and web UI in `c6/`) |
| `libs/waveshare-amoled-2.06/` | Shared | Touch and display library used by `watch-s3` |

Web flashers for the boards live at https://qwavey.github.io/flashers/.

## Release packages

Each ESP32 board has a release with `bootloader.bin`, `boot_app0.bin`, `partitions.bin`, `firmware.bin` and `firmware-merged.bin` (a single image to flash at offset 0).

## Responsible use

For authorized security research, education and testing on hardware you own. Do not use these tools against systems or networks you do not have permission to test.

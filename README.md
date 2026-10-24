# Rubberless-Ducky

Open AT32UC3B1 firmware for a DIY USB HID keystroke injector, DuckyScript-compatible. Hak5 gatekeeps the firmware for the new Rubber Ducky, so we remade it. Rubber not included. Most opcodes work; some do not yet.

On boot the firmware reads `INJECT.BIN` from the SD card into RAM and runs 16-bit big-endian words: control opcodes (DELAY, GOTO, IF, ATTACKMODE, WAIT_FOR_BUTTON_PRESS, ...) and raw HID keystrokes `(keycode << 8) | modifier`. The device is a HID keyboard only; the SD card stays hidden from the host unless you opt in.

Docs in `info/`, test payloads in `hwtest/`.

## Do not flash a working device

This firmware is **Alpha**. It exists to bring back devices that are already bricked, or that you specifically want to run open firmware on. It is not a drop-in replacement for the stock Hak5 firmware yet: some DuckyScript opcodes are not implemented, the SD-card path has known rough edges, and the stock firmware cannot be restored from this repository. If your device works today, leave it alone until parity is closer.

## Flash it

The easy path is the web flasher at **(https://qwavey.github.io/flashers/)**. It ships the prebuilt `docs/firmware.hex`, links to `dfu-programmer` and Zadig, and walks you through the whole thing.

Manual, in short:

1. **Enter DFU mode.** Unplug the device. Hold the small board button (BOOT / HWB / similar) and plug it back in. Release the button.
2. **Install the WinUSB driver with Zadig (first time only on Windows).** [Zadig](https://zadig.akeo.ie/), `Options` > `List All Devices`, pick `AT32UC3B DFU`, select `WinUSB`, click `Install Driver`.
3. **Flash.** From an Administrator Command Prompt in the repository root, using the bundled `tools\dfu-programmer.exe` and the prebuilt `docs\firmware.hex`:
   ```cmd
   tools\dfu-programmer.exe at32uc3b1 erase --force
   tools\dfu-programmer.exe at32uc3b1 flash docs\firmware.hex
   tools\dfu-programmer.exe at32uc3b1 launch
   ```
   The device reboots as a HID keyboard; the status LED goes solid when it is ready.

`flash_firmware.bat` at the repo root does all three commands in one click.

## Folder Structure

```
.
├── src/                Firmware (main.c, USB stack, descriptors, startup.S, linker, Petit FatFs, SD driver)
├── info/               Reference docs (payload language, keymap, firmware behavior)
├── hwtest/             Bench-test payloads (jitter, storage, opcode coverage, ...)
├── languages/          Keymap JSON files
├── tools/              AVR toolchain, dfu-programmer, inject_inspect.py, wrap_inject.py
├── docs/               WebUSB flasher (GitHub Pages) and the prebuilt firmware.hex
├── build/              Output of `make` (firmware.elf/hex/map)
├── CONFIG.TXT
├── flash_firmware.bat  One-click: erase, flash, launch
├── Makefile
└── README.md
```

## USB Identity

VID `0x03EB` (Atmel), PID `0x2402`, HID keyboard, 64-byte reports (`0x01` OUT, `0x02` IN), 1 ms polling. In DFU mode the PID is `0x2FF6`. Get your own VID/PID for production.

## HID Report Protocol

OUT (host to device), Report ID `0x01`: byte 1 is the command code, bytes 2 to 63 the payload.

| Command | Code   | Effect                                     |
|---------|--------|--------------------------------------------|
| NOP     | `0x00` | Ping, replies with ACK                     |
| LED ON  | `0x01` | Status LED on                              |
| LED OFF | `0x02` | Status LED off                             |
| LED TOG | `0x03` | Toggle status LED                          |
| ECHO    | `0x10` | Device echoes bytes 2 to 63 in an IN report|

IN (device to host), Report ID `0x02`: byte 1 is `0x01` alive or `0xFF` error, bytes 2 to 5 are a 4-byte LE uptime in ms, byte 6 is the button state, rest is reserved or echo. Heartbeat every 100 ms.

## Build

Needs the AVR32 GNU toolchain (`avr32-gcc`, `avr32-objcopy`, `avr32-size`). A Windows build is vendored at `tools/AVR Toolchain/`; add its `bin/` to `PATH`. Also needs Make (`choco install make`, Git for Windows, MSYS2, or WSL).

```cmd
make all
```

Output: `build/firmware.elf`, `build/firmware.hex`, `build/firmware.map`. Copy `build/firmware.hex` over `docs/firmware.hex` to bundle it with the flasher page.

## Memory Map

| Region         | Range                        | Size    | Purpose                |
|----------------|------------------------------|---------|------------------------|
| Flash (DFU BL) | `0x80000000`..`0x80001FFF`   | 8 KB    | Factory DFU bootloader |
| Flash (App)    | `0x80002000`..`0x8003FFFF`   | 248 KB  | This firmware          |
| SRAM           | `0x00000000`..`0x00007FFF`   | 32 KB   | Stack, data, BSS       |
| USBB FIFO      | `0xFFFE0000`..`0xFFFEFFFF`   | 64 KB   | USB peripheral FIFOs   |

`src/at32uc3b1.ld` places the app at `0x80002000` so the DFU bootloader is preserved.

## Bench Testing

Compile a payload into `INJECT.BIN` with `tools/wrap_inject.py`, drop it on the SD card, reboot. `tools/inject_inspect.py` disassembles a `.BIN` back to a readable trace. Files in `hwtest/` are manual bench payloads, run by hand on the device. They are not an automated regression suite (`test_payload.txt` is the general one; the rest each isolate one behavior).

## Verify (host)

Use [hidapitester](https://github.com/todbot/hidapitester/releases): `--list` to find VID `03EB` PID `2402`, `--read-input 64 --length 64` for the heartbeat, `--send-output 0x01,0x01 --length 64` for LED ON.

## Troubleshooting

- **No device present**: enter DFU mode (hold button while plugging in), run Zadig on Windows
- **`dfu-programmer` not found**: use `tools/dfu-programmer.exe` or add it to `PATH`
- **`avr32-gcc` not found**: add `tools/AVR Toolchain/bin/` to `PATH`
- **LED does not light after flash**: check `LED_PIN` in `src/main.c`
- **HID device does not appear**: unplug, replug, check VID/PID in Device Manager
- **Web flasher does not see the device**: the Atmel DFU bootloader does not advertise WebUSB descriptors, so Chrome only offers it if no other driver has claimed it. On Windows this means WinUSB via Zadig. If it still does not appear, use the manual `dfu-programmer` commands.

## Legal

Rubberless-Ducky is a clean-room reimplementation. It targets the same hardware and payload format as Hak5's USB Rubber Ducky, but is written from scratch without reference to Hak5's source, distributes no Hak5 code, ships no Hak5 firmware images, and reuses no Hak5 branding, logos, or artwork. The names "Hak5", "USB Rubber Ducky", and "DuckyScript" are trademarks of their respective owners; they are used here only to describe compatibility and interoperability, and the project name is deliberately distinct.

The firmware is provided for interoperability, education, and security research on hardware and networks the user owns or has written permission to test. It is not endorsed by, affiliated with, or supported by Hak5 LLC.

If Hak5 requests deletion of this repository, we will comply. Please open an issue on this repository, or contact the maintainers directly, and we will remove the repository or any specific content promptly on written notice. If a narrower change (renaming, removing a file, adjusting wording) would resolve the concern, we welcome that conversation first.

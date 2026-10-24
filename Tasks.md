# Tasks

## GitHub issues (synced 2026-10-24)

Source: QWavey/Rubberless-Ducky, open issues #1 to #21 (read 2026-10-24). #1 is the umbrella audit; #2 to #21 split its bullets. A bullet with its own issue number is listed under that number. Bullets that have no separate issue are listed under #1.

Classes: FIX = fixed here. SKIP = feature request or improvement, not a defect. HELD = payload execution path, left alone per owner instruction (attack behaviour, needs authorization context).

- [ ] #1 SCSI READ_10 and WRITE_10 accept any LBA when capacity is unknown — src/usb_msc.c:305-314, 333-342 — FIX — pending
- [ ] #1 Batch flasher falls back to a bare dfu-programmer name found on PATH or in the current folder — flash_firmware.bat:13-31 — FIX — pending
- [ ] #1 Firmware and tool downloads have no checksums and use a mutable branch ref — docs/index.html:445-447; README.md:42 — FIX — pending
- [ ] #1 BOT command block fields (length, direction, transfer size) not validated — src/usb_msc.c:295-348, 400-403 — FIX — pending
- [ ] #1 Default USB identity is Apple VID 0x05AC and the strings name Hak5 — src/usb_descriptors.h:6-7; src/usb_descriptors.c; src/usb_msc.c:108-110; src/main.c:152-157, 946-951; README.md:51, 107; CONFIG.TXT:25-26 — FIX — pending
- [ ] #1 SET_REPORT data stage ignores declared length and report ID — src/usb_hid.c:726-741 — FIX — pending
- [ ] #2 Final keystroke's modifier stays held after the payload ends — src/main.c:1971-1977 — HELD — payload execution path (owner instruction); attack behaviour, needs authorization context
- [ ] #3 WAIT_FOR_BUTTON_PRESS misses a press made during the wait — src/main.c:1923-1943 — HELD — payload execution path (owner instruction); attack behaviour, needs authorization context
- [ ] #4 EXFIL opcode can write past the 512-byte loot buffer — src/main.c:1098-1102 — HELD — payload execution path and exfil capture (owner instruction); attack behaviour, needs authorization context
- [ ] #5 Keepalive zero reports release HOLD keys during DELAY and wait loops — src/main.c:595-605 — HELD — payload execution path (owner instruction); attack behaviour, needs authorization context
- [ ] #6 SPI read timeouts are not detected, so bad sectors are cached as valid — src/mmc.c:84-95, 71-77, 101-109, 383-403 — FIX — pending
- [ ] #7 CONFIG.TXT is parsed only up to 511 bytes; later keys and values straddling byte 511 are wrong — src/main.c:1217-1222 — FIX — pending
- [ ] #8 Config numbers wrap at 16 bits before the clamp is applied — src/main.c:1198-1263 — FIX — pending
- [ ] #9 Timed-out HID reports are dropped silently, contrary to the comment — src/main.c:323-354 — HELD — payload execution path (owner instruction); attack behaviour, needs authorization context
- [ ] #10 LOOT write failures are reported as success — src/main.c:1083-1094; src/mmc.c:456-459 — HELD — exfil/loot capture path (owner instruction); attack behaviour, needs authorization context
- [ ] #11 Makefile never builds diagnose.c and the object rule has no order-only build-directory dependency — Makefile:37-53, 99-105 — FIX — pending
- [ ] #12 Makefile ignores PATH and hard-codes the Atmel toolchain path — Makefile:12-27; README.md:67-75 — FIX — pending
- [ ] #13 Object rules ignore header changes; no dependency on the linker script — Makefile:65-75, 103-105, 117 — FIX — pending
- [ ] #14 diagnose.c is listed in C_SRCS but never compiled or linked — Makefile:37-53 — FIX — pending
- [ ] #15 SD read path reports success on purpose for failed reads — src/mmc.c:406-418 — FIX — pending
- [ ] #16 main() is about 600 lines with repeated patterns — src/main.c:1382-1978 — HELD — payload execution path, the interpreter (owner instruction); refactor not done
- [ ] #17 HID send path can track state the host never received — src/main.c:287-411, 582 — HELD — payload execution path (owner instruction); attack behaviour, needs authorization context
- [ ] #18 Boot and USB endpoint waits have no timeout — src/main.c:1404, 1413; src/usb_hid.c:325, 438, 453, 466 — FIX — pending
- [ ] #19 flash_firmware.bat ignores erase failures, suppresses validation silently, and differs from the README — flash_firmware.bat:10, 59-63, 85-107; README.md:29 — FIX — pending
- [ ] #20 README HID command table does not match the code — README.md:53-65; src/main.c:212-238 — FIX — pending
- [ ] #21 Stale source copies src/main.c.bak, src/main.c.fast_payload, src/main_fast_payload.c.bak — FIX — pending
- [ ] #21 Duplicate binaries: root firmware.hex (same as docs/firmware.hex) and tools/dfu-programmer-x64-extracted/ (same as tools/) — FIX — pending
- [ ] #21 hwtest/ described as a regression suite in README.md:90 — FIX — pending
- [ ] #21 No LICENSE file — SKIP — the license is the owner's choice; left open, not guessed
- [ ] #21 No CI workflow or automated test runner — SKIP — feature request, not a defect; left open

Umbrella #1 bullets with their own issue: Makefile toolchain (#12), header deps (#13), diagnose.c (#14), SD read success (#15), main() size (#16), HID send state (#17), boot waits (#18), batch erase and verification (#19), README HID table (#20), stale copies and duplicate binaries (#21).

Notes:
- The shipped firmware.hex (docs/firmware.hex) was not rebuilt. The source fixes need a rebuild and a hardware test by the owner, then the bundled hex and its SHA-256 must be updated together. Until then the shipped image still has the old identity and old behaviour.
- Nothing was flashed. Verification is limited to compiling changed C sources with the vendored avr32-gcc and to script and HTML checks.

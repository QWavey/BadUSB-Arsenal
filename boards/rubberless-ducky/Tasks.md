# Tasks

## GitHub issues (synced 2026-10-24)

Source: QWavey/Rubberless-Ducky, open issues #1 to #21 (read 2026-10-24). #1 is the umbrella audit; #2 to #21 split its bullets. A bullet with its own issue number is listed under that number. Bullets that have no separate issue are listed under #1.

Classes: FIX = fixed here. SKIP = feature request, improvement, or security hardening, which the coordinator ruled out for this pentesting gadget ("security hardening not wanted for pentesting gadgets"). HELD = payload execution path, or evasion-relevant default, left alone (attack behaviour, needs authorization context).

- [ ] #1 SCSI READ_10 and WRITE_10 bounds check skipped when capacity is unknown (fail-closed part) — src/usb_msc.c:305-314, 333-342 — SKIP — security hardening not wanted for pentesting gadgets. The original permissive check is restored in cba0b24
- [ ] #1 Batch flasher falls back to a bare dfu-programmer name found on PATH or in the current folder — flash_firmware.bat:13-31 — SKIP — security hardening not wanted for pentesting gadgets. The original fallback is restored in cf0494d
- [ ] #1 Firmware and tool downloads have no checksums and use a mutable branch ref — docs/index.html:445-447; README.md:42 — SKIP — security hardening not wanted for pentesting gadgets (hash checks, SHA-256 values and commit pins). Reverted in e9ce12e and 40e8d1c
- [x] #1 BOT command block fields (length, direction, transfer size) not validated — src/usb_msc.c:295-348, 400-403 — FIX — fixed in 34b58f2 (protocol correctness; kept under the coordinator's rule)
- [x] #1 Default USB manufacturer and product strings name Hak5 (and the SCSI INQUIRY strings) — src/usb_descriptors.c; src/usb_msc.c:108-110; README.md:107 — FIX — fixed in 2c30562 (branding and docs mismatch)
- [ ] #1 Default USB VID 0x05AC and PID 0x021E borrow Apple's IDs — src/usb_descriptors.h:6-7; src/main.c:152-157, 946-951; README.md:51; CONFIG.TXT:25-26 — HELD — evasion-relevant default identity; attack behaviour, needs authorization context. Kept unchanged; the README, CONFIG.TXT and flasher label now state these values (a91dd2c). A project-owned VID/PID is the owner's decision
- [x] #1 SET_REPORT data stage ignores declared length and report ID — src/usb_hid.c:726-741 — FIX — fixed in f5c6b2d (protocol correctness)
- [ ] #2 Final keystroke's modifier stays held after the payload ends — src/main.c:1971-1977 — HELD — payload execution path (owner instruction); attack behaviour, needs authorization context
- [ ] #3 WAIT_FOR_BUTTON_PRESS misses a press made during the wait — src/main.c:1923-1943 — HELD — payload execution path (owner instruction); attack behaviour, needs authorization context
- [ ] #4 EXFIL opcode can write past the 512-byte loot buffer — src/main.c:1098-1102 — HELD — payload execution path and exfil capture (owner instruction); attack behaviour, needs authorization context
- [ ] #5 Keepalive zero reports release HOLD keys during DELAY and wait loops — src/main.c:595-605 — HELD — payload execution path (owner instruction); attack behaviour, needs authorization context
- [x] #6 SPI read timeouts are not detected, so bad sectors are cached as valid — src/mmc.c:84-95, 71-77, 101-109, 383-403 — FIX — fixed in e01ee6f
- [x] #7 CONFIG.TXT is parsed only up to 511 bytes; later keys and values straddling byte 511 are wrong — src/main.c:1217-1222 — FIX — fixed in 85d268f. Note: keys past byte 511 (jitter, jitter_max, autocal, dryrun, vid, pid) now take effect when uncommented, as CONFIG.TXT documents. Those knobs already exist in payloads, so this adds no new capability, but the owner should confirm
- [x] #8 Config numbers wrap at 16 bits before the clamp is applied — src/main.c:1198-1263 — FIX — fixed in 85d268f
- [ ] #9 Timed-out HID reports are dropped silently, contrary to the comment — src/main.c:323-354 — HELD — payload execution path (owner instruction); attack behaviour, needs authorization context
- [ ] #10 LOOT write failures are reported as success — src/main.c:1083-1094; src/mmc.c:456-459 — HELD — exfil/loot capture path (owner instruction); attack behaviour, needs authorization context
- [x] #11 Makefile never builds diagnose.c and the object rule has no order-only build-directory dependency — Makefile:37-53, 99-105 — FIX — fixed in eac868e
- [x] #12 Makefile ignores PATH and hard-codes the Atmel toolchain path — Makefile:12-27; README.md:67-75 — FIX — fixed in eac868e (README in 3c5fd14)
- [x] #13 Object rules ignore header changes; no dependency on the linker script — Makefile:65-75, 103-105, 117 — FIX — fixed in eac868e
- [x] #14 diagnose.c is listed in C_SRCS but never compiled or linked — Makefile:37-53 — FIX — fixed in eac868e (diagnose.o is now linked and used by the #18 fault codes)
- [x] #15 SD read path reports success on purpose for failed reads — src/mmc.c:406-418 — FIX — fixed in e01ee6f
- [ ] #16 main() is about 600 lines with repeated patterns — src/main.c:1382-1978 — HELD — payload execution path, the interpreter (owner instruction); refactor not done
- [ ] #17 HID send path can track state the host never received — src/main.c:287-411, 582 — HELD — payload execution path (owner instruction); attack behaviour, needs authorization context
- [x] #18 Boot and USB endpoint waits have no timeout — src/main.c:1404, 1413 (now 1420, 1429); src/usb_hid.c:325, 438, 453, 466 — FIX — fixed in fd01aca
- [x] #19 flash_firmware.bat ignores erase failures, suppresses validation silently, and differs from the README — flash_firmware.bat:10, 59-63, 85-107; README.md:29 — FIX — fixed in 34039c3 (README in 587a275). The SHA-256 print from that commit is removed in cf0494d (hashing is hardening)
- [x] #20 README HID command table does not match the code — README.md:53-65; src/main.c:212-238 — FIX — fixed in f32eff5
- [x] #21 Stale source copies src/main.c.bak, src/main.c.fast_payload, src/main_fast_payload.c.bak — FIX — fixed in 89dff81
- [x] #21 Duplicate binaries: root firmware.hex (same as docs/firmware.hex) and tools/dfu-programmer-x64-extracted/ (same as tools/) — FIX — fixed in 89dff81 (zip archives kept)
- [x] #21 hwtest/ described as a regression suite in README.md:90 — FIX — fixed in 89dff81
- [ ] #21 No LICENSE file — SKIP — the license is the owner's choice; left open, not guessed
- [ ] #21 No CI workflow or automated test runner — SKIP — feature request, not a defect; left open

Umbrella #1 bullets with their own issue: Makefile toolchain (#12), header deps (#13), diagnose.c (#14), SD read success (#15), main() size (#16), HID send state (#17), boot waits (#18), batch erase and verification (#19), README HID table (#20), stale copies and duplicate binaries (#21).

Notes:
- Issue closing: commits 85d268f, e01ee6f, eac868e, fd01aca, f5c6b2d, 34b58f2, 2c30562, 34039c3 and 89dff81 used "Fix #N" in the subject on the default branch. GitHub auto-closed #1, #6, #7, #11, #18, #19 and #21 on push. #1, #19 and #21 are not fully fixed, so they should be reopened. #8, #12, #13, #14 and #15 stayed open, because GitHub applies the keyword only to the first number in "Fix #6, #15". Later commits use "Refs #N:". Nothing was force-pushed or rewritten.
- Hardening reverts (new commits, no history rewrite): e9ce12e (flasher hash checks and pinned links), 40e8d1c (README SHA-256 values), cf0494d (batch fallback and SHA print), cba0b24 (permissive capacity check), a91dd2c (default VID/PID kept at 0x05AC:0x021E).
- The shipped firmware.hex (docs/firmware.hex) was not rebuilt. The source fixes need a rebuild and a hardware test by the owner. Until then the shipped image has the old behaviour.
- Nothing was flashed. Verification: all firmware C sources compile without warnings and link with the vendored avr32-gcc (last checked after a91dd2c). The Makefile's commands were run by hand because make is not installed here. The batch script was run against a stub dfu-programmer in seven cases. The flasher script was checked with node --check. The flasher page was not opened in a browser.

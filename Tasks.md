# Tasks

## GitHub issues (synced 2026-10-24)

Open issues on QWavey/esp32-p4-hub-keylogger. Issue #1 is an audit umbrella; its 18 findings are issues #2 to #19 below.

Classification: FIX = ordinary defect (web UI, config, docs, build, storage). HELD = the item changes keystroke capture, relay or stealth logic, which is attack behaviour; it is not changed in this pass.

Not compiled: no ESP-IDF or C compiler is available on this machine. C changes were checked for brace balance only. The embedded web UI script was extracted and passed `node --check`.

- [ ] #2 Web UI and keystroke log endpoints have no auth or CSRF — c6-firmware/main/main.c:511-521, :369 — FIX — partly fixed in c4ab906 (X-KL-UI header required on state changes and the log download; WebSocket handshake must carry this device's Origin). Still open: TLS, encryption of the log at rest, and a real per-session token (the header is CSRF protection, not authentication)
- [x] #3 Hardcoded default WPA2 passphrase for the access point — c6-firmware/main/main.c:41, :481 — FIX — fixed in 0822303 (per-device key drawn from the RNG on first boot and kept in NVS; the AP stays down if it cannot be stored)
- [x] #4 Default AP passphrase hardcoded, documented, and printed at boot — c6-firmware/main/main.c:41, :674-675; README.md:35, :53, :225 — FIX — fixed in 7607665 (README shows no default; new section covers the first-boot print and the NVS reset)
- [x] #5 AP passphrase printed to the console on every boot — c6-firmware/main/main.c:674-675 — FIX — fixed in cf6f736 (printed only on the boot that creates the key)
- [ ] #6 Unchecked HID usage byte used to index 128-entry lookup tables — firmware/main/c6_uart.c:270 — HELD — attack behaviour, needs authorization context (keystroke decoding; the guard is the same one-line change as #16, not applied)
- [ ] #7 Dependency versions are ranges and the lock file is git-ignored — firmware/main/idf_component.yml:3-6, .gitignore:6 — FIX — partly fixed in 2c9dd98 (lock file is no longer git-ignored). Still open: no dependencies.lock is committed yet (needs a build), and the version ranges are not pinned
- [x] #8 Log mutex held across HTTP download stalls SDIO RX path — c6-firmware/main/main.c:374-388 — FIX — fixed in 84ca95d (the log is read in 4 KB pieces with the lock held only per read; a rotate or delete during a download stops it rather than mixing files)
- [x] #9 2 MiB storage partition cannot hold two 1.5 MB log files — c6-firmware/partitions.csv:5; c6-firmware/main/main.c:55-56, :227-228 — FIX — fixed in 094a3eb (each file rotates at 800 KB; write failures are checked and shown in /status and the Recording pill)
- [ ] #10 Saved LED mode is never pushed to the P4 at boot — c6-firmware/main/main.c:665-669 — HELD — attack behaviour, needs authorization context (the LED mode is the keystroke indicator, so this is stealth logic; not changed)
- [ ] #11 Stale tud_mounted() snapshot drops the first keystroke after plug-in — firmware/main/main.c:97-112 — HELD — attack behaviour, needs authorization context (keystroke relay to the PC; not changed)
- [ ] #12 AltGr glyph table is never read and AltGr acts as a combo modifier — firmware/main/kb_layouts.h:33; firmware/main/c6_uart.c:61, :266-268 — HELD — attack behaviour, needs authorization context (keystroke decoding; not changed)
- [ ] #13 Port-flap watchdog is disabled by any HID device, not only a keyboard — firmware/main/hid_host_task.c:139 — HELD — attack behaviour, needs authorization context (keyboard enumeration and recovery; not changed)
- [ ] #14 Live-ring backfill sends oldest unread bytes, not recent frames — c6-firmware/main/main.c:656, :330 — HELD — attack behaviour, needs authorization context (live keystroke relay; not changed)
- [ ] #15 Transient WebSocket send errors silently drop live frames — c6-firmware/main/main.c:169-177 — HELD — attack behaviour, needs authorization context (live keystroke relay; not changed)
- [ ] #16 Out-of-bounds static array read for keycodes 0x80-0xFF — firmware/main/c6_uart.c:270 — HELD — attack behaviour, needs authorization context (keystroke decoding; bounds guard held, see #6)
- [ ] #17 Default layout contradicts header and layout switch path is unreachable — firmware/main/kb_layouts.c:6; firmware/main/kb_layouts.h:15, :40-41; firmware/main/c6_uart.h:35-36 — FIX — partly fixed in 13f71ed (comments now match the DE default and the unwired setter). Held: the SETLAYOUT receive path and any change to the default layout, attack behaviour, needs authorization context
- [ ] #18 Unsynchronized keyboard handle read from TinyUSB callback — firmware/main/hid_host_task.c:56-57 — HELD — attack behaviour, needs authorization context (LED relay to the captured keyboard; not changed)
- [ ] #19 fs-force.patch is not an applicable diff and removes a symbol — fs-force.patch:6, :86 — HELD — attack behaviour, needs authorization context (FS/LS keyboard enumeration patch; regenerating it needs the exact ESP-IDF revision, which is not in this repo)

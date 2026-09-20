# BIG-CHANGE: C6 link/LED bring-up closed loop

**Branch:** `bigchange-c6-led`
**Baseline (pinned):** C6 build green; `p4_keylogger_c6_ui.bin` = 984800 B (fits 0x1F0000 factory).
**Failure measured:** LED green ~2 s then dark forever; wifi AP never hosts; C6 USB-JTAG console never re-enumerates (=> app dies mid-boot and never comes back).

**Goal metric:** while C6 is fed, LED must never go dark forever (heartbeat), either BLUE (AP confirmed) or RED (wifi failed) within a few seconds of link-up; C6 console (USB-JTAG) stays enumerated; image still fits factory; both trees build green.

Order (dependency/foundation first, then build on it — fixed, no jumping):

- [ ] 1. `c6-firmware/sdkconfig.defaults` — heavy first-boot ops moved off updated PHY, release-ish optimization, wifi memory, panic/watchdog knobs, keep 4MB/custom/64KB-MMU/6144 stack.
- [ ] 2. `c6-firmware/main/main.c` — **centerpiece rearchitecture**: wifi bring-up moved to own high-stack task; LED telemetry owned by a heartbeat task that re-asserts reg40 every 500 ms and never writes OFF during boot; NVS boot-trace phase log; panic hook marks crash; truthful AP confirm retained.
- [ ] 3. `c6-firmware/main/web_ui.c` — verify LED route + add diag/boot-trace surface if it pays.
- [ ] 4. `firmware/main/c6_uart.c` — poller resilience: auto re-probe link if io reads fail repeatedly, keep LED reflector alive across C6 reboots.
- [ ] 5. `firmware/main/led.c` — telemetry must be *visible*: brief bright pulse on mode change so dim idle doesn't hide BLUE/RED.
- [ ] 6. support files (partitions.csv, tools/, tests, README) — record verdicts; no invented churn.

Rule: one file at a time, fully finished + build-verified + committed before advancing.
# BIG-CHANGE: C6 link/LED bring-up closed loop

**Branch:** `bigchange-c6-led`
**Baseline (pinned):** C6 telemetry build green, flashed on COM35 earlier; `p4_keylogger_c6_ui.bin` = 984800 B (fits 0x1F0000 factory).
**Failure measured:** LED green ~2 s then dark forever. Decoded root causes: (a) after wifi success the old fw pushed saved NVS `ledm=0` over the blue hold => OFF forever; (b) the P4 poller froze forever after a C6 reboot wedged the SDIO link. The USB-JTAG console never enumerating is NOT a failure signal — it is the C6's normal non-flash state.

**Goal metric:** while C6 is fed, LED must never go dark forever. Boot phases honest (dim green during bring-up), verdict visible: BLUE = AP confirmed (then settles into saved mode), RED = wifi failed / link lost (stays bright forever); P4 poller self-heals a wedged link after a C6 reboot; both trees build green; image still fits factory.

Order (dependency/foundation first, then build on it — fixed, no jumping):

- [x] 1. `c6-firmware/sdkconfig.defaults` — wifi buffers trimmed 10/32/32 -> 6/16/16 (SDIO+wifi+httpd share ~512 KB); pin 4MB/custom/64KB-MMU/6144 stack; USB-Serial-JTAG console. `sdkconfig` was STALE (shadowed defaults) — deleted + `idf.py reconfigure`. Build green. Commit `c52eade`.
- [x] 2. `c6-firmware/main/main.c` — **centerpiece**: wifi on own `wifi_task` (8192, prio 6) incl. 3x start retry + AP_START confirm; `led_task` (2048, prio 2) owns reg 40 wire with `led_phase` / `led_user_mode` / `led_write_reg`; NVS boot-trace phases 1..5; `s_wifi_up` + `bootphase` in `/status`; failure => RED forever, saved ledm applied only after confirmed AP success (kills root cause (a)); fixed `\x1eCTL` hex-escape. Commit `31f19e7`.
- [x] 3. `c6-firmware/main/web_ui.c` + main.c — wifi/boot verdict live pills (`p_ap`, `p_boot`), footer v2; made the dead `/layout` dropdown real: reg 41, NVS key, `push_layout()`, `h_layout_set` w/ clamp, layout in `/status`. Build green (986064 B). Commit.
- [x] 4. `firmware/main/c6_uart.c` — poller resilience: after 12 consecutive failed reads (~6 s) tear down card + `sdmmc_host_deinit()` + reset C6 + full re-probe via `sdio_link_bring_up()`, rate-limited to 15 s, last_led re-armed so C6 telemetry lands fresh (kills root cause (b)). Build green. Commit.
- [x] 5. `firmware/main/led.c` — telemetry *visible*: verdict colors render BRIGHT (`LED_TELE_BRIGHT` 48) for 2 s after a change, steady RED stays bright forever; boot/key-flash idle stays dim 12. Build green. Commit.
- [x] 6. support files — verdicts recorded here; partitions.csv / tools / tests untouched (no invented churn).

Rule: one file at a time, fully finished + build-verified + committed before advancing.

**Now:** flash the fresh C6 build (plug C6 USB for flash mode, then from `c6-firmware/`):
`idf.py -p COM35 flash` — bin = `build/p4_keylogger_c6_ui.bin` (latest 986064 B).
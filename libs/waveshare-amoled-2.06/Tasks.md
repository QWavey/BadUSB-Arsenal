# Tasks

## GitHub issues (synced 2026-10-24)

Open issues in QWavey/ESP32-S3-Waveshare-OLED-Watch: #1 (audit, 30 bullets), #2 to #6 (single items). Several #1 bullets repeat #2 to #6 and are cross-referenced. Paths are relative to the repo root. Line numbers refer to the code as it was at the start of this pass. No item is HELD: none asks for a change to attack or evasion behaviour.

Issue #1, security hygiene:

- [x] #1 moveFile deletes source after a partial copy — ESP_DISPLAY_TOUCH/SD/SDMounter.cpp:452-455 — FIX — fixed in 71dc6fa
- [x] #1 Password-mode text written to serial log — ESP_DISPLAY_TOUCH/UI/SimpleUI.cpp:451-452 — FIX — fixed in 7953364
- [x] #1 truncateFile ignores failures and deletes original first — ESP_DISPLAY_TOUCH/SD/SDMounter.cpp:378-390 — FIX — fixed in e7aec80
- [x] #1 format() reports success after failed deletions — ESP_DISPLAY_TOUCH/SD/SDMounter.cpp:110-127 — FIX — fixed in 5880e29
- [x] #1 UIKeyboard keeps a raw pointer to its textbox — ESP_DISPLAY_TOUCH/UI/SimpleUI.cpp:95, 436-457 — FIX — fixed in 0e055e2 (link cleared on textbox destruction, on LVGL delete, and re-read after the Enter callback)
- [x] #1 Unbounded memory and recursion driven by card contents — ESP_DISPLAY_TOUCH/SD/SDMounter.cpp:248-260, 517-580, 987-1016 — FIX — fixed in 9f349f2 (readFile refuses files over 64 KB, listings capped at 512 entries, recursion capped at depth 8; streaming large files not implemented)
- [x] #1 Path arguments are not confined and the raw FS is exposed — ESP_DISPLAY_TOUCH/SD/SDMounter.cpp:880-891; ESP_DISPLAY_TOUCH/SD/SDMounter.h:89 — FIX — fixed in b21b54e ('.' and '..' resolved and clamped at the card root; getSD() kept and documented as unchecked)
- [x] #1 RTC set from unauthenticated NTP without checks — examples/time.cpp:21, 80-89 — FIX — fixed in d7074f7 (year must be 2024-2099; NTS not available on the Arduino ESP32 core, not used)
- [x] #1 Credential literals in examples and no .gitignore — examples/time.cpp:19-20; repo root — FIX — fixed in 51e2e37 (credentials moved to git-ignored wifi_secrets.h, template added, .gitignore added, README updated)
- [ ] #1 Dependencies unpinned and README asks for latest — README.md:33, 48-57, 75, 80 — SKIP — skipped: improvement only; no defect shown, and the repo has no manifest to pin against

Issue #1, bugs:

- [x] #1 moveFile deletes the source after a short copy — ESP_DISPLAY_TOUCH/SD/SDMounter.cpp:443, 453-454, 979-981 — FIX — fixed in 71dc6fa (same fix as the first moveFile item)
- [x] #1 truncateFile removes the original before the replacement is confirmed — ESP_DISPLAY_TOUCH/SD/SDMounter.cpp:378, 386-390 — FIX — fixed in e7aec80 (same fix as truncateFile above)
- [x] #1 Hot-swap polling silently remounts the card after unmount() — ESP_DISPLAY_TOUCH/SD/SDMounter.cpp:796-801, 804-808, 917-934 — FIX — fixed in a5a9320 (same fix as #3)
- [x] #1 copyFile with identical source and destination truncates the source — ESP_DISPLAY_TOUCH/SD/SDMounter.cpp:428-432, 453 — FIX — fixed in 71dc6fa
- [x] #1 Fade-down computes a uint8_t from a negative float — ESP_DISPLAY_TOUCH/BRIGHTNESS/AMOLEDBrightness.cpp:289-290 — FIX — fixed in 2e8bae9
- [x] #1 Keyboard loses its text box link when re-spawned — ESP_DISPLAY_TOUCH/UI/SimpleUI.cpp:397-405, 436-442 — FIX — fixed in 482bb7e (same fix as #5)
- [x] #1 onClick() registers another LVGL handler on every call — ESP_DISPLAY_TOUCH/UI/SimpleUI.cpp:232-237, 239-244 — FIX — fixed in 1b59399
- [x] #1 Screen on() does not redraw the UI after off() — ESP_DISPLAY_TOUCH/SCREEN_TOUCH/ESP32-S3-Screen-AMOLED-2.06.h:12-24 — FIX — fixed in 64c3928
- [x] #1 Touch read path rejects edge touches and uses a wrong upper bound — ESP_DISPLAY_TOUCH/SCREEN_TOUCH/ESP32-S3-Touch-AMOLED-2.06.h:153 — FIX — fixed in efba7c3 (zero is still treated as "no reading", because readRawXSafe() returns 0 on an I2C error)
- [ ] #1 mount() format fallback never runs and insert-time auto-mount ignores its flag — ESP_DISPLAY_TOUCH/SD/SDMounter.cpp:37-46, 839-858 — FIX — partly fixed in a5a9320 and 5880e29 (insert-time mount now gated on auto_mount_enabled, unreachable block removed, misleading message removed). Open: the format fallback itself is not implemented, because it needs a destructive FAT format path. format_if_failed is accepted and reported as not implemented.

Issue #1, improvements:

- [x] #1 truncateFile deletes the original before its write and rename are checked — ESP_DISPLAY_TOUCH/SD/SDMounter.cpp:372-390 — FIX — fixed in e7aec80 (same fix as truncateFile above)
- [x] #1 copyFile and moveFile destroy the source when source and destination are the same — ESP_DISPLAY_TOUCH/SD/SDMounter.cpp:428-455 — FIX — fixed in 71dc6fa (same fix as the copyFile identical-path bug above)
- [x] #1 Keyboard Enter prints the typed text to Serial, including password boxes — ESP_DISPLAY_TOUCH/UI/SimpleUI.cpp:448-452 — FIX — fixed in 7953364 (same fix as the password-mode item above)
- [x] #1 Brightness state is committed only when a fade ends — ESP_DISPLAY_TOUCH/BRIGHTNESS/AMOLEDBrightness.cpp:92-105, 145, 259, 272-293 — FIX — fixed in 2d94fe3
- [x] #1 onClick adds a new event registration on every call — ESP_DISPLAY_TOUCH/UI/SimpleUI.cpp:215-237 — FIX — fixed in 1b59399 (same fix as the onClick bug above)
- [ ] #1 Code calls LVGL 8 API names while the README requires LVGL 9 — ESP_DISPLAY_TOUCH/UI/SimpleUI.cpp:120, 137, 157, 218; ESP_DISPLAY_TOUCH/BATTERY/BatteryDesign.cpp:148, 234, 283; ESP_DISPLAY_TOUCH/BRIGHTNESS/AMOLEDBrightness.cpp:18 — FIX — partly fixed in e25157a and 21b428d (library renamed, LVGL 9 guard added). Open: examples/ still use the LVGL 8 names (outside the cited files; they still build with LVGL 9.5's default compatibility mapping)
- [x] #1 Hot-swap polling re-initializes the SD controller every 200 ms without a card — ESP_DISPLAY_TOUCH/SD/SDMounter.cpp:804-815, 917-934, 796-802 — FIX — fixed in a5a9320 (same as #3)
- [ ] #1 mount() and format() ignore flags and report success after failures — ESP_DISPLAY_TOUCH/SD/SDMounter.cpp:37-46, 85-128, 169 — FIX — partly fixed in 5880e29 (format returns false on any failed delete; autoMount uses the configured mount point and 1-bit mode). Open: format_if_failed is still not implemented, and format() is not renamed
- [x] #1 Keyboard link is lost when linkTo() runs before spawn() or spawn() runs again — ESP_DISPLAY_TOUCH/UI/SimpleUI.cpp:397-442 — FIX — fixed in 482bb7e (same as #5)
- [x] #1 SimpleUI.cpp repeats every class declaration from SimpleUI.h — ESP_DISPLAY_TOUCH/UI/SimpleUI.cpp:1-113 — FIX — fixed in 69f3b35 (same as #6)

Issues #2 to #6 (each issue is one item):

- [ ] #2 Code calls LVGL 8 API names while the README requires LVGL 9 — ESP_DISPLAY_TOUCH/UI/SimpleUI.cpp:120, 137, 157, 218; ESP_DISPLAY_TOUCH/BATTERY/BatteryDesign.cpp:148, 234, 283; ESP_DISPLAY_TOUCH/BRIGHTNESS/AMOLEDBrightness.cpp:18 — FIX — partly fixed in e25157a and 21b428d. Library renamed to LVGL 9 names (lv_timer_handler, lv_screen_load, lv_screen_active, lv_button_create, lv_obj_delete, lv_obj_set_style_anim_duration) and a version guard added to SimpleUI.h. Open: examples/ not changed
- [x] #3 Hot-swap polling re-initializes the SD controller every 200 ms without a card — ESP_DISPLAY_TOUCH/SD/SDMounter.cpp:804-815, 917-934, 796-802 — FIX — fixed in a5a9320 (probe releases the driver; 1 s back-off while the slot is empty; isInserted()/isRemoved() read the polled state when hot-swap is on; no card-detect pin used because the board's pin is not in the repo)
- [ ] #4 mount() and format() ignore flags and report success after failures — ESP_DISPLAY_TOUCH/SD/SDMounter.cpp:37-46, 85-128, 169 — FIX — partly fixed in 5880e29 (same as the #1 copy above). Open: format_if_failed not implemented; format() not renamed (for example wipeRoot())
- [x] #5 Keyboard link is lost when linkTo() runs before spawn() or spawn() runs again — ESP_DISPLAY_TOUCH/UI/SimpleUI.cpp:397-442 — FIX — fixed in 482bb7e (spawn re-applies the link) and 0e055e2 (link cleared when a textbox is destroyed)
- [x] #6 SimpleUI.cpp repeats every class declaration from SimpleUI.h — ESP_DISPLAY_TOUCH/UI/SimpleUI.cpp:1-113; ESP_DISPLAY_TOUCH/UI/SimpleUI.h:1-115 — FIX — fixed in 69f3b35

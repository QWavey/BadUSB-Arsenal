# Tasks

## Audit 2026-10-24

- [ ] **[MED]** SD pins overlap octal PSRAM pins — sd:3-5 — MOSI 35, MISO 37 and SCK 36 are GPIO35-37, which ESP32-S3 modules with octal PSRAM (e.g. N8R8/N16R8) use internally, so on such a board the SD card cannot be wired to those pads and the SPI bus will not work. The README does not name the board, so the correct free pins cannot be chosen safely here; left open until the board is confirmed.

## GitHub issues (synced 2026-10-24)

- [ ] #1 Classic ESP32: CS, MOSI and SCK on input-only GPIOs — sd:2, sd:3, sd:5 — SKIP — not applicable: the repo targets ESP32-S3 (repo description); this applies only if the target were a classic ESP32
- [ ] #1 Classic ESP32: MISO on GPIO37, which the chip lacks — sd:4 — SKIP — not applicable: the repo targets ESP32-S3 (repo description); this applies only if the target were a classic ESP32
- [ ] #1 ESP32-S3 octal memory: GPIO34-37 reserved internally — sd:2, sd:3, sd:4, sd:5 — FIX — open: the module variant is not named, and free pins cannot be chosen safely without its datasheet. The issue's VSPI example (MISO on GPIO19) must not be used on ESP32-S3, where GPIO19 is USB D-.
- [ ] #1 SD pins all in GPIO 33-37, which octal memory may use — sd:2, sd:3, sd:4, sd:5 — FIX — open: same blocker as the item above; unverified until the module is confirmed
- [x] #1 README has none of the pinout content its description promises — README.md:1 — FIX — fixed in 5bdc6a9
- [ ] #1 Header has no extension and no include guard — sd:1 — SKIP — style only: the header holds only #define lines, and identical macro redefinitions are legal C, so double inclusion does not break the build
- [ ] #1 Pin defines not checked for uniqueness or range — sd:2, sd:3, sd:4, sd:5 — SKIP — improvement only: the current values are unique and inside the ESP32-S3 GPIO range (0-48)

Notes:
- The "Audit 2026-10-24" entry above gives the octal overlap as sd:3-5. CS (sd:2, GPIO34) is in the same GPIO33-37 range, so the overlap is sd:2-5.
- Pin numbers in sd match the earlier audit entry; no other wrong pin numbers or file contradictions were found.

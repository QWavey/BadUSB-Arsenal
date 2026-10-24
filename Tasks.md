# Tasks

## Audit 2026-10-24

- [ ] **[MED]** SD pins overlap octal PSRAM pins — sd:3-5 — MOSI 35, MISO 37 and SCK 36 are GPIO35-37, which ESP32-S3 modules with octal PSRAM (e.g. N8R8/N16R8) use internally, so on such a board the SD card cannot be wired to those pads and the SPI bus will not work. The README does not name the board, so the correct free pins cannot be chosen safely here; left open until the board is confirmed.

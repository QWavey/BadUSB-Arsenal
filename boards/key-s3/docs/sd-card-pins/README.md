# ESP-KEY-SD-CARD-PINS

## SD card pins

The `sd` header defines the SD card SPI pins as preprocessor macros (no code):

| Signal | Macro         | GPIO |
|--------|---------------|------|
| CS     | `SD_CS_PIN`   | 34   |
| MOSI   | `SD_MOSI_PIN` | 35   |
| MISO   | `SD_MISO_PIN` | 37   |
| SCK    | `SD_SCK_PIN`  | 36   |

- **Target chip:** ESP32-S3. These GPIO numbers do not work on a classic ESP32 (GPIO34-36 are input-only there and GPIO37 does not exist).
- **Module variant:** not recorded in this repo. ESP32-S3 modules with octal PSRAM (for example N8R8 or N16R8) use GPIO33-37 internally. Check the module datasheet before wiring the SD card to these pins.
- **SPI host:** the header does not choose one. The code that includes it selects the host.
- **Include:** `#include "sd"`

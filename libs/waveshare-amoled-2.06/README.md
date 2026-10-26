<p align="center">
  <a href="https://www.waveshare.com/wiki/ESP32-S3-Touch-AMOLED-2.06" target="_blank">
    <img width="270" height="203" alt="ESP32-S3-Touch-AMOLED-2.06 board" src="https://github.com/user-attachments/assets/3c2f57c0-1153-4300-82ca-c60a3f09c733" />
  </a>
</p>

# ESP32-S3 AMOLED 2.06" display library

An Arduino library for the Waveshare ESP32-S3 Touch AMOLED 2.06" display. It handles the FT3168 touch controller and the display, and it integrates with LVGL. It provides two classes:

- **`TouchClass`** reads capacitive touch from the FT3168 over I2C.
- **`ScreenClass`** drives the display through Arduino GFX and Arduino DriveBus, and renders with LVGL.

## Features

### TouchClass

- Initializes the FT3168 over I2C and detects its address automatically.
- Registers as an LVGL input device.
- `isTouched()`: checks whether the screen is being touched right now.
- `getTouchPoint(x, y)`: returns the raw touch coordinates.

### ScreenClass

- Initializes the display hardware through Arduino GFX and DriveBus.
- Renders through LVGL.
- Turns the display on and off while LVGL stays initialized.
- The helpers `button_create()`, `slider_create()`, and `checkbox_create()` are **work in progress and don't fully work yet**.

## Requirements

- Arduino IDE or PlatformIO
- LVGL v9.x (other versions cause compile errors)

### Board settings for the examples

| Setting | Value |
|---|---|
| **Board** | ESP32S3 Dev Module |
| **Flash size** | 32MB (256Mb) |
| **PSRAM** | OPI PSRAM |
| **Partition scheme** | Huge APP (3MB No OTA / 1MB SPIFFS) |
| **CPU frequency** | 240MHz (WiFi) |
| **USB CDC on boot** | Enabled |

### Dependencies

| Demo | What it shows | Dependency library |
| --- | --- | --- |
| 01_HelloWorld | Basic graphics functions. Also a quick way to test display performance and rendering of random text | GFX_Library_for_Arduino |
| 02_GFX_AsciiTable | Prints the ASCII characters in rows and columns, sized to the screen | GFX_Library_for_Arduino |
| 03_LVGL_PCF85063_simpleTime | LVGL displays the current time | LVGL, SensorLib |
| 04_LVGL_QMI8658_ui | LVGL draws an acceleration line chart | LVGL, SensorLib |
| 05_LVGL_AXP2101_ADC_Data | LVGL displays PMIC data | LVGL, XPowersLib |
| 06_LVGL_Arduino_v9 | LVGL demo | LVGL, Arduino_DriveBus |
| 07_LVGL_SD_Test | LVGL displays the contents of files on the TF card | LVGL |
| 08_ES8311 | ES8311 driver demo, plays a simple audio clip | none |

You also need a `pin_config.h` file in the same directory, with your pin definitions.

The Wi-Fi example (`examples/time.cpp`) reads its credentials from `wifi_secrets.h`. Copy `examples/wifi_secrets.example.h` to `examples/wifi_secrets.h` and enter your network name and password. Both `pin_config.h` and `wifi_secrets.h` are listed in `.gitignore`, so keep them out of commits.

## Links

- **Arduino libraries:** https://github.com/waveshareteam/ESP32-S3-Touch-AMOLED-2.06/tree/main/examples/Arduino-v3.2.0/libraries
- **ESP-IDF libraries:** https://github.com/waveshareteam/ESP32-S3-Touch-AMOLED-2.06/tree/main/examples/ESP-IDF-v5.4.2
- **Wiki:** https://www.waveshare.com/wiki/ESP32-S3-Touch-AMOLED-2.06

## Installation

1. Download or clone this repository.
2. Copy it into your Arduino libraries folder:
   - Windows: `Documents\Arduino\libraries\ESP_TOUCH_DISPLAY\`
   - macOS: `~/Documents/Arduino/libraries/ESP_TOUCH_DISPLAY/`
   - Linux: `~/Arduino/libraries/ESP_TOUCH_DISPLAY/`
3. Restart the Arduino IDE.
4. Install the dependencies listed above.

### Compilation errors

- Use LVGL v9.x.
- Update all dependency libraries to their latest versions.
- Make sure global instances are declared properly.

## License

Provided as-is, for use with the ESP32-S3 AMOLED 2.06" display.

## Contributing

Issues and pull requests are welcome.

## Status

Work in progress. The API may change.

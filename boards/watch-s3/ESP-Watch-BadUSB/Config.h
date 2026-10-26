#ifndef CONFIG_H
#define CONFIG_H

// Watch port of ESP-S3-Key-BadUSB. The Waveshare ESP32-S3-Touch-AMOLED-2.06
// board uses:
//   * SD on the 1-bit SDMMC bus (not SPI). We alias `SD` -> `SD_MMC` so the
//     upstream FSManager code compiles unchanged.
//   * No addressable RGB LED — GpioLed.h forwards colour pushes to the
//     AMOLED status widget instead.
//   * BOOT (GPIO0) is the only physical button.
// The AP name/pass keep the ESP32-BadUSB defaults — the same SSID the
// dashboard docs reference and the flasher hub shows.
#include <Arduino.h>
#include <SD_MMC.h>
#include "pin_config.h"

// SD compatibility shim: the upstream FSManager was written for the SPI
// SD library and calls SD.open/exists/remove/etc. Both SD and SD_MMC
// derive from fs::FS and expose the same file API, so a single define
// swaps every call site with no diff. `begin()` and `setPins()` are
// handled directly in FSManager.
#ifndef SD
#define SD SD_MMC
#endif

// Reset button pin (BOOT on the watch — same as the Key).
#define RESET_BUTTON_PIN WATCH_BUTTON_PIN

// Legacy LED pin field kept so GpioLed's (NUMPIXELS, LED_PIN) constructor
// still compiles. The value is unused on the watch — the GpioLed shim
// renders to the AMOLED, not to a GPIO.
#define LED_PIN   1
#define NUMPIXELS 1

// SD "pin" macros only referenced by any lingering SPI-init path — kept
// for compile compatibility. FSManager for the watch uses the SDMMC_*
// symbols from pin_config.h instead.
#define SD_CS_PIN   SDMMC_CS
#define SD_MOSI_PIN SDMMC_CMD
#define SD_MISO_PIN SDMMC_DATA
#define SD_SCK_PIN  SDMMC_CLK

// Default WiFi AP configuration.
#define DEFAULT_AP_SSID     "ESP-Watch-BadUSB"
#define DEFAULT_AP_PASSWORD "badusb123"

// Intervals and limits — SD_CHECK_INTERVAL is bumped from the Key's
// 1000 ms to 5000 ms. The Key's 1 Hz cardType() poll on the shared
// SDMMC bus (in 1-bit mode on the watch) contended with the composite
// MSC callbacks and produced transient CARD_NONE reads that triggered
// setLEDMode(2) + a redraw storm. 5 s is plenty for user-observable
// hot-swap detection.
#define SD_CHECK_INTERVAL      5000
#define STATUS_UPDATE_INTERVAL 5000
#define MAX_HISTORY_SIZE       50
#define WIFI_SCAN_TIMEOUT      5000
#define LOG_SESSION_START_MARKER "=== Log Session Started ==="
#define LOG_SESSION_END_MARKER   "=== Log Session Ended ==="

// File system paths (unchanged).
#define DIR_LANGUAGES "/languages"
#define DIR_SCRIPTS   "/scripts"
#define DIR_LOGS      "/logs"
#define DIR_UPLOADS   "/uploads"
#define FILE_HISTORY  "/logs/history.txt"
#define FILE_LOG      "/logs/log.txt"
#define FILE_DEBUG    "/logs/debug.txt"
#define FILE_INDEX    "/index.html"

#endif // CONFIG_H

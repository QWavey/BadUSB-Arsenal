// Waveshare ESP32-S3 Touch AMOLED 2.06" pin map — used by the vendor
// Screen/Touch drivers we depend on and by SD_MMC init.
#pragma once
#define XPOWERS_CHIP_AXP2101

// AMOLED CO5300 over QSPI (SPI2/FSPI)
#define LCD_SDIO0   4
#define LCD_SDIO1   5
#define LCD_SDIO2   6
#define LCD_SDIO3   7
#define LCD_SCLK    11
#define LCD_CS      12
#define LCD_RESET   8
#define LCD_WIDTH   410
#define LCD_HEIGHT  502

// FT3168 capacitive touch on the shared I2C bus (AXP2101 lives here too)
#define IIC_SDA     15
#define IIC_SCL     14
#define TP_INT      38
#define TP_RESET    9

// microSD in 1-bit SDMMC mode (verified working with the vendor
// ShowFiles(SD_MMC_SCREEN) example).
static const int SDMMC_CLK  = 2;
static const int SDMMC_CMD  = 1;
static const int SDMMC_DATA = 3;
static const int SDMMC_CS   = 17;

// BOOT is the only physical push button on the watch.
#ifndef WATCH_BUTTON_PIN
#define WATCH_BUTTON_PIN 0
#endif

// SD card over SPI (the "3V3 / CS / MOSI / CLK / MISO / GND" adapter
// style, as opposed to the wider SDMMC 4-bit bus). Wired on the P4-
// MINI-B-M-EXT to spare GPIO pins picked to not collide with the SDIO
// link to the C6 (GPIO 14-19), the USB PHY (24-25), the status LED
// (GPIO 10) or the C6 reset line (GPIO 54).
//
// Default wiring — override with -D or edit here:
//   SD adapter -> P4 GPIO
//   3V3        -> 3V3
//   GND        -> GND
//   CS         -> GPIO 21
//   MOSI       -> GPIO 20
//   CLK        -> GPIO 23
//   MISO       -> GPIO 22
//
// The card mounts as /sdcard as a FATFS volume. Non-fatal if the card
// isn't present at boot — call sdcard_present() before touching files.

#include "sdcard.h"

#include "esp_log.h"
#include "esp_err.h"
#include "esp_vfs_fat.h"
#include "sdmmc_cmd.h"
#include "driver/spi_common.h"
#include "driver/sdspi_host.h"

static const char *TAG = "sdcard";

// User's SD adapter labels -> P4 GPIOs. Change here if you rewire.
#ifndef SDCARD_PIN_CS
#define SDCARD_PIN_CS    21
#endif
#ifndef SDCARD_PIN_MOSI
#define SDCARD_PIN_MOSI  20
#endif
#ifndef SDCARD_PIN_CLK
#define SDCARD_PIN_CLK   23
#endif
#ifndef SDCARD_PIN_MISO
#define SDCARD_PIN_MISO  22
#endif

// SPI2 is the general-purpose SPI on ESP32-P4 (SPI1 is flash). SPI3 is
// also usable but we pick SPI2 for compatibility.
#define SDCARD_SPI_HOST     SPI2_HOST
#define SDCARD_MOUNT_POINT  "/sdcard"

static sdmmc_card_t *s_card = NULL;
static bool          s_bus_up = false;

// Bring up the SPI bus (once) then probe + mount the card. Returns
// ESP_OK on success. Failure is logged as a warning — the rest of the
// firmware runs fine without a card, so callers just check sdcard_present().
esp_err_t sdcard_mount(void) {
    if (s_card) return ESP_OK;
    esp_err_t err;

    if (!s_bus_up) {
        spi_bus_config_t bus = {
            .mosi_io_num     = SDCARD_PIN_MOSI,
            .miso_io_num     = SDCARD_PIN_MISO,
            .sclk_io_num     = SDCARD_PIN_CLK,
            .quadwp_io_num   = -1,
            .quadhd_io_num   = -1,
            // Match esp_vfs_fat_sdspi_mount's internal buffer size so it can
            // do full-sector reads/writes in one DMA transaction.
            .max_transfer_sz = 4000,
        };
        err = spi_bus_initialize(SDCARD_SPI_HOST, &bus, SDSPI_DEFAULT_DMA);
        if (err != ESP_OK) {
            ESP_LOGW(TAG, "spi_bus_initialize: %s", esp_err_to_name(err));
            return err;
        }
        s_bus_up = true;
    }

    sdmmc_host_t host = SDSPI_HOST_DEFAULT();
    host.slot = SDCARD_SPI_HOST;

    sdspi_device_config_t dev = SDSPI_DEVICE_CONFIG_DEFAULT();
    dev.gpio_cs = SDCARD_PIN_CS;
    dev.host_id = SDCARD_SPI_HOST;

    esp_vfs_fat_sdmmc_mount_config_t mcfg = {
        .format_if_mount_failed = false,   // don't nuke an existing card
        .max_files              = 8,
        .allocation_unit_size   = 16 * 1024,
    };

    err = esp_vfs_fat_sdspi_mount(SDCARD_MOUNT_POINT, &host, &dev, &mcfg, &s_card);
    if (err != ESP_OK) {
        ESP_LOGW(TAG, "SD (SPI) mount failed: %s (no card / bad wiring?)", esp_err_to_name(err));
        s_card = NULL;
        return err;
    }
    sdmmc_card_print_info(stdout, s_card);
    ESP_LOGI(TAG, "SD (SPI) mounted at %s — CS=%d MOSI=%d CLK=%d MISO=%d",
             SDCARD_MOUNT_POINT, SDCARD_PIN_CS, SDCARD_PIN_MOSI,
             SDCARD_PIN_CLK, SDCARD_PIN_MISO);
    return ESP_OK;
}

bool sdcard_present(void) { return s_card != NULL; }

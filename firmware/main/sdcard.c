// SD card scaffold — the P4 has a second SDMMC controller (SLOT_0) that
// the M-EXT board leaves free (SLOT_1 is already the SDIO link to the
// C6). This file mounts a MicroSD stack over SLOT_0 on user-configurable
// pins so DuckyScript payloads can live on the card and survive reflash.
//
// Pin defaults below match the Spotpear MINI-B-M-EXT's exposed SDIO
// header — override in sdkconfig or by editing SDCARD_PIN_* if your
// build uses different pins. If the card doesn't mount (no card, wrong
// pins, wiring fault) we log a warning and continue — the rest of the
// firmware doesn't depend on the SD card being present.

#include "sdcard.h"

#include "esp_log.h"
#include "esp_err.h"
#include "esp_vfs_fat.h"
#include "sdmmc_cmd.h"
#include "driver/sdmmc_host.h"

static const char *TAG = "sdcard";

// Adjust to match the board you're wiring the SD reader into. Defaults
// pick spare GPIOs on the ESP32-P4-MINI-B-M-EXT header — the user said
// "we have pins" but did not pin down which ones, so these are best-
// guess defaults consistent with the board's SDMMC0 breakout. Edit as
// needed and rebuild — no code changes needed beyond the four #defines.
#ifndef SDCARD_PIN_CLK
#define SDCARD_PIN_CLK   43
#endif
#ifndef SDCARD_PIN_CMD
#define SDCARD_PIN_CMD   44
#endif
#ifndef SDCARD_PIN_D0
#define SDCARD_PIN_D0    39
#endif
#ifndef SDCARD_PIN_D1
#define SDCARD_PIN_D1    40
#endif
#ifndef SDCARD_PIN_D2
#define SDCARD_PIN_D2    41
#endif
#ifndef SDCARD_PIN_D3
#define SDCARD_PIN_D3    42
#endif

#define SDCARD_MOUNT_POINT   "/sdcard"

static sdmmc_card_t *s_card = NULL;

// Mount the SD card. Non-fatal on failure — a system without an SD
// reader wired keeps running. Idempotent: multiple calls are no-ops
// after the first success.
esp_err_t sdcard_mount(void) {
    if (s_card) return ESP_OK;
    sdmmc_host_t host = SDMMC_HOST_DEFAULT();
    host.slot = SDMMC_HOST_SLOT_0;      // SLOT_1 is the SDIO link to C6
    host.flags = SDMMC_HOST_FLAG_4BIT | SDMMC_HOST_FLAG_ALLOC_ALIGNED_BUF;
    host.max_freq_khz = SDMMC_FREQ_DEFAULT;

    sdmmc_slot_config_t slot = SDMMC_SLOT_CONFIG_DEFAULT();
    slot.clk = SDCARD_PIN_CLK;
    slot.cmd = SDCARD_PIN_CMD;
    slot.d0  = SDCARD_PIN_D0;
    slot.d1  = SDCARD_PIN_D1;
    slot.d2  = SDCARD_PIN_D2;
    slot.d3  = SDCARD_PIN_D3;
    slot.width = 4;
    slot.flags = SDMMC_SLOT_FLAG_INTERNAL_PULLUP;

    esp_vfs_fat_sdmmc_mount_config_t mcfg = {
        .format_if_mount_failed = false,   // don't nuke an existing card
        .max_files              = 8,
        .allocation_unit_size   = 16 * 1024,
    };

    esp_err_t err = esp_vfs_fat_sdmmc_mount(SDCARD_MOUNT_POINT, &host, &slot, &mcfg, &s_card);
    if (err != ESP_OK) {
        ESP_LOGW(TAG, "SD mount failed: %s (no card or wrong pins?)", esp_err_to_name(err));
        s_card = NULL;
        return err;
    }
    sdmmc_card_print_info(stdout, s_card);
    ESP_LOGI(TAG, "SD mounted at %s (%llu MiB)", SDCARD_MOUNT_POINT,
             ((uint64_t)s_card->csd.capacity) * s_card->csd.sector_size / (1024 * 1024));
    return ESP_OK;
}

bool sdcard_present(void) { return s_card != NULL; }

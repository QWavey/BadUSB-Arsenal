// DEVICE-ONLY TEST — TinyUSB HID on HS-OTG.
// Purpose: figure out whether U4 with the switch on "1 Slave" routes the
// P4's HS-OTG directly to U4 (bypassing the CH334 hub). If it does, the PC
// on U4 will see a new HID keyboard (VID 303A / PID 4004). If not, U4-Slave
// is not a direct HS-OTG-device port and we need a hardware mod to get
// HID passthrough on this board.
//
// This build does NOT install USB host on HS, so we intentionally lose the
// ability to read keyboards. It's a routing test only.

#include <string.h>
#include "esp_log.h"
#include "esp_err.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"

#include "tinyusb.h"
#include "class/hid/hid_device.h"

static const char *TAG = "dev_test";

enum { REPORT_ID_KEYBOARD = 1, REPORT_ID_MOUSE = 2 };

static const uint8_t hid_report_desc[] = {
    TUD_HID_REPORT_DESC_KEYBOARD(HID_REPORT_ID(REPORT_ID_KEYBOARD)),
    TUD_HID_REPORT_DESC_MOUSE   (HID_REPORT_ID(REPORT_ID_MOUSE))
};

static const char *string_desc[] = {
    (const char[]){0x09, 0x04},
    "Spotpear",
    "ESP32-P4 HID Device Test",
    "P4HIDDEV-0001",
    "HID Device Test"
};

#define CFG_TOTAL_LEN (TUD_CONFIG_DESC_LEN + TUD_HID_DESC_LEN)
static const uint8_t configuration_desc[] = {
    TUD_CONFIG_DESCRIPTOR(1, 1, 0, CFG_TOTAL_LEN, TUSB_DESC_CONFIG_ATT_REMOTE_WAKEUP, 100),
    TUD_HID_DESCRIPTOR(0, 4, HID_ITF_PROTOCOL_NONE, sizeof(hid_report_desc), 0x81, 16, 10),
};

uint8_t const *tud_hid_descriptor_report_cb(uint8_t instance) { (void)instance; return hid_report_desc; }
uint16_t tud_hid_get_report_cb(uint8_t i, uint8_t r, hid_report_type_t t, uint8_t *b, uint16_t rl) {
    (void)i; (void)r; (void)t; (void)b; (void)rl; return 0;
}
void tud_hid_set_report_cb(uint8_t i, uint8_t r, hid_report_type_t t, uint8_t const *b, uint16_t bs) {
    (void)i; (void)r; (void)t; (void)b; (void)bs;
}

void app_main(void) {
    ESP_LOGW(TAG, "==== DEVICE-ONLY test on HS-OTG ====");
    ESP_LOGW(TAG, " Plug U4 (switch = 1 Slave) into PC.");
    ESP_LOGW(TAG, " Expected: PC sees new HID device VID=303a PID=4004.");
    ESP_LOGW(TAG, " If yes -> U4-Slave routes HS-OTG directly.");

    const tinyusb_config_t tcfg = {
        .device_descriptor = NULL,
        .string_descriptor = string_desc,
        .string_descriptor_count = sizeof(string_desc)/sizeof(*string_desc),
        .external_phy = false,
        .configuration_descriptor = configuration_desc,
    };
    ESP_ERROR_CHECK(tinyusb_driver_install(&tcfg));
    ESP_LOGW(TAG, " TinyUSB installed on HS. Waiting for mount...");

    uint32_t t = 0;
    uint8_t keycodes[6] = {0};
    while (1) {
        vTaskDelay(pdMS_TO_TICKS(2000));
        t += 2;
        ESP_LOGI(TAG, "t=%us  tud_mounted=%d  tud_ready=%d",
                 (unsigned)t, tud_mounted() ? 1 : 0, tud_ready() ? 1 : 0);
        // If mounted, every ~4 s send a fake keystroke of 'A' so we visibly
        // prove passthrough works (a key will appear in your text editor).
        if (tud_mounted() && (t % 4 == 0)) {
            keycodes[0] = 0x04;  // HID usage 'A'
            tud_hid_keyboard_report(REPORT_ID_KEYBOARD, 0, keycodes);
            vTaskDelay(pdMS_TO_TICKS(50));
            keycodes[0] = 0;
            tud_hid_keyboard_report(REPORT_ID_KEYBOARD, 0, keycodes);
            ESP_LOGI(TAG, " sent 'a' keystroke");
        }
    }
}

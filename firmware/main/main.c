// TinyUSB device side + entry point. Presents an HID composite (keyboard+mouse)
// to the PC on the FS OTG port (the "core" Type-C / U4-Slave). Reads reports
// from the bridge queue that hid_host_task.c fills with keystrokes coming in
// from the real keyboard on the HS port.

#include <string.h>
#include "esp_log.h"
#include "esp_err.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"

#include "tinyusb.h"
#include "class/hid/hid_device.h"

#include "hid_bridge.h"

// Runtime USB PHY selector — swaps FS_PHY1↔FS_PHY2 assignments.
// After swap: OTG_FS drives GPIO24/25 (the core Type-C on this board).
// USJ (COM34) is gone until next reset with BOOT held (ROM ignores this reg).
#define LP_SYSTEM_USB_CTRL_REG   0x50110100u
#define BIT_SW_HW_USB_PHY_SEL    (1u << 0)   // 1 = use SW override below
#define BIT_SW_USB_PHY_SEL       (1u << 1)   // 1 = swap PHY assignments

static void usb_phy_swap_to_otgfs_on_core_typec(void)
{
    volatile uint32_t *reg = (volatile uint32_t *)LP_SYSTEM_USB_CTRL_REG;
    *reg |= BIT_SW_HW_USB_PHY_SEL | BIT_SW_USB_PHY_SEL;
}

static const char *TAG = "hid_dev";

enum { REPORT_ID_KEYBOARD = 1, REPORT_ID_MOUSE = 2 };

static const uint8_t hid_report_desc[] = {
    TUD_HID_REPORT_DESC_KEYBOARD(HID_REPORT_ID(REPORT_ID_KEYBOARD)),
    TUD_HID_REPORT_DESC_MOUSE   (HID_REPORT_ID(REPORT_ID_MOUSE))
};

static const char *string_desc[] = {
    (const char[]){0x09, 0x04},   // 0: English (US)
    "Spotpear",                   // 1: Manufacturer
    "ESP32-P4 HID Passthrough",   // 2: Product
    "P4HIDPT-0001",               // 3: Serial
    "HID Passthrough"             // 4: HID interface
};

#define CFG_TOTAL_LEN (TUD_CONFIG_DESC_LEN + TUD_HID_DESC_LEN)

static const uint8_t configuration_desc[] = {
    TUD_CONFIG_DESCRIPTOR(1, 1, 0, CFG_TOTAL_LEN, TUSB_DESC_CONFIG_ATT_REMOTE_WAKEUP, 100),
    // Interval byte = 1 ms poll instead of 10 ms — cuts one-way latency by 9 ms
    TUD_HID_DESCRIPTOR(0, 4, HID_ITF_PROTOCOL_NONE, sizeof(hid_report_desc), 0x81, 16, 1),
};

// TinyUSB required callbacks
uint8_t const *tud_hid_descriptor_report_cb(uint8_t instance) {
    (void)instance;
    return hid_report_desc;
}
uint16_t tud_hid_get_report_cb(uint8_t instance, uint8_t report_id, hid_report_type_t report_type,
                               uint8_t *buffer, uint16_t reqlen) {
    (void)instance; (void)report_id; (void)report_type; (void)buffer; (void)reqlen;
    return 0;
}
void tud_hid_set_report_cb(uint8_t instance, uint8_t report_id, hid_report_type_t report_type,
                           uint8_t const *buffer, uint16_t bufsize) {
    (void)instance; (void)report_id; (void)report_type; (void)buffer; (void)bufsize;
}

// Boot keyboard input report layout: modifier(1) + reserved(1) + keycode[6]
// Boot mouse input report layout:    buttons(1) + dx(int8) + dy(int8) [+ wheel]

static void forward_task(void *arg) {
    hid_bridge_msg_t m;
    while (1) {
        if (xQueueReceive(g_hid_bridge_q, &m, portMAX_DELAY) != pdTRUE) continue;
        if (!tud_mounted()) continue;

        // If TinyUSB is still transmitting the previous IN packet, wait very
        // briefly for it to drain. Skips only after a hard 5 ms stall which
        // means the PC isn't polling — dropping is preferable to backing up.
        int spins = 0;
        while (!tud_hid_ready() && spins < 50) {
            vTaskDelay(pdMS_TO_TICKS(1));
            spins++;
        }
        if (!tud_hid_ready()) continue;

        if (m.kind == HID_REP_KIND_KEYBOARD && m.len >= 8) {
            uint8_t modifier = m.data[0];
            uint8_t keycodes[6];
            memcpy(keycodes, &m.data[2], 6);
            tud_hid_keyboard_report(REPORT_ID_KEYBOARD, modifier, keycodes);
        } else if (m.kind == HID_REP_KIND_MOUSE && m.len >= 3) {
            uint8_t buttons = m.data[0];
            int8_t dx = (int8_t)m.data[1];
            int8_t dy = (int8_t)m.data[2];
            tud_hid_mouse_report(REPORT_ID_MOUSE, buttons, dx, dy, 0, 0);
        }
    }
}

void app_main(void) {
    ESP_LOGI(TAG, "Starting HID passthrough on ESP32-P4");
    ESP_LOGW(TAG, "==== SWAPPING USB PHY: core Type-C -> OTG_FS ====");
    ESP_LOGW(TAG, "==== USJ / COM34 will disappear in 3 s; hold BOOT+tap RST to flash ====");
    vTaskDelay(pdMS_TO_TICKS(3000));

    // Runtime PHY swap: after this line, GPIO24/25 (the core Type-C on the MINI)
    // carries the OTG-FS controller instead of USB-Serial-JTAG. TinyUSB HID device
    // configured on FS peripheral (see sdkconfig) will now reach the PC via the
    // core Type-C — exactly the port we've been flashing from.
    usb_phy_swap_to_otgfs_on_core_typec();

    g_hid_bridge_q = xQueueCreate(32, sizeof(hid_bridge_msg_t));
    configASSERT(g_hid_bridge_q);

    const tinyusb_config_t tcfg = {
        .device_descriptor = NULL,   // use default VID/PID from Kconfig
        .string_descriptor = string_desc,
        .string_descriptor_count = sizeof(string_desc) / sizeof(string_desc[0]),
        .external_phy = false,
        .configuration_descriptor = configuration_desc,
    };
    ESP_ERROR_CHECK(tinyusb_driver_install(&tcfg));
    ESP_LOGI(TAG, "TinyUSB HID device up (FS port -> PC)");

    // Priority 10 so the forward task preempts everything else on core 1
    // and hands each report to TinyUSB the instant it lands in the queue.
    xTaskCreatePinnedToCore(forward_task, "hid_fwd", 4096, NULL, 10, NULL, 1);

    hid_host_task_start();

    // Diagnostic heartbeat: prints TinyUSB mount + HID readiness every 3 s.
    // Watch this on the USB-Serial-JTAG (core Type-C, COM34). "mounted=1" means
    // the PC has enumerated our HID composite on whichever Type-C is FS-OTG.
    uint32_t tick = 0;
    while (1) {
        ESP_LOGI(TAG, "heartbeat t=%us  tud_mounted=%d  tud_ready=%d",
                 (unsigned)tick, tud_mounted() ? 1 : 0, tud_ready() ? 1 : 0);
        vTaskDelay(pdMS_TO_TICKS(3000));
        tick += 3;
    }
}

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
#include "c6_uart.h"
#include "led.h"

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
    (void)instance;
    // PC pushes Caps/Num/Scroll Lock state as an OUTPUT report on the keyboard
    // interface. Boot layout is a single byte bitmap. Forward to the physical
    // keyboard so its LEDs stay in sync with the PC.
    if (report_type == HID_REPORT_TYPE_OUTPUT && report_id == REPORT_ID_KEYBOARD && bufsize >= 1) {
        hid_host_set_kbd_leds(buffer[0]);
    }
}

// Boot keyboard input report layout: modifier(1) + reserved(1) + keycode[6]
// Boot mouse input report layout:    buttons(1) + dx(int8) + dy(int8) [+ wheel]

// Send every report in order — for keyboards, intermediate states matter
// (Shift down + Shift+Tab + Tab up must ALL land). Retry the pending report
// until it goes out; TinyUSB's tud_hid_ready flips true on every USB frame
// (1 ms) so worst case we're re-trying a few times.
//
// If the PC isn't mounted (boot, suspend, cable not fully seated) we DROP
// buffered reports rather than hold them — otherwise the whole backlog gets
// dumped into whichever window has focus the moment the PC comes back,
// which the user saw as "usb HID automatic spam" after enumeration.
static void forward_task(void *arg) {
    hid_bridge_msg_t m;
    bool have_pending = false;
    bool was_mounted = false;
    TickType_t mount_grace_end = 0;
    while (1) {
        bool now_mounted = tud_mounted();
        // Detect the unmounted -> mounted edge and open a 400 ms grace window
        // during which we DROP incoming reports. That kills the spam-on-
        // enumerate bug (PC finishes SET_CONFIGURATION, we start dumping the
        // queue before Windows has bound our HID; text lands in the wrong
        // window as a rapid flood).
        if (now_mounted && !was_mounted) {
            mount_grace_end = xTaskGetTickCount() + pdMS_TO_TICKS(400);
        }
        was_mounted = now_mounted;

        if (!have_pending) {
            if (xQueueReceive(g_hid_bridge_q, &m, portMAX_DELAY) != pdTRUE) continue;
            have_pending = true;
        }
        if (!now_mounted || xTaskGetTickCount() < mount_grace_end) {
            // Drop the pending one and drain the queue so we don't hold onto
            // pre-mount / grace-window keystrokes.
            have_pending = false;
            hid_bridge_msg_t drop;
            while (xQueueReceive(g_hid_bridge_q, &drop, 0) == pdTRUE) { /* discard */ }
            vTaskDelay(pdMS_TO_TICKS(20));
            continue;
        }
        if (!tud_hid_ready())  { vTaskDelay(pdMS_TO_TICKS(1)); continue; }

        bool sent = false;
        if (m.kind == HID_REP_KIND_KEYBOARD && m.len >= 8) {
            uint8_t mod = m.data[0];
            uint8_t kcs[6]; memcpy(kcs, &m.data[2], 6);
            sent = tud_hid_keyboard_report(REPORT_ID_KEYBOARD, mod, kcs);
        } else if (m.kind == HID_REP_KIND_MOUSE && m.len >= 3) {
            uint8_t buttons = m.data[0];
            int8_t dx = (int8_t)m.data[1];
            int8_t dy = (int8_t)m.data[2];
            sent = tud_hid_mouse_report(REPORT_ID_MOUSE, buttons, dx, dy, 0, 0);
        } else {
            sent = true;   // unknown kind, drop
        }
        if (sent) have_pending = false;
        else      vTaskDelay(pdMS_TO_TICKS(1));
    }
}

void app_main(void) {
    ESP_LOGI(TAG, "Starting HID passthrough on ESP32-P4");

    // Status LED first so we get a green idle blink as boot progresses.
    led_init();

    // Bring up the SDIO link to the C6 keylogger web UI. Runs in a background
    // task so app_main isn't blocked if SDIO enum fails.
    c6_uart_init();

    // Short pause so any early ESP_LOG lands on USJ before the PHY swap.
    // Removed the long diagnostic window — SDIO is proven working now.
    vTaskDelay(pdMS_TO_TICKS(300));

    // Runtime PHY swap: after this line, GPIO24/25 (the core Type-C on the MINI)
    // carries the OTG-FS controller instead of USB-Serial-JTAG. USJ / COM34 dies.
    usb_phy_swap_to_otgfs_on_core_typec();

    // 128 slots so a brief TinyUSB stall (PC suspend / poll gap) can't overflow
    // and start dropping input reports from the physical keyboard.
    g_hid_bridge_q = xQueueCreate(128, sizeof(hid_bridge_msg_t));
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

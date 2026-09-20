// TinyUSB device side + entry point. Presents an HID composite (keyboard+mouse)
// to the PC on the FS OTG port (the "core" Type-C / U4-Slave). Reads reports
// from the bridge queue that hid_host_task.c fills with keystrokes coming in
// from the real keyboard on the HS port.
//
// Windows-specific fixes:
// - Microsoft OS 2.0 descriptors (BOS + registry property). Deliberately NO
//   "WINUSB" compatible-ID override: that would steal the interface from the
//   HID class driver and break the keyboard.
// - Custom PID 0x4B44 + bumped bcdDevice in sdkconfig.defaults. The default
//   esp_tinyusb PID 0x4004 is shared by many Espressif HID examples; Windows
//   caches drivers per VID:PID(+serial), so a stale cache entry for
//   303A:4004 keeps Windows from re-enumerating the device. A fresh PID makes
//   Windows bind it as a brand-new device.
// - Chip-unique serial string so a second board never collides with this one.
// - 1ms polling retained: the fastest legal Full-Speed interval and fully
//   Windows-legal. 4ms would only add keystroke latency.
// - 250ms settle window after mount + an explicit "all keys released" flush
//   (see forward_task), so a key-release swallowed by the settle window or a
//   host-side hub reset can never leave Windows with a stuck key/modifier.

#include <string.h>
#include <stdint.h>
#include "esp_log.h"
#include "esp_err.h"
#include "esp_mac.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"

#include "tinyusb.h"
#include "class/hid/hid_device.h"
#include "tusb.h"

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

// Serial string is chip-unique (filled from eFuse MAC in app_main) so two
// boards with the same custom PID never present the same identity to Windows.
static char serial_str[16];

static const char *string_desc[] = {
    (const char[]){0x09, 0x04},   // 0: English (US)
    "Spotpear",                   // 1: Manufacturer
    "ESP32-P4 HID Passthrough",   // 2: Product
    serial_str,                   // 3: Serial
    "HID Passthrough"             // 4: HID interface
};

// ---------------------------------------------------------------------------
// Microsoft OS 2.0 descriptors
// ---------------------------------------------------------------------------
// Only the Registry Property form is used (DeviceInterfaceGUIDs). No WINUSB
// compatible-ID override is declared — that would make Windows bind WinUSB
// instead of the HID class driver and break the keyboard. Linux/Android never
// issue the vendor control request, so they are unaffected by this block.
#define VENDOR_REQUEST_MICROSOFT 1
#define MS_OS_20_DESC_LEN        0x9E  // full table length (layout below)

#define BOS_TOTAL_LEN (TUD_BOS_DESC_LEN + TUD_BOS_MICROSOFT_OS_DESC_LEN)

static const uint8_t desc_bos[] = {
    // total length, number of device capabilities
    TUD_BOS_DESCRIPTOR(BOS_TOTAL_LEN, 1),
    // Microsoft OS 2.0 platform capability
    TUD_BOS_MS_OS_20_DESCRIPTOR(MS_OS_20_DESC_LEN, VENDOR_REQUEST_MICROSOFT),
};

static const uint8_t desc_ms_os_20[] = {
    // Set header: length, type, windows version, total length
    U16_TO_U8S_LE(0x000A), U16_TO_U8S_LE(MS_OS_20_SET_HEADER_DESCRIPTOR),
    U32_TO_U8S_LE(0x06030000), U16_TO_U8S_LE(MS_OS_20_DESC_LEN),

    // Configuration subset: length, type, config value, reserved, subset length
    U16_TO_U8S_LE(0x0008), U16_TO_U8S_LE(MS_OS_20_SUBSET_HEADER_CONFIGURATION),
    0x00, 0x00, U16_TO_U8S_LE(0x0094),

    // Function subset for interface 0: length, type, first iface, reserved, subset len
    U16_TO_U8S_LE(0x0008), U16_TO_U8S_LE(MS_OS_20_SUBSET_HEADER_FUNCTION),
    0x00, 0x00, U16_TO_U8S_LE(0x008C),

    // Registry property: length, type
    U16_TO_U8S_LE(0x0084), U16_TO_U8S_LE(MS_OS_20_FEATURE_REG_PROPERTY),
    // REG_MULTI_SZ, name "DeviceInterfaceGUIDs\0" as UTF-16 (42 bytes)
    U16_TO_U8S_LE(0x0007), U16_TO_U8S_LE(0x002A),
    'D', 0x00, 'e', 0x00, 'v', 0x00, 'i', 0x00, 'c', 0x00, 'e', 0x00,
    'I', 0x00, 'n', 0x00, 't', 0x00, 'e', 0x00, 'r', 0x00, 'f', 0x00,
    'a', 0x00, 'c', 0x00, 'e', 0x00, 'G', 0x00, 'U', 0x00, 'I', 0x00,
    'D', 0x00, 's', 0x00, 0x00, 0x00,
    // Property data: "{6F519A81-2F3C-4C6A-9E8B-2A4C9A12730A}\0\0" UTF-16 (80 bytes)
    U16_TO_U8S_LE(0x0050),
    '{', 0x00, '6', 0x00, 'F', 0x00, '5', 0x00, '1', 0x00, '9', 0x00,
    'A', 0x00, '8', 0x00, '1', 0x00, '-', 0x00, '2', 0x00, 'F', 0x00,
    '3', 0x00, 'C', 0x00, '-', 0x00, '4', 0x00, 'C', 0x00, '6', 0x00,
    'A', 0x00, '-', 0x00, '9', 0x00, 'E', 0x00, '8', 0x00, 'B', 0x00,
    '-', 0x00, '2', 0x00, 'A', 0x00, '4', 0x00, 'C', 0x00, '9', 0x00,
    'A', 0x00, '1', 0x00, '2', 0x00, '7', 0x00, '3', 0x00, '0', 0x00,
    'A', 0x00, '}', 0x00, 0x00, 0x00, 0x00, 0x00,
};

_Static_assert(sizeof(desc_ms_os_20) == MS_OS_20_DESC_LEN, "MS OS 2.0 table size mismatch");

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

// BOS descriptor: lets Windows 8.1+ ask for the Microsoft OS 2.0 table below.
uint8_t const *tud_descriptor_bos_cb(void) {
    return desc_bos;
}

// Answers the Microsoft OS 2.0 descriptor query (bRequest=1, wIndex=7).
bool tud_vendor_control_xfer_cb(uint8_t rhport, uint8_t stage,
                                tusb_control_request_t const *request) {
    if (stage != CONTROL_STAGE_SETUP) return true;
    if (request->bmRequestType_bit.type == TUSB_REQ_TYPE_VENDOR &&
        request->bRequest == VENDOR_REQUEST_MICROSOFT &&
        request->wIndex == 7) {
        return tud_control_xfer(rhport, request,
                                (void *)(uintptr_t)desc_ms_os_20, MS_OS_20_DESC_LEN);
    }
    // Stall anything else we don't own.
    return false;
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
//
// Two layers make sure Windows can never keep a stuck key/modifier, which is
// the "keyboard goes crazy" symptom:
//  1. publish_report() on the host side never loses keyboard traffic: if the
//     queue is full it evicts the OLDEST report instead of dropping the newest
//     (a dropped key-UP is what leaves the modifier stuck).
//  2. When the PC re-enumerates, we drop reports for a short 250 ms settle,
//     then send an explicit "all keys released" keyboard report before
//     forwarding live input. Also handled on keyboard re-connect via
//     HID_REP_KIND_KEYBOARD_RESET from hid_host_task.c.
static void forward_task(void *arg) {
    hid_bridge_msg_t m;
    bool have_pending = false;
    bool was_mounted = false;
    bool reset_pending = false;
    TickType_t mount_grace_end = 0;
    while (1) {
        bool now_mounted = tud_mounted();
        // Detect the unmounted -> mounted edge. Open a short settle window
        // (we DROP incoming reports while Windows binds our HID collection),
        // then flush an all-released keyboard report to clear any phantom
        // held keys before live input resumes.
        if (now_mounted && !was_mounted) {
            mount_grace_end = xTaskGetTickCount() + pdMS_TO_TICKS(250);
            reset_pending = true;
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

        // Settle window over: tell the PC no key is held, then forward live
        // input. Without this, a key-release that got dropped during the
        // window leaves Windows holding the key/modifier forever.
        if (reset_pending) {
            static const uint8_t all_zero[6] = {0};
            if (tud_hid_keyboard_report(REPORT_ID_KEYBOARD, 0, all_zero)) {
                reset_pending = false;
            } else {
                vTaskDelay(pdMS_TO_TICKS(1));
                continue;
            }
        }

        bool sent = false;
        if (m.kind == HID_REP_KIND_KEYBOARD && m.len >= 8) {
            uint8_t mod = m.data[0];
            uint8_t kcs[6]; memcpy(kcs, &m.data[2], 6);
            sent = tud_hid_keyboard_report(REPORT_ID_KEYBOARD, mod, kcs);
        } else if (m.kind == HID_REP_KIND_KEYBOARD_RESET) {
            static const uint8_t all_zero[6] = {0};
            sent = tud_hid_keyboard_report(REPORT_ID_KEYBOARD, 0, all_zero);
        } else if (m.kind == HID_REP_KIND_MOUSE && m.len >= 3) {
            uint8_t buttons = m.data[0];
            int8_t dx = (int8_t)m.data[1];
            int8_t dy = (int8_t)m.data[2];
            // Boot mouse report is 3 bytes; some report a 4th byte (wheel).
            // Forward it when present so scroll wheels work end-to-end.
            int8_t wheel = (m.len >= 4) ? (int8_t)m.data[3] : 0;
            sent = tud_hid_mouse_report(REPORT_ID_MOUSE, buttons, dx, dy, wheel, 0);
        } else {
            sent = true;   // unknown kind, drop
        }
        if (sent) have_pending = false;
        else      vTaskDelay(pdMS_TO_TICKS(1));
    }
}

// Mount watchdog for the PC (TinyUSB device side). Whether Windows has bound
// our HID composite was invisible — the C6 UI only knew about the keyboard and
// mouse on the HOST side. Emit EVT PC_CONNECT / PC_DISCONNECT on mount edges so
// the web UI shows a "PC" pill: green = Windows sees the keyboard, red/warn =
// the device isn't mounted and keystrokes are being dropped by design.
static void pc_watch_task(void *arg) {
    (void)arg;
    bool was = false;
    while (1) {
        bool now = tud_mounted();
        if (now != was) {
            was = now;
            c6_uart_send_evt(now ? "PC_CONNECT" : "PC_DISCONNECT");
            ESP_LOGI(TAG, "PC %s", now ? "mounted" : "unmounted");
        }
        vTaskDelay(pdMS_TO_TICKS(300));
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

    // Unique-per-chip serial keeps this device's identity stable across
    // re-flashes while never colliding with a second board on the same PC.
    // The factory MAC is unique per chip; fold its first 4 bytes into the id.
    uint8_t mac[6] = { 0 };
    if (esp_efuse_mac_get_default(mac) != ESP_OK) { /* keep all-zero -> still stable */ }
    uint32_t id = ((uint32_t)mac[0] << 24) | ((uint32_t)mac[1] << 16)
                | ((uint32_t)mac[2] << 8)  |  (uint32_t)mac[3];
    snprintf(serial_str, sizeof(serial_str), "P4HID-%08X", (unsigned int)id);

    // 512 slots so a brief TinyUSB stall (PC suspend / poll gap) can't overflow
    // and start dropping input reports from the physical keyboard. On the rare
    // full queue, publish_report() still guarantees the newest keyboard report
    // (which may be a key release) is never dropped.
    g_hid_bridge_q = xQueueCreate(512, sizeof(hid_bridge_msg_t));
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

    xTaskCreatePinnedToCore(pc_watch_task, "pc_watch", 2048, NULL, 6, NULL, 0);

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

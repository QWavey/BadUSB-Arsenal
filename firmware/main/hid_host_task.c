// USB host side. Reads raw HID reports from a boot keyboard/mouse plugged into
// the HS OTG port (through the CH334 hub on U2/U3) and posts them to the
// shared queue for the TinyUSB-device side to consume.
//
// This file only includes ESP-IDF USB host headers, NOT TinyUSB. Keeping them
// in separate translation units avoids the `hid_report_type_t` clash between
// usb_host_hid/include/usb/hid.h and tinyusb/src/class/hid/hid.h.

#include <string.h>
#include "esp_log.h"
#include "esp_err.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "freertos/queue.h"

#include "usb/usb_host.h"
#include "usb/hid_host.h"
#include "usb/hid_usage_keyboard.h"
#include "usb/hid_usage_mouse.h"

#include "hid_bridge.h"
#include "c6_uart.h"

static const char *TAG = "hid_host";

// Per-interface HID device kind, so DISCONNECTED can emit the right EVT.
// Boot subclass keyboards/mice come in as separate hid_host_device handles;
// we tag them at CONNECTED time.
#define HID_TAG_KBD   1
#define HID_TAG_MOUSE 2

QueueHandle_t g_hid_bridge_q;

// Track the most recently connected boot keyboard so tinyusb's SET_REPORT
// (LED state) can forward to the physical keyboard.
static hid_host_device_handle_t s_kbd_hdev;

void hid_host_set_kbd_leds(uint8_t leds) {
    if (!s_kbd_hdev) return;
    // HID Output report ID 0 for boot keyboard, single byte LED bitmap.
    esp_err_t err = hid_class_request_set_report(s_kbd_hdev,
        HID_REPORT_TYPE_OUTPUT, 0, &leds, 1);
    if (err != ESP_OK) ESP_LOGD(TAG, "set_report(LEDs=0x%02x): %s", leds, esp_err_to_name(err));
}

static void publish_report(hid_rep_kind_t kind, const uint8_t *buf, size_t len) {
    hid_bridge_msg_t m = { .kind = kind, .len = (len > sizeof(m.data)) ? sizeof(m.data) : (uint8_t)len };
    memcpy(m.data, buf, m.len);
    if (g_hid_bridge_q) {
        // Drop on full — passthrough must not stall the USB stack.
        xQueueSend(g_hid_bridge_q, &m, 0);
    }
}

// Callbacks below use LOG-and-CONTINUE instead of ESP_ERROR_CHECK.
// The USB stack sees transient errors when a device is in flux (power dips,
// hub port bounces). Aborting the chip on every glitch is exactly the reboot
// loop we do NOT want — we handle the error and keep serving other devices.

static void hid_iface_cb(hid_host_device_handle_t hdev,
                         const hid_host_interface_event_t event, void *arg) {
    uint8_t buf[64];
    size_t data_len = 0;
    hid_host_dev_params_t p;
    esp_err_t err;

    switch (event) {
    case HID_HOST_INTERFACE_EVENT_INPUT_REPORT:
        err = hid_host_device_get_params(hdev, &p);
        if (err != ESP_OK) { ESP_LOGD(TAG, "get_params in iface_cb: %s", esp_err_to_name(err)); return; }
        err = hid_host_device_get_raw_input_report_data(hdev, buf, sizeof(buf), &data_len);
        if (err != ESP_OK) { ESP_LOGD(TAG, "get_input_report: %s", esp_err_to_name(err)); return; }
        if (p.sub_class == HID_SUBCLASS_BOOT_INTERFACE) {
            if (p.proto == HID_PROTOCOL_KEYBOARD) {
                publish_report(HID_REP_KIND_KEYBOARD, buf, data_len);
                c6_uart_feed_kbd_report(buf, data_len);
            } else if (p.proto == HID_PROTOCOL_MOUSE) {
                publish_report(HID_REP_KIND_MOUSE, buf, data_len);
                c6_uart_feed_mouse_report(buf, data_len);
            }
        }
        break;
    case HID_HOST_INTERFACE_EVENT_DISCONNECTED:
        ESP_LOGI(TAG, "HID iface disconnected");
        // Look up which kind this iface was tagged as at connect time.
        {
            hid_host_dev_params_t pp;
            if (hid_host_device_get_params(hdev, &pp) == ESP_OK &&
                pp.sub_class == HID_SUBCLASS_BOOT_INTERFACE) {
                if (pp.proto == HID_PROTOCOL_KEYBOARD) {
                    if (s_kbd_hdev == hdev) s_kbd_hdev = NULL;
                    c6_uart_kbd_disconnect();
                } else if (pp.proto == HID_PROTOCOL_MOUSE) c6_uart_mouse_disconnect();
            }
        }
        err = hid_host_device_close(hdev);
        if (err != ESP_OK) ESP_LOGW(TAG, "device_close: %s", esp_err_to_name(err));
        break;
    case HID_HOST_INTERFACE_EVENT_TRANSFER_ERROR:
        ESP_LOGD(TAG, "HID transfer error (device probably in flux)");
        break;
    default:
        break;
    }
}

static void hid_device_cb(hid_host_device_handle_t hdev,
                          const hid_host_driver_event_t event, void *arg) {
    if (event != HID_HOST_DRIVER_EVENT_CONNECTED) return;

    hid_host_dev_params_t p;
    esp_err_t err = hid_host_device_get_params(hdev, &p);
    if (err != ESP_OK) { ESP_LOGW(TAG, "connect get_params: %s", esp_err_to_name(err)); return; }
    ESP_LOGI(TAG, "HID connected: proto=%d subclass=%d", p.proto, p.sub_class);

    const hid_host_device_config_t cfg = { .callback = hid_iface_cb, .callback_arg = NULL };
    err = hid_host_device_open(hdev, &cfg);
    if (err != ESP_OK) { ESP_LOGW(TAG, "device_open: %s", esp_err_to_name(err)); return; }

    if (p.sub_class == HID_SUBCLASS_BOOT_INTERFACE) {
        err = hid_class_request_set_protocol(hdev, HID_REPORT_PROTOCOL_BOOT);
        if (err != ESP_OK) ESP_LOGW(TAG, "set_protocol: %s (non-fatal)", esp_err_to_name(err));
        if (p.proto == HID_PROTOCOL_KEYBOARD) {
            err = hid_class_request_set_idle(hdev, 0, 0);
            if (err != ESP_OK) ESP_LOGW(TAG, "set_idle: %s (non-fatal)", esp_err_to_name(err));
            s_kbd_hdev = hdev;   // remember for LED forwarding
            c6_uart_kbd_connect();
        } else if (p.proto == HID_PROTOCOL_MOUSE) {
            c6_uart_mouse_connect();
        }
    }
    err = hid_host_device_start(hdev);
    if (err != ESP_OK) ESP_LOGW(TAG, "device_start: %s", esp_err_to_name(err));
}

static void usb_host_lib_task(void *arg) {
    const usb_host_config_t hcfg = { .skip_phy_setup = false, .intr_flags = ESP_INTR_FLAG_LEVEL1 };
    esp_err_t err = usb_host_install(&hcfg);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "usb_host_install failed: %s — halting host task", esp_err_to_name(err));
        vTaskDelete(NULL);
        return;
    }
    xTaskNotifyGive(arg);

    while (1) {
        uint32_t flags;
        usb_host_lib_handle_events(portMAX_DELAY, &flags);
        if (flags & USB_HOST_LIB_EVENT_FLAGS_NO_CLIENTS) usb_host_device_free_all();
    }
}

void hid_host_task_start(void) {
    TaskHandle_t self = xTaskGetCurrentTaskHandle();
    xTaskCreatePinnedToCore(usb_host_lib_task, "usb_host", 4096, self, 5, NULL, 0);
    ulTaskNotifyTake(pdTRUE, pdMS_TO_TICKS(2000));

    const hid_host_driver_config_t dcfg = {
        .create_background_task = true,
        .task_priority = 5,
        .stack_size = 4096,
        .core_id = 0,
        .callback = hid_device_cb,
        .callback_arg = NULL,
    };
    esp_err_t err = hid_host_install(&dcfg);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "hid_host_install failed: %s", esp_err_to_name(err));
        return;
    }
    ESP_LOGI(TAG, "USB host HID installed on HS port. Plug keyboard into U2/U3 (U4 switch = Host).");
}

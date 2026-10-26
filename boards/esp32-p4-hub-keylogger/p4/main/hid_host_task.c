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
#include "led.h"

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

// Set to true whenever the HID host stack fires a CONNECTED event. The
// port-flap watchdog uses this to decide whether to poke the hub.
static volatile bool s_hid_ever_connected = false;

static void port_flap_watchdog(void *arg);
static volatile bool s_watchdog_running = false;

// Spawn a fresh watchdog task. Safe to call from any context — no-ops if a
// watchdog is already running. Uses a boolean instead of eTaskGetState on a
// deleted handle (which is undefined behaviour in FreeRTOS).
static void spawn_watchdog(void) {
    if (s_watchdog_running) return;
    s_watchdog_running = true;
    xTaskCreatePinnedToCore(port_flap_watchdog, "port_flap", 3072, NULL, 3,
                            NULL, 0);
}

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
        // Accept both boot-subclass (declared) and "No Subclass" — many
        // full-featured keyboards enumerate as sub_class 0 and we've already
        // forced them into BOOT protocol via set_protocol on connect.
        if (p.proto == HID_PROTOCOL_KEYBOARD) {
            publish_report(HID_REP_KIND_KEYBOARD, buf, data_len);
            c6_uart_feed_kbd_report(buf, data_len);
            // Blink the status LED on any keyboard event (both press and
            // release fire input reports).
            led_pulse_key();
        } else if (p.proto == HID_PROTOCOL_MOUSE) {
            publish_report(HID_REP_KIND_MOUSE, buf, data_len);
            c6_uart_feed_mouse_report(buf, data_len);
        }
        break;
    case HID_HOST_INTERFACE_EVENT_DISCONNECTED:
        ESP_LOGI(TAG, "HID iface disconnected");
        // Always clear the LED-forward pointer if this was the keyboard,
        // regardless of whether get_params succeeds — the device is going
        // away no matter what.
        if (s_kbd_hdev == hdev) {
            s_kbd_hdev = NULL;
            c6_uart_kbd_disconnect();
        } else {
            // No stale kbd handle -> it might have been a mouse. Best effort
            // to notify the pill.
            hid_host_dev_params_t pp;
            if (hid_host_device_get_params(hdev, &pp) == ESP_OK &&
                pp.proto == HID_PROTOCOL_MOUSE) c6_uart_mouse_disconnect();
        }
        err = hid_host_device_close(hdev);
        if (err != ESP_OK) ESP_LOGW(TAG, "device_close: %s", esp_err_to_name(err));
        // Session-long watchdog: any disconnect resets the flag so if the
        // user unplugs and re-plugs into a wonky port later, the port-flap
        // watchdog gets another chance to rescue enumeration.
        s_hid_ever_connected = false;
        spawn_watchdog();      // restart the retry loop for the rest of the session
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
    s_hid_ever_connected = true;   // watchdog: kbd has attached at least once

    hid_host_dev_params_t p;
    esp_err_t err = hid_host_device_get_params(hdev, &p);
    if (err != ESP_OK) { ESP_LOGW(TAG, "connect get_params: %s", esp_err_to_name(err)); return; }
    ESP_LOGI(TAG, "HID connected: proto=%d subclass=%d", p.proto, p.sub_class);

    const hid_host_device_config_t cfg = { .callback = hid_iface_cb, .callback_arg = NULL };
    err = hid_host_device_open(hdev, &cfg);
    if (err != ESP_OK) { ESP_LOGW(TAG, "device_open: %s", esp_err_to_name(err)); return; }

    // Try to switch into boot protocol regardless of the descriptor's
    // sub_class — many keyboards declare sub_class 0 but still respond to
    // SET_PROTOCOL(BOOT) with the 8-byte report layout we already parse.
    err = hid_class_request_set_protocol(hdev, HID_REPORT_PROTOCOL_BOOT);
    if (err != ESP_OK) ESP_LOGW(TAG, "set_protocol: %s (non-fatal)", esp_err_to_name(err));
    if (p.proto == HID_PROTOCOL_KEYBOARD) {
        err = hid_class_request_set_idle(hdev, 0, 0);
        if (err != ESP_OK) ESP_LOGW(TAG, "set_idle: %s (non-fatal)", esp_err_to_name(err));
        s_kbd_hdev = hdev;
        c6_uart_kbd_connect();
    } else if (p.proto == HID_PROTOCOL_MOUSE) {
        c6_uart_mouse_connect();
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

// Port-flap workaround. On cold boot the CH334 hub's port sometimes ends up
// stuck after the first (HS) enumeration attempt fails, so the FS-Force patch
// never gets a chance to retry at FS speed. We kick the hub every few seconds
// until at least one HID device attaches by calling usb_host_device_free_all()
// (which closes the hub and forces a fresh enumeration).
// Emit a status frame the C6 web UI can render so the user knows why the
// KBD pill is still red during boot enumeration.
extern void c6_uart_send_evt(const char *body);

// Full reset path: close any lingering handles, free every device, then
// uninstall + reinstall the HID host driver. This is what gets a truly stuck
// CH334 downstream port back — free_all alone often returns NOT_FINISHED
// when the HID host client holds a stale handle. Called from the watchdog
// when free_all keeps failing.
static void hid_stack_full_reset(void) {
    ESP_LOGW(TAG, "hid stack: close+uninstall -> reinstall");
    // 1) Close any handle we're still tracking so uninstall isn't blocked.
    if (s_kbd_hdev) {
        hid_host_device_close(s_kbd_hdev);
        s_kbd_hdev = NULL;
    }
    // 2) Tear the driver down.
    esp_err_t err = hid_host_uninstall();
    if (err != ESP_OK) ESP_LOGW(TAG, "hid_host_uninstall: %s", esp_err_to_name(err));
    vTaskDelay(pdMS_TO_TICKS(200));
    // 3) Now free_all can succeed because no client holds refs.
    err = usb_host_device_free_all();
    if (err != ESP_OK && err != ESP_ERR_NOT_FINISHED)
        ESP_LOGW(TAG, "post-uninstall free_all: %s", esp_err_to_name(err));
    vTaskDelay(pdMS_TO_TICKS(200));
    // 4) Reinstall — hub_root_task will re-enumerate everything from scratch.
    const hid_host_driver_config_t dcfg = {
        .create_background_task = true,
        .task_priority = 5,
        .stack_size = 4096,
        .core_id = 0,
        .callback = hid_device_cb,
        .callback_arg = NULL,
    };
    err = hid_host_install(&dcfg);
    if (err != ESP_OK) ESP_LOGE(TAG, "hid_host_install (reinstall): %s", esp_err_to_name(err));
}

static void port_flap_watchdog(void *arg) {
    (void)arg;
    // Fast first attempt (500 ms) so if the hub is healthy we just discover
    // the keyboard; if it's stuck we start recovery immediately.
    vTaskDelay(pdMS_TO_TICKS(500));
    int free_all_stuck_count = 0;
    for (int attempt = 0; attempt < 12 && !s_hid_ever_connected; attempt++) {
        char body[48];
        snprintf(body, sizeof(body), "ENUM_RETRY %d 12", attempt + 1);
        c6_uart_send_evt(body);
        ESP_LOGW(TAG, "no HID attach; poking hub (%d/12)", attempt + 1);

        // Race guard: recheck s_hid_ever_connected right before we free_all;
        // if a connect fired between the loop condition and here, don't kick
        // the just-attached keyboard.
        if (s_hid_ever_connected) break;

        esp_err_t err = usb_host_device_free_all();
        if (err == ESP_OK) {
            free_all_stuck_count = 0;
            ESP_LOGI(TAG, "free_all OK — hub will re-enumerate");
        } else if (err == ESP_ERR_NOT_FINISHED) {
            free_all_stuck_count++;
            ESP_LOGW(TAG, "free_all NOT_FINISHED (stuck refs, streak=%d)", free_all_stuck_count);
            // After two consecutive stuck cycles, do a full hid_host reset.
            if (free_all_stuck_count >= 2) {
                hid_stack_full_reset();
                free_all_stuck_count = 0;
            }
        } else {
            ESP_LOGW(TAG, "free_all err: %s", esp_err_to_name(err));
        }

        // Backoff: 500 ms first, then 1s, 1.5s, ..., cap ~3s.
        int delay_ms = 500 + attempt * 300;
        if (delay_ms > 3000) delay_ms = 3000;
        vTaskDelay(pdMS_TO_TICKS(delay_ms));
    }
    if (s_hid_ever_connected) {
        ESP_LOGI(TAG, "port-flap watchdog: HID attached");
        c6_uart_send_evt("ENUM_OK");
    } else {
        ESP_LOGW(TAG, "port-flap watchdog gave up");
        c6_uart_send_evt("ENUM_FAIL");
    }
    s_watchdog_running = false;
    vTaskDelete(NULL);
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

    spawn_watchdog();
}

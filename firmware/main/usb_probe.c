// USB PROBE — Full-Speed OTG host test
//
// Goal: figure out empirically whether any Type-C on the Spotpear
// ESP32-P4-MINI-B-M-EXT carries the P4's *Full-Speed* OTG lines (bypassing
// the CH334 hub, which the HS-OTG stack cannot address due to the ESP-IDF
// "no Transaction Translator" limitation).
//
// Strategy: after boot we announce ourselves on USB-Serial-JTAG (COM34) so
// you see the plan, then we tear USJ down and hand the internal FS PHY over
// to the OTG controller in HOST mode. From then on the log is unreachable
// (USJ is gone), so we drive the onboard WS2812 RGB LED (GPIO10) instead:
//
//   YELLOW  = FS host running, no device attached
//   GREEN   = a USB device is enumerated (any type)
//   CYAN    = an HID class device is attached
//   BLUE flash = HID input report received (a key was pressed)
//   RED     = init error
//
// Plug the keyboard into each Type-C in turn. The one that lights the LED
// green/cyan is the port that carries FS-OTG.

#include <string.h>
#include "esp_log.h"
#include "esp_err.h"
#include "esp_check.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"

#include "usb/usb_host.h"
#include "usb/hid_host.h"
#include "usb/hid_usage_keyboard.h"
#include "usb/hid_usage_mouse.h"

#include "driver/rmt_tx.h"
#include "driver/rmt_encoder.h"

static const char *TAG = "fsprobe";

// ------------------------- WS2812 LED helpers -------------------------
#define WS2812_GPIO   10
#define RMT_RES_HZ    10000000  // 10 MHz -> 100 ns resolution

static rmt_channel_handle_t rmt_ch;
static rmt_encoder_handle_t rmt_enc;

typedef struct { uint8_t g, r, b; } rgb_t;

// Use MUCH more distinctive colors — pure primaries with no overlap
static rgb_t COL_OFF    = { 0,   0,   0   };
static rgb_t COL_RED    = { 0,   96,  0   };   // pure red
static rgb_t COL_WAIT   = { 0,   0,   0   };   // OFF — nothing plugged in
static rgb_t COL_GREEN  = { 96,  0,   0   };   // pure bright green — any device
static rgb_t COL_PURPLE = { 0,   64,  96  };   // pure purple — HID device
static rgb_t COL_WHITE  = { 60,  60,  60  };   // flash on key press

static void led_set(rgb_t c) {
    uint8_t bytes[3] = { c.g, c.r, c.b };
    rmt_transmit_config_t tcfg = { .loop_count = 0 };
    rmt_transmit(rmt_ch, rmt_enc, bytes, sizeof(bytes), &tcfg);
    rmt_tx_wait_all_done(rmt_ch, portMAX_DELAY);
}

static esp_err_t led_init(void) {
    rmt_tx_channel_config_t chc = {
        .clk_src         = RMT_CLK_SRC_DEFAULT,
        .gpio_num        = WS2812_GPIO,
        .mem_block_symbols = 64,
        .resolution_hz   = RMT_RES_HZ,
        .trans_queue_depth = 4,
    };
    ESP_RETURN_ON_ERROR(rmt_new_tx_channel(&chc, &rmt_ch), TAG, "rmt_ch");
    rmt_bytes_encoder_config_t ec = {
        .bit0 = { .level0 = 1, .duration0 = 3, .level1 = 0, .duration1 = 9 },  // 0-code
        .bit1 = { .level0 = 1, .duration0 = 9, .level1 = 0, .duration1 = 3 },  // 1-code
        .flags.msb_first = 1,
    };
    ESP_RETURN_ON_ERROR(rmt_new_bytes_encoder(&ec, &rmt_enc), TAG, "rmt_enc");
    ESP_RETURN_ON_ERROR(rmt_enable(rmt_ch), TAG, "rmt_enable");
    led_set(COL_OFF);
    return ESP_OK;
}

// ------------------------- USB host + HID -------------------------
static volatile bool s_any_dev = false;
static volatile bool s_hid_dev = false;

static void hid_iface_cb(hid_host_device_handle_t hdev,
                         const hid_host_interface_event_t event, void *arg) {
    uint8_t buf[64] = {0};
    size_t data_len = 0;
    hid_host_dev_params_t p;
    hid_host_device_get_params(hdev, &p);
    if (event == HID_HOST_INTERFACE_EVENT_INPUT_REPORT) {
        hid_host_device_get_raw_input_report_data(hdev, buf, sizeof(buf), &data_len);
        printf("[FS-HID/%s] len=%d :", p.proto == HID_PROTOCOL_KEYBOARD ? "KB" :
                                       p.proto == HID_PROTOCOL_MOUSE    ? "MS" : "??", (int)data_len);
        for (size_t i = 0; i < data_len; i++) printf(" %02x", buf[i]);
        putchar('\n');
        led_set(COL_WHITE);
        vTaskDelay(pdMS_TO_TICKS(50));
        led_set(COL_PURPLE);
    } else if (event == HID_HOST_INTERFACE_EVENT_DISCONNECTED) {
        s_hid_dev = false;
        hid_host_device_close(hdev);
        led_set(s_any_dev ? COL_GREEN : COL_WAIT);
    }
}

static void hid_device_cb(hid_host_device_handle_t hdev,
                          const hid_host_driver_event_t event, void *arg) {
    hid_host_dev_params_t p;
    hid_host_device_get_params(hdev, &p);
    if (event == HID_HOST_DRIVER_EVENT_CONNECTED) {
        s_hid_dev = true;
        printf("\n*** [FS] HID CONNECTED: addr=%d iface=%d proto=%d subclass=%d ***\n\n",
               p.addr, p.iface_num, p.proto, p.sub_class);
        const hid_host_device_config_t cfg = { .callback = hid_iface_cb, .callback_arg = NULL };
        hid_host_device_open(hdev, &cfg);
        if (p.sub_class == HID_SUBCLASS_BOOT_INTERFACE) {
            hid_class_request_set_protocol(hdev, HID_REPORT_PROTOCOL_BOOT);
            if (p.proto == HID_PROTOCOL_KEYBOARD) {
                hid_class_request_set_idle(hdev, 0, 0);
            }
        }
        hid_host_device_start(hdev);
        led_set(COL_PURPLE);
    }
}

static void client_event_cb(const usb_host_client_event_msg_t *msg, void *arg) {
    if (msg->event == USB_HOST_CLIENT_EVENT_NEW_DEV) {
        s_any_dev = true;
        printf("\n*** [FS] NEW DEVICE at addr=%d ***\n\n", msg->new_dev.address);
        if (!s_hid_dev) led_set(COL_GREEN);
    } else if (msg->event == USB_HOST_CLIENT_EVENT_DEV_GONE) {
        s_any_dev = false;
        s_hid_dev = false;
        printf("\n*** [FS] DEVICE GONE ***\n\n");
        led_set(COL_WAIT);
    }
}

static void client_task(void *arg) {
    usb_host_client_handle_t client;
    const usb_host_client_config_t ccfg = {
        .is_synchronous = false,
        .max_num_event_msg = 8,
        .async = { .client_event_callback = client_event_cb, .callback_arg = NULL }
    };
    usb_host_client_register(&ccfg, &client);
    while (1) usb_host_client_handle_events(client, portMAX_DELAY);
}

static void usb_lib_task(void *arg) {
    while (1) {
        uint32_t flags;
        usb_host_lib_handle_events(portMAX_DELAY, &flags);
        if (flags & USB_HOST_LIB_EVENT_FLAGS_NO_CLIENTS) usb_host_device_free_all();
    }
}

// ------------------------- App -------------------------

static esp_err_t start_fs_otg_host(void) {
    // With the peter-marcisovsky FSLSSupp patch applied to the HAL, the HS-OTG
    // root port now negotiates at FS/LS speed with an external HS hub — no TT
    // needed. Keyboards behind the CH334 should enumerate straight through the
    // default HS peripheral (index 0).
    const usb_host_config_t hc = {
        .skip_phy_setup = false,
        .intr_flags     = ESP_INTR_FLAG_LEVEL1,
        // peripheral_map = 0 => use default = HS-OTG (index 0)
    };
    ESP_RETURN_ON_ERROR(usb_host_install(&hc), TAG, "usb_host_install");

    xTaskCreatePinnedToCore(usb_lib_task, "usb_lib",   4096, NULL, 5, NULL, 0);
    xTaskCreatePinnedToCore(client_task,  "usb_client",4096, NULL, 5, NULL, 0);

    const hid_host_driver_config_t hd = {
        .create_background_task = true,
        .task_priority = 5,
        .stack_size    = 4096,
        .core_id       = 0,
        .callback      = hid_device_cb,
        .callback_arg  = NULL,
    };
    ESP_RETURN_ON_ERROR(hid_host_install(&hd), TAG, "hid_host_install");
    return ESP_OK;
}

void app_main(void) {
    if (led_init() != ESP_OK) {
        // Can't signal anything, just spin.
        while (1) vTaskDelay(1000);
    }

    // Startup blink — 3 quick reds so you see it booted.
    for (int i = 0; i < 3; ++i) { led_set(COL_RED); vTaskDelay(pdMS_TO_TICKS(150));
                                  led_set(COL_OFF); vTaskDelay(pdMS_TO_TICKS(150)); }

    ESP_LOGW(TAG, "===============================================================");
    ESP_LOGW(TAG, " ESP32-P4 FS-OTG host probe");
    ESP_LOGW(TAG, " Taking over FS PHY -> USB-Serial-JTAG will DIE, COM34 vanishes.");
    ESP_LOGW(TAG, " LED codes:");
    ESP_LOGW(TAG, "   YELLOW = FS host up, waiting for a device");
    ESP_LOGW(TAG, "   GREEN  = any device enumerated");
    ESP_LOGW(TAG, "   CYAN   = HID device attached");
    ESP_LOGW(TAG, "   BLUE flash = HID input report (key press)");
    ESP_LOGW(TAG, "   RED    = init failed");
    ESP_LOGW(TAG, " Plug keyboard into each Type-C in turn. Whichever port makes");
    ESP_LOGW(TAG, " the LED go green/cyan carries the FS-OTG lines.");
    ESP_LOGW(TAG, "===============================================================");

    // Grace period so the message reaches USJ before we tear it down.
    for (int s = 5; s > 0; --s) {
        ESP_LOGW(TAG, " starting FS host in %d s ...", s);
        vTaskDelay(pdMS_TO_TICKS(1000));
    }

    esp_err_t err = start_fs_otg_host();
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "FS host init FAILED: %s", esp_err_to_name(err));
        while (1) {
            led_set(COL_RED);  vTaskDelay(pdMS_TO_TICKS(200));
            led_set(COL_OFF);  vTaskDelay(pdMS_TO_TICKS(200));
        }
    }

    // Success — LED starts OFF (no color) so you can visually tell "no device".
    led_set(COL_WAIT);
    ESP_LOGW(TAG, " OK: FS-OTG host running. Plug something into each Type-C. Log will announce every event.");

    uint32_t t = 0;
    while (1) {
        vTaskDelay(pdMS_TO_TICKS(2000));
        t += 2;
        usb_host_lib_info_t info = {0};
        usb_host_lib_info(&info);
        printf("[FS] heartbeat t=%us  num_devices=%d  any=%d  hid=%d\n",
               (unsigned)t, info.num_devices, s_any_dev, s_hid_dev);
    }
}

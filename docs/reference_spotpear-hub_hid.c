/*
 * SPDX-FileCopyrightText: 2026 Espressif Systems (Shanghai) CO LTD
 *
 * SPDX-License-Identifier: Apache-2.0
 *
 * HUB HID driver, built on the CherryUSB host stack.
 *
 * CherryUSB rather than the ESP-IDF USB Host stack because the external-hub
 * driver in ESP-IDF v5.5 does not implement Transaction Translator (TT): a
 * low-speed keyboard or mouse behind a high-speed hub fails enumeration with
 * "TT not implemented". CherryUSB's DWC2 driver implements split transactions
 * (HCSPLT), so low-speed devices work through a hub.
 *
 * This driver claims the USB-OTG controller directly via usbh_initialize().
 * bsp_usb_host_start() and usb_host_install() must therefore NOT be called as
 * well; they would contend for the same peripheral.
 */
#include <string.h>
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "freertos/queue.h"
#include "esp_log.h"
#include "esp_timer.h"
#include "esp_heap_caps.h"
#include "bsp/display.h"
#include "usbh_core.h"
#include "usbh_hid.h"
#include "hub_hid.h"

static const char *TAG = "HUB_HID";

/* Mouse coordinate range, i.e. the panel resolution chosen in menuconfig. */
#define HUB_MOUSE_MAX_X     BSP_LCD_H_RES
#define HUB_MOUSE_MAX_Y     BSP_LCD_V_RES

/* Keyboard event queue depth */
#define HUB_KBD_QUEUE_LEN   32
/* Raw HID report queue depth (ISR -> worker task) */
#define HUB_MSG_QUEUE_LEN   10

/* Boot keyboard report: byte0 = modifiers, byte1 = reserved, byte2..7 = up to 6 keycodes */
#define HID_BOOT_KBD_KEYS   6
/* Keycodes <= 0x03 are ErrorRollOver/POSTFail/ErrorUndefined, not real keys */
#define HID_KEY_ERR_UNDEF   0x03

static bool s_initialized = false;

/* Connection state, read by the UI */
static hub_hid_status_t s_status = {0};

/* Mouse state, with accumulated coordinates */
static hub_hid_mouse_data_t s_mouse = {0};
static volatile bool s_mouse_updated = false;

/* Keyboard event queue exposed to callers */
static QueueHandle_t s_kbd_queue = NULL;

/* Raw HID report, forwarded from the URB completion callback (ISR) to the worker task */
typedef struct {
    uint8_t  protocol;          /* HID_PROTOCOL_KEYBOARD / HID_PROTOCOL_MOUSE / 0 */
    uint16_t len;
    uint8_t  data[64];
} hub_hid_msg_t;

static QueueHandle_t s_msg_queue = NULL;
static TaskHandle_t  s_msg_task  = NULL;

/* Interrupt-IN polling context, one per HID interface */
typedef struct {
    bool                is_active;  /* An URB is in flight */
    esp_timer_handle_t  timer;      /* Submits an URB periodically */
    uint8_t            *buffer;     /* DMA buffer */
    uint8_t             protocol;   /* HID protocol of this interface */
} hub_hid_intin_t;

/*------------------------------------------------------------
 * Report parsing: raw boot-protocol bytes into the exposed queue and state
 *----------------------------------------------------------*/
static inline bool key_found(const uint8_t *src, uint8_t key, unsigned int length)
{
    for (unsigned int i = 0; i < length; i++) {
        if (src[i] == key) {
            return true;
        }
    }
    return false;
}

static void handle_keyboard_report(const uint8_t *data, int length)
{
    if (length < 8) {   /* A boot keyboard report is always 8 bytes */
        return;
    }
    const uint8_t modifier = data[0];
    const uint8_t *keys = &data[2];

    static uint8_t prev_keys[HID_BOOT_KBD_KEYS] = {0};

    for (int i = 0; i < HID_BOOT_KBD_KEYS; i++) {
        /* Detect releases */
        if (prev_keys[i] > HID_KEY_ERR_UNDEF &&
            !key_found(keys, prev_keys[i], HID_BOOT_KBD_KEYS)) {
            hub_hid_keyboard_event_t ev = {
                .key_code = prev_keys[i],
                .modifier = 0,
                .pressed  = false,
            };
            if (s_kbd_queue) {
                xQueueSend(s_kbd_queue, &ev, 0);
            }
        }
        /* Detect presses */
        if (keys[i] > HID_KEY_ERR_UNDEF &&
            !key_found(prev_keys, keys[i], HID_BOOT_KBD_KEYS)) {
            hub_hid_keyboard_event_t ev = {
                .key_code = keys[i],
                .modifier = modifier,
                .pressed  = true,
            };
            if (s_kbd_queue) {
                xQueueSend(s_kbd_queue, &ev, 0);
            }
        }
    }
    memcpy(prev_keys, keys, HID_BOOT_KBD_KEYS);
}

static void handle_mouse_report(const uint8_t *data, int length)
{
    if (length < 3) {   /* Boot mouse report: buttons, x, y */
        return;
    }
    const uint8_t buttons = data[0];
    const int8_t  dx = (int8_t)data[1];
    const int8_t  dy = (int8_t)data[2];

    int nx = s_mouse.x + dx;
    int ny = s_mouse.y + dy;
    if (nx < 0) nx = 0;
    if (nx >= HUB_MOUSE_MAX_X) nx = HUB_MOUSE_MAX_X - 1;
    if (ny < 0) ny = 0;
    if (ny >= HUB_MOUSE_MAX_Y) ny = HUB_MOUSE_MAX_Y - 1;

    s_mouse.x = nx;
    s_mouse.y = ny;
    s_mouse.left_button  = (buttons & 0x01) != 0;
    s_mouse.right_button = (buttons & 0x02) != 0;
    s_mouse_updated = true;
}

/*------------------------------------------------------------
 * Report dispatch task
 *----------------------------------------------------------*/
static void hub_hid_msg_task(void *arg)
{
    (void)arg;
    hub_hid_msg_t msg;
    while (true) {
        if (xQueueReceive(s_msg_queue, &msg, portMAX_DELAY) != pdTRUE) {
            continue;
        }
        if (msg.protocol == HID_PROTOCOL_KEYBOARD) {
            handle_keyboard_report(msg.data, msg.len);
        } else if (msg.protocol == HID_PROTOCOL_MOUSE) {
            handle_mouse_report(msg.data, msg.len);
        }
    }
}

/*------------------------------------------------------------
 * CherryUSB interrupt-IN polling: URB completion callback, ISR context
 *----------------------------------------------------------*/
static void hub_hid_urb_complete(void *arg, int nbytes)
{
    struct usbh_hid *hid_class = (struct usbh_hid *)arg;
    hub_hid_intin_t *intin = (hub_hid_intin_t *)hid_class->user_data;
    if (intin == NULL) {
        return;
    }
    if (nbytes <= 0) {          /* Error or disconnect: stop this round */
        intin->is_active = false;
        return;
    }
    if (s_msg_queue && nbytes <= (int)sizeof(((hub_hid_msg_t *)0)->data)) {
        hub_hid_msg_t msg;
        msg.protocol = intin->protocol;
        msg.len = (uint16_t)nbytes;
        memcpy(msg.data, intin->buffer, nbytes);
        BaseType_t hpw = pdFALSE;
        xQueueSendFromISR(s_msg_queue, &msg, &hpw);
        intin->is_active = false;
        if (hpw == pdTRUE) {
            portYIELD_FROM_ISR();
        }
        return;
    }
    intin->is_active = false;
}

/* Timer callback: submits one interrupt-IN URB to fetch a report. */
static void hub_hid_poll_timer_cb(void *arg)
{
    struct usbh_hid *hid_class = (struct usbh_hid *)arg;
    hub_hid_intin_t *intin = (hub_hid_intin_t *)hid_class->user_data;
    if (intin == NULL || intin->is_active) {
        return;
    }
    usbh_int_urb_fill(&hid_class->intin_urb, hid_class->hport, hid_class->intin,
                      intin->buffer, hid_class->intin->wMaxPacketSize, 0,
                      hub_hid_urb_complete, hid_class);
    intin->is_active = true;
    int ret = usbh_submit_urb(&hid_class->intin_urb);
    if (ret != 0) {
        if (ret == -USB_ERR_NOTCONN) {
            esp_timer_stop(intin->timer);
            return;
        }
        intin->is_active = false;
    }
}

/*------------------------------------------------------------
 * CherryUSB HID class callbacks, overriding the weak symbols in usbh_hid.c.
 * Device attach -> usbh_hid_run, detach -> usbh_hid_stop.
 *----------------------------------------------------------*/
void usbh_hid_run(struct usbh_hid *hid_class)
{
    uint8_t sub_class = hid_class->hport->config.intf[hid_class->intf].altsetting[0].intf_desc.bInterfaceSubClass;
    uint8_t protocol  = hid_class->hport->config.intf[hid_class->intf].altsetting[0].intf_desc.bInterfaceProtocol;
    ESP_LOGI(TAG, "HID connected: intf=%u subclass=%u protocol=%u", hid_class->intf, sub_class, protocol);

    /* Boot-interface devices are switched to boot protocol for fixed-layout reports. */
    if (sub_class == HID_SUBCLASS_BOOTIF) {
        if (usbh_hid_set_protocol(hid_class, HID_PROTOCOL_BOOT) < 0) {
            ESP_LOGW(TAG, "set boot protocol failed");
        }
    }

    if (hid_class->intin == NULL) {
        ESP_LOGW(TAG, "no interrupt-in endpoint");
        return;
    }

    hub_hid_intin_t *intin = heap_caps_calloc(1, sizeof(hub_hid_intin_t),
                                              MALLOC_CAP_DEFAULT | MALLOC_CAP_INTERNAL);
    if (intin == NULL) {
        ESP_LOGE(TAG, "intin alloc failed");
        return;
    }
    intin->buffer = heap_caps_aligned_alloc(CONFIG_USB_ALIGN_SIZE,
                                            hid_class->intin->wMaxPacketSize,
                                            MALLOC_CAP_DMA | MALLOC_CAP_INTERNAL);
    if (intin->buffer == NULL) {
        ESP_LOGE(TAG, "intin buffer alloc failed");
        heap_caps_free(intin);
        return;
    }
    intin->protocol = (sub_class == HID_SUBCLASS_BOOTIF) ? protocol : 0;

    if (intin->protocol == HID_PROTOCOL_KEYBOARD) {
        s_status.keyboard_connected = true;
    } else if (intin->protocol == HID_PROTOCOL_MOUSE) {
        s_status.mouse_connected = true;
    }

    const esp_timer_create_args_t timer_cfg = {
        .callback = hub_hid_poll_timer_cb,
        .arg = hid_class,
#if CONFIG_ESP_TIMER_SUPPORTS_ISR_DISPATCH_METHOD
        .dispatch_method = ESP_TIMER_ISR,
#else
        .dispatch_method = ESP_TIMER_TASK,
#endif
        .name = "hub_hid_poll",
        .skip_unhandled_events = true,
    };
    if (esp_timer_create(&timer_cfg, &intin->timer) != ESP_OK) {
        ESP_LOGE(TAG, "poll timer create failed");
        heap_caps_free(intin->buffer);
        heap_caps_free(intin);
        return;
    }

    hid_class->user_data = intin;
    esp_timer_start_periodic(intin->timer,
                             USBH_GET_URB_INTERVAL(hid_class->intin->bInterval, hid_class->hport->speed));
}

void usbh_hid_stop(struct usbh_hid *hid_class)
{
    hub_hid_intin_t *intin = (hub_hid_intin_t *)hid_class->user_data;
    if (intin) {
        if (intin->protocol == HID_PROTOCOL_KEYBOARD) {
            s_status.keyboard_connected = false;
        } else if (intin->protocol == HID_PROTOCOL_MOUSE) {
            s_status.mouse_connected = false;
        }
        esp_timer_stop(intin->timer);
        esp_timer_delete(intin->timer);
        heap_caps_free(intin->buffer);
        heap_caps_free(intin);
        hid_class->user_data = NULL;
    }
    ESP_LOGW(TAG, "HID disconnected");
}

/* Referenced from outside so the linker keeps this translation unit, and with it
 * the strong usbh_hid_run/usbh_hid_stop symbols above. */
void hub_hid_force_link(void) {}

/*------------------------------------------------------------
 * Public API
 *----------------------------------------------------------*/
esp_err_t hub_hid_init(void)
{
    if (s_initialized) {
        return ESP_OK;
    }

    /* The queue and worker task are created once and reused across app entries,
     * so repeatedly opening and closing the app does not leak them. */
    if (s_kbd_queue == NULL) {
        s_kbd_queue = xQueueCreate(HUB_KBD_QUEUE_LEN, sizeof(hub_hid_keyboard_event_t));
        if (!s_kbd_queue) {
            return ESP_ERR_NO_MEM;
        }
    }
    if (s_msg_queue == NULL) {
        s_msg_queue = xQueueCreate(HUB_MSG_QUEUE_LEN, sizeof(hub_hid_msg_t));
        if (!s_msg_queue) {
            return ESP_ERR_NO_MEM;
        }
    }
    if (s_msg_task == NULL) {
        if (xTaskCreate(hub_hid_msg_task, "hub_hid_msg", 4096, NULL, 5, &s_msg_task) != pdTRUE) {
            s_msg_task = NULL;
            return ESP_ERR_NO_MEM;
        }
    }

    /* Hand the P4 high-speed root port to CherryUSB; the host starts only on app entry. */
    int ret = usbh_initialize(0, ESP_USB_HS0_BASE);
    if (ret != 0) {
        ESP_LOGE(TAG, "usbh_initialize failed: %d", ret);
        return ESP_FAIL;
    }

    s_initialized = true;
    ESP_LOGI(TAG, "HUB HID (CherryUSB) started");
    return ESP_OK;
}

esp_err_t hub_hid_deinit(void)
{
    if (!s_initialized) {
        return ESP_OK;
    }
    usbh_deinitialize(0);
    s_initialized = false;
    s_status.keyboard_connected = false;
    s_status.mouse_connected = false;
    return ESP_OK;
}

hub_hid_status_t hub_hid_get_status(void)
{
    return s_status;
}

bool hub_hid_get_mouse(hub_hid_mouse_data_t *data)
{
    if (!data || !s_mouse_updated) {
        return false;
    }
    *data = s_mouse;
    s_mouse_updated = false;
    return true;
}

bool hub_hid_get_keyboard_event(hub_hid_keyboard_event_t *event)
{
    if (!event || !s_kbd_queue) {
        return false;
    }
    return xQueueReceive(s_kbd_queue, event, 0) == pdTRUE;
}

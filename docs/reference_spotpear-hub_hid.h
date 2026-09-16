/*
 * SPDX-FileCopyrightText: 2026 Espressif Systems (Shanghai) CO LTD
 *
 * SPDX-License-Identifier: Apache-2.0
 */
#pragma once

#include <stdint.h>
#include <stdbool.h>
#include "esp_err.h"

#ifdef __cplusplus
extern "C" {
#endif

/*
 * Standard USB HID keyboard usage IDs and modifier bits (boot protocol).
 * ESP-IDF's "usb/hid_usage_keyboard.h" is not available alongside the CherryUSB
 * host stack, so the constants the UI keycode mapping needs are defined here.
 * Values match the USB HID specification.
 */
#ifndef HID_LEFT_SHIFT
#define HID_LEFT_SHIFT   (1 << 1)
#define HID_RIGHT_SHIFT  (1 << 5)
#endif

#ifndef HID_KEY_A
enum hub_hid_keycode {
    HID_KEY_A             = 0x04,
    HID_KEY_Z             = 0x1D,
    HID_KEY_1             = 0x1E,
    HID_KEY_9             = 0x26,
    HID_KEY_0             = 0x27,
    HID_KEY_ENTER         = 0x28,
    HID_KEY_DEL           = 0x2A,  /* Backspace */
    HID_KEY_TAB           = 0x2B,
    HID_KEY_SPACE         = 0x2C,
    HID_KEY_MINUS         = 0x2D,
    HID_KEY_EQUAL         = 0x2E,
    HID_KEY_OPEN_BRACKET  = 0x2F,
    HID_KEY_CLOSE_BRACKET = 0x30,
    HID_KEY_BACK_SLASH    = 0x31,
    HID_KEY_COLON         = 0x33,  /* ; : */
    HID_KEY_QUOTE         = 0x34,
    HID_KEY_TILDE         = 0x35,
    HID_KEY_LESS          = 0x36,  /* , < */
    HID_KEY_GREATER       = 0x37,  /* . > */
    HID_KEY_SLASH         = 0x38,
};
#endif

/**
 * @brief HID device type
 */
typedef enum {
    HUB_HID_DEVICE_NONE = 0,
    HUB_HID_DEVICE_KEYBOARD,
    HUB_HID_DEVICE_MOUSE,
} hub_hid_device_type_t;

/**
 * @brief HUB HID status, for display in the UI
 */
typedef struct {
    bool keyboard_connected;   /* A keyboard is attached */
    bool mouse_connected;      /* A mouse is attached */
} hub_hid_status_t;

/**
 * @brief Mouse event data: absolute position plus button state
 */
typedef struct {
    int x;                     /* Accumulated X position, in pixels */
    int y;                     /* Accumulated Y position, in pixels */
    bool left_button;          /* Left button held */
    bool right_button;         /* Right button held */
} hub_hid_mouse_data_t;

/**
 * @brief Keyboard event data
 */
typedef struct {
    uint8_t key_code;          /* HID keycode (HID_KEY_*) */
    uint8_t modifier;          /* Modifier state (HID_LEFT_SHIFT and friends) */
    bool pressed;              /* true = press, false = release */
} hub_hid_keyboard_event_t;

/**
 * @brief Initialise the HUB HID driver, starting the USB host and HID host
 *
 * Idempotent: calling it again while already initialised is a no-op.
 *
 * @return ESP_OK, or an error code
 */
esp_err_t hub_hid_init(void);

/**
 * @brief Deinitialise the HUB HID driver
 *
 * @return ESP_OK, or an error code
 */
esp_err_t hub_hid_deinit(void);

/**
 * @brief Read the current HID device connection status
 */
hub_hid_status_t hub_hid_get_status(void);

/**
 * @brief Read the latest mouse data. Non-blocking; clears the button-change flags.
 *
 * @param[out] data Mouse data
 * @return true if new data was available, false if nothing changed
 */
bool hub_hid_get_mouse(hub_hid_mouse_data_t *data);

/**
 * @brief Pop one event from the keyboard event queue. Non-blocking.
 *
 * @param[out] event Keyboard event
 * @return true if an event was dequeued, false if the queue was empty
 */
bool hub_hid_get_keyboard_event(hub_hid_keyboard_event_t *event);

#ifdef __cplusplus
}
#endif

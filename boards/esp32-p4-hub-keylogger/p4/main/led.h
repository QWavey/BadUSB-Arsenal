#pragma once
// Single-pixel RGB status LED driver for the WS2812B on the M-EXT board
// (P4 GPIO10, per the base-board schematic).
//
// Behavior:
//   Idle              -> steady green (dim)
//   Keystroke arrives -> quick red flash (~120 ms), then back to green
//   Explicitly off    -> LED entirely dark
//
// The mode is controlled by led_set_mode() so the C6 web UI can later push
// the setting over the SDIO reverse channel when we add one.

#include <stdint.h>

// IDs mirror c6/main/main.c LED_MODE_* -- must stay in sync.
typedef enum {
    LED_MODE_OFF        = 0,   // dark
    LED_MODE_GREEN_ONLY = 1,   // steady green, no flash
    LED_MODE_KEY_FLASH  = 2,   // green idle + red flash per key event  (default)
    LED_MODE_RED_ONLY   = 3,   // steady red
    LED_MODE_BLUE_ONLY  = 4,   // steady blue
} led_mode_t;

void led_init(void);
void led_set_mode(led_mode_t m);
led_mode_t led_get_mode(void);

// Call from any task when a HID keyboard event was received. Non-blocking.
void led_pulse_key(void);

#pragma once
// Shared types between the USB-host code and the TinyUSB-device code.
// Kept minimal so neither side needs to include the other's headers.

#include <stdint.h>
#include <stddef.h>
#include "freertos/FreeRTOS.h"
#include "freertos/queue.h"

typedef enum {
    HID_REP_KIND_KEYBOARD       = 1,
    HID_REP_KIND_MOUSE          = 2,
    HID_REP_KIND_KEYBOARD_RESET = 3,   // flush: send "all keys released" to the PC
} hid_rep_kind_t;

typedef struct {
    hid_rep_kind_t kind;
    uint8_t        len;
    uint8_t        data[16];  // boot keyboard = 8 bytes; boot mouse = 3-4 bytes
} hid_bridge_msg_t;

// Created in app_main before any producer runs.
extern QueueHandle_t g_hid_bridge_q;

// Starts the USB host + HID host stacks. Publishes reports on g_hid_bridge_q.
void hid_host_task_start(void);

// Push a keyboard LED-state byte back to the physical keyboard (bit0=NumLock,
// bit1=CapsLock, bit2=ScrollLock, bit3=Compose, bit4=Kana). No-op if no
// keyboard is currently connected. Safe to call from the tinyusb set-report
// callback (short, non-blocking). Always caches the value so the key logger
// on the C6 side can render Caps Lock correctly.
void hid_host_set_kbd_leds(uint8_t leds);
// Last LED bitmap Windows pushed at us (or 0 at boot). Read by c6_uart's
// letter renderer so Caps Lock upper-cases letters in the web-UI log.
uint8_t hid_host_kbd_leds(void);

#pragma once
// Shared types between the USB-host code and the TinyUSB-device code.
// Kept minimal so neither side needs to include the other's headers.

#include <stdint.h>
#include <stddef.h>
#include "freertos/FreeRTOS.h"
#include "freertos/queue.h"

typedef enum {
    HID_REP_KIND_KEYBOARD = 1,
    HID_REP_KIND_MOUSE    = 2,
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

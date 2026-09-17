#pragma once
// Line-based UART framing sent from the P4 to the C6 for the web UI.
// See c6-firmware/main/main.c uart_task() for the parser.
//
// Frames (each terminated by '\n'):
//   KEY <text>            printable characters (may include spaces, tabs)
//   EVT KBD_CONNECT
//   EVT KBD_DISCONNECT
//   EVT MOUSE_CONNECT
//   EVT MOUSE_DISCONNECT
//   RAW <hex bytes>       for reports we couldn't decode (rare)

#include <stdint.h>
#include <stddef.h>

// One-time init. Idempotent; safe to call from app_main after PHY swap.
void c6_uart_init(void);

// Feed one boot-keyboard input report (8 bytes: mod, rsv, key[6]).
// The decoder tracks which keys are new since last report and emits
// "KEY x\n" for each key that just went down. Modifier-only changes are
// currently silent (only decoded printable + a set of named specials).
void c6_uart_feed_kbd_report(const uint8_t *buf, size_t len);

// Feed one boot-mouse report — currently just used to gate MOUSE_CONNECT
// state so we don't have to emit anything per-motion.
void c6_uart_feed_mouse_report(const uint8_t *buf, size_t len);

// EVT emitters used by the HID host driver callbacks.
void c6_uart_kbd_connect(void);
void c6_uart_kbd_disconnect(void);
void c6_uart_mouse_connect(void);
void c6_uart_mouse_disconnect(void);

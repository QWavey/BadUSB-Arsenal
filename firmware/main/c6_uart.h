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
#include <stdbool.h>

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

// Send one "EVT <body>\n" SDIO frame (used by the port-flap watchdog and the
// PC-mount watcher so the web UI can show enumeration + mount state).
void c6_uart_send_evt(const char *body);

// Set the active keyboard layout ("US", "DE", "UK"). Called by the SDIO RX
// side of c6_uart.c when the C6 pushes a "SETLAYOUT <name>\n" packet.
void c6_uart_set_layout(const char *name);

// True once the SDIO link to the C6 is up and EVT/KEY frames actually leave
// the P4. pc_watch_task in main.c uses this to defer PC_CONNECT until the
// wire is live (so the "PC" pill isn't stuck red because the first mount
// edge fired before SDIO bring-up).
bool c6_uart_link_ready(void);

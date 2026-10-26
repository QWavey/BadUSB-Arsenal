#pragma once
// Optional keyboard layout tables for the P4-side HID decoder.
//
// The physical keyboard sends HID Usage codes (page 0x07), which represent
// physical key positions, not glyphs. On the PC the OS applies a layout to
// translate that position into a character (US, DE, FR, ...). The keylogger's
// decoder sees only the position, so if you type "y" on a German (QWERTZ)
// keyboard the raw usage is 0x1c ("Y" in the US table). Unless we know which
// layout the user has, the log reads "keyboard" as "kezboard".
//
// This file provides a tiny per-layout override table that the decoder
// applies before falling back to the default US layout in c6_uart.c.
//
// Currently supported:
//   KL_US        US ANSI (no overrides)
//   KL_DE        German QWERTZ (y/z swap, ä/ö/ü/ß, special punctuation)
//   KL_UK        UK QWERTY (# / @ / " swap, £ symbol)

#include <stdint.h>

typedef enum {
    KL_US = 0,
    KL_DE = 1,
    KL_UK = 2,
    KL_COUNT
} kb_layout_t;

// One per-HID-usage entry. Empty strings mean "layout has no override,
// use the US default". Multi-byte UTF-8 is fine (e.g. "ä" = 0xC3 0xA4).
typedef struct {
    const char *unshifted;   // 0-3 bytes (may be UTF-8), "" = fall through
    const char *shifted;     // shift held
    const char *altgr;       // right-alt (AltGr) held — layout-specific glyphs
} kb_layout_entry_t;

// Look up an override for (layout, kc). Returns NULL if the layout has no
// entry for this key (caller then uses the default US table).
const kb_layout_entry_t *kb_layout_lookup(kb_layout_t layout, uint8_t kc);

// Set / get the current layout. Layout is not persisted: it starts as DE on
// every boot (s_layout in kb_layouts.c). Nothing sets it at runtime yet.
void          kb_layout_set(kb_layout_t layout);
kb_layout_t   kb_layout_get(void);
const char   *kb_layout_name(kb_layout_t layout);

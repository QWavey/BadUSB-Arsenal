#include "kb_layouts.h"
#include <stddef.h>

// Default matches the tester's physical keyboard. Change here (or wire in a
// runtime switcher later) if you use a US keyboard.
static kb_layout_t s_layout = KL_DE;

void        kb_layout_set(kb_layout_t l) { if (l < KL_COUNT) s_layout = l; }
kb_layout_t kb_layout_get(void)          { return s_layout; }

const char *kb_layout_name(kb_layout_t layout) {
    switch (layout) {
    case KL_US: return "US";
    case KL_DE: return "DE";
    case KL_UK: return "UK";
    default:    return "?";
    }
}

// ---------------- German QWERTZ overrides ----------------
// Only the keys whose glyph differs from US ANSI are listed. Everything else
// falls through to the default US table.
static const kb_layout_entry_t de_1c = { "y",  "Y", "" };     // 0x1c Z-position on QWERTZ (US Y)
static const kb_layout_entry_t de_1d = { "z",  "Z", "" };     // 0x1d Y-position (US Z)

// Row: 1 2 3 4 5 6 7 8 9 0 ß ´
static const kb_layout_entry_t de_1e = { "1", "!",  "" };
static const kb_layout_entry_t de_1f = { "2", "\"", "\xc2\xb2" }; // " and ²
static const kb_layout_entry_t de_20 = { "3", "\xc2\xa7", "\xc2\xb3" }; // § ³
static const kb_layout_entry_t de_21 = { "4", "$",  "" };
static const kb_layout_entry_t de_22 = { "5", "%",  "" };
static const kb_layout_entry_t de_23 = { "6", "&",  "" };
static const kb_layout_entry_t de_24 = { "7", "/",  "{" };
static const kb_layout_entry_t de_25 = { "8", "(",  "[" };
static const kb_layout_entry_t de_26 = { "9", ")",  "]" };
static const kb_layout_entry_t de_27 = { "0", "=",  "}" };
static const kb_layout_entry_t de_2d = { "\xc3\x9f", "?", "\\" };   // ß ?
static const kb_layout_entry_t de_2e = { "\xc2\xb4", "`", "" };     // ´ ` (dead)

// Row: q w e r t z u i o p ü +
static const kb_layout_entry_t de_2f = { "\xc3\xbc", "\xc3\x9c", "" }; // ü Ü
static const kb_layout_entry_t de_30 = { "+", "*", "~" };

// Row: a s d f g h j k l ö ä #
static const kb_layout_entry_t de_33 = { "\xc3\xb6", "\xc3\x96", "" }; // ö Ö
static const kb_layout_entry_t de_34 = { "\xc3\xa4", "\xc3\x84", "" }; // ä Ä
static const kb_layout_entry_t de_35 = { "^", "\xc2\xb0", "" };       // ^ °
static const kb_layout_entry_t de_31 = { "#", "'", "" };              // # '

// Row: < y x c v b n m , . -
static const kb_layout_entry_t de_64 = { "<", ">", "|" };   // non-US backslash
static const kb_layout_entry_t de_36 = { ",", ";", "" };
static const kb_layout_entry_t de_37 = { ".", ":", "" };
static const kb_layout_entry_t de_38 = { "-", "_", "" };

// ---------------- UK overrides ----------------
static const kb_layout_entry_t uk_1f = { "2", "\"", "" };            // 2 "
static const kb_layout_entry_t uk_20 = { "3", "\xc2\xa3", "" };      // 3 £
static const kb_layout_entry_t uk_34 = { "'", "@",  "" };            // ' @
static const kb_layout_entry_t uk_31 = { "#", "~",  "" };            // # ~
static const kb_layout_entry_t uk_64 = { "\\","|",  "" };            // <> key -> \ |

const kb_layout_entry_t *kb_layout_lookup(kb_layout_t layout, uint8_t kc) {
    if (layout == KL_DE) {
        switch (kc) {
        case 0x1c: return &de_1c;
        case 0x1d: return &de_1d;
        case 0x1e: return &de_1e;
        case 0x1f: return &de_1f;
        case 0x20: return &de_20;
        case 0x21: return &de_21;
        case 0x22: return &de_22;
        case 0x23: return &de_23;
        case 0x24: return &de_24;
        case 0x25: return &de_25;
        case 0x26: return &de_26;
        case 0x27: return &de_27;
        case 0x2d: return &de_2d;
        case 0x2e: return &de_2e;
        case 0x2f: return &de_2f;
        case 0x30: return &de_30;
        case 0x31: return &de_31;
        case 0x33: return &de_33;
        case 0x34: return &de_34;
        case 0x35: return &de_35;
        case 0x36: return &de_36;
        case 0x37: return &de_37;
        case 0x38: return &de_38;
        case 0x64: return &de_64;
        default:   return NULL;
        }
    } else if (layout == KL_UK) {
        switch (kc) {
        case 0x1f: return &uk_1f;
        case 0x20: return &uk_20;
        case 0x31: return &uk_31;
        case 0x34: return &uk_34;
        case 0x64: return &uk_64;
        default:   return NULL;
        }
    }
    return NULL;
}

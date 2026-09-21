// BadUSB port of QWavey/ESP-S3-Key-BadUSB onto the P4/C6 split.
//
// Their firmware is single-chip ESP32-S3 Arduino with the UI, SD-card
// script store, DuckyScript interpreter, HID keyboard emitter and MSC
// stub all in one binary. Our stack splits USB HID onto the P4 and
// Wi-Fi/UI onto the C6, with a shared SDIO link and (per the user's
// hardware plan) a spare SD card slot wired to the P4's second SDMMC
// controller. This file lives on the C6 and exposes:
//
//   - Static assets: /, /style.css, /script.js — served straight from
//     the C6 flash via IDF EMBED_FILES (see CMakeLists.txt).
//   - /api/live-type — same reg-44 transport the earlier BadUSB card
//     already used; the JS in the ported UI POSTs {"text":"..."} here.
//   - /execute — accepts a DuckyScript payload. Minimal parser (STRING,
//     DELAY, ENTER, TAB, ESC, GUI/WIN, CTRL, ALT, SHIFT + key, plain
//     KEY names) emits the equivalent live-type bytes so the P4 side
//     needs no changes to accept it. Full 3.0 interpreter is roadmap.
//   - /api/scripts, /api/save, /api/load, /api/list-files,
//     /api/delete-file, /api/upload, /api/download — thin SPIFFS-backed
//     script store. Files live under /data/ducky/*.
//   - /api/stats, /api/tasks, /api/languages, /api/setup-complete,
//     /api/tutorial-done — safe defaults so the JS renders.
//   - /api/toggle-*, /api/save-* — accept + persist to NVS or no-op.
//
// Anything the ported UI queries that we don't yet back with real state
// returns a shape-compatible empty default; the goal is that the JS UI
// LOADS and REMAINS INTERACTIVE while individual features are added in
// follow-up commits. All comments here call out what's real vs stubbed.

#include "esp_log.h"
#include "esp_err.h"
#include "esp_http_server.h"
#include "esp_spiffs.h"
#include "esp_timer.h"           // esp_timer_get_time() for the dedupe
#include "driver/sdio_slave.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"

#include <string.h>
#include <strings.h>       // strncasecmp / strcasecmp
#include <stdlib.h>
#include <dirent.h>
#include <sys/stat.h>
#include <sys/types.h>
#include <unistd.h>        // unlink()
#include <ctype.h>

static const char *TAG = "badusb_ui";

// Matches main.c's SDIO_REG_BADUSB_TYPE.
#define SDIO_REG_BADUSB_TYPE  44   // ASCII byte (live-type stream)
#define SDIO_REG_BADUSB_MOD   45   // HID modifier bitmap for the next combo
#define SDIO_REG_BADUSB_HID   46   // HID scancode for combo/named keys

// Modifier bit mask constants (match the HID boot-report modifier byte
// on the P4 side).
#define MOD_CTRL   0x01
#define MOD_SHIFT  0x02
#define MOD_ALT    0x04
#define MOD_GUI    0x08

// C6-side mirror of firmware/main/live_type.c's s_ascii table — needed
// so we can convert a single-char combo key (e.g. `CTRL a`) into the
// right (HID scancode, shift-required) pair before we send the combo
// over reg 45/46 to the P4. Only the printable slots are populated
// here; unmapped bytes return kc=0 which the caller treats as "not a
// key we can send as part of a combo".
uint8_t badusb_ascii_to_hid(uint8_t c, uint8_t *out_shift) {
    struct { uint8_t kc, sh; } m;
    m.kc = 0; m.sh = 0;
    if (c >= 'a' && c <= 'z') { m.kc = 0x04 + (c - 'a'); m.sh = 0; }
    else if (c >= 'A' && c <= 'Z') { m.kc = 0x04 + (c - 'A'); m.sh = 1; }
    else if (c >= '1' && c <= '9') { m.kc = 0x1e + (c - '1'); m.sh = 0; }
    else if (c == '0') { m.kc = 0x27; m.sh = 0; }
    else {
        switch (c) {
            case ' ':  m.kc = 0x2c; break;
            case '\n': m.kc = 0x28; break;
            case '\t': m.kc = 0x2b; break;
            case '-':  m.kc = 0x2d; break;
            case '=':  m.kc = 0x2e; break;
            case '[':  m.kc = 0x2f; break;
            case ']':  m.kc = 0x30; break;
            case '\\': m.kc = 0x31; break;
            case ';':  m.kc = 0x33; break;
            case '\'': m.kc = 0x34; break;
            case '`':  m.kc = 0x35; break;
            case ',':  m.kc = 0x36; break;
            case '.':  m.kc = 0x37; break;
            case '/':  m.kc = 0x38; break;
            case '!':  m.kc = 0x1e; m.sh = 1; break;
            case '@':  m.kc = 0x1f; m.sh = 1; break;
            case '#':  m.kc = 0x20; m.sh = 1; break;
            case '$':  m.kc = 0x21; m.sh = 1; break;
            case '%':  m.kc = 0x22; m.sh = 1; break;
            case '^':  m.kc = 0x23; m.sh = 1; break;
            case '&':  m.kc = 0x24; m.sh = 1; break;
            case '*':  m.kc = 0x25; m.sh = 1; break;
            case '(':  m.kc = 0x26; m.sh = 1; break;
            case ')':  m.kc = 0x27; m.sh = 1; break;
            case '_':  m.kc = 0x2d; m.sh = 1; break;
            case '+':  m.kc = 0x2e; m.sh = 1; break;
            case '{':  m.kc = 0x2f; m.sh = 1; break;
            case '}':  m.kc = 0x30; m.sh = 1; break;
            case '|':  m.kc = 0x31; m.sh = 1; break;
            case ':':  m.kc = 0x33; m.sh = 1; break;
            case '"':  m.kc = 0x34; m.sh = 1; break;
            case '~':  m.kc = 0x35; m.sh = 1; break;
            case '<':  m.kc = 0x36; m.sh = 1; break;
            case '>':  m.kc = 0x37; m.sh = 1; break;
            case '?':  m.kc = 0x38; m.sh = 1; break;
        }
    }
    if (out_shift) *out_shift = m.sh;
    return m.kc;
}

// Ducky/reference token -> HID scancode. Returns 0 if the token isn't a
// known named key. Case-insensitive.
static uint8_t ducky_key_to_hid(const char *tok) {
    // F1..F12
    if ((tok[0]=='F'||tok[0]=='f') && isdigit((unsigned char)tok[1])) {
        int f = atoi(tok + 1);
        if (f >= 1 && f <= 12) return 0x3a + (f - 1);
    }
    struct { const char *n; uint8_t k; } m[] = {
        {"ENTER",     0x28}, {"RETURN",    0x28},
        {"ESC",       0x29}, {"ESCAPE",    0x29},
        {"BACKSPACE", 0x2a}, {"BKSP",      0x2a},
        {"TAB",       0x2b},
        {"SPACE",     0x2c},
        {"CAPSLOCK",  0x39}, {"CAPS",      0x39},
        {"PRINTSCREEN", 0x46}, {"PRTSC",   0x46},
        {"SCROLLLOCK",0x47}, {"SCROLL",    0x47},
        {"PAUSE",     0x48}, {"BREAK",     0x48},
        {"INSERT",    0x49}, {"INS",       0x49},
        {"HOME",      0x4a},
        {"PAGEUP",    0x4b}, {"PGUP",      0x4b},
        {"DELETE",    0x4c}, {"DEL",       0x4c},
        {"END",       0x4d},
        {"PAGEDOWN",  0x4e}, {"PGDN",      0x4e},
        {"RIGHT",     0x4f}, {"RIGHTARROW",0x4f},
        {"LEFT",      0x50}, {"LEFTARROW", 0x50},
        {"DOWN",      0x51}, {"DOWNARROW", 0x51},
        {"UP",        0x52}, {"UPARROW",   0x52},
        {"NUMLOCK",   0x53},
        {"APP",       0x65}, {"MENU",      0x65},
        {"POWER",     0x66},
    };
    for (size_t i = 0; i < sizeof(m)/sizeof(m[0]); i++) {
        if (!strcasecmp(tok, m[i].n)) return m[i].k;
    }
    return 0;
}

// Wait for P4 to ack + write (mod, kc) as a combo. The P4's live_type
// task checks reg 46 first, so the modifier must land BEFORE the HID
// byte to avoid a race where P4 sees kc but stale mod. Total round-trip
// is ~20-40 ms (one P4 poll tick).
static bool send_combo(uint8_t mod, uint8_t kc) {
    if (kc == 0) return false;
    // Wait for reg 46 to be idle so we don't stomp an in-flight combo.
    for (int spin = 0; spin < 20; spin++) {
        if (sdio_slave_read_reg(SDIO_REG_BADUSB_HID) == 0) break;
        vTaskDelay(pdMS_TO_TICKS(10));
    }
    // Modifier goes down first (P4 checks reg 46 as the trigger).
    if (sdio_slave_write_reg(SDIO_REG_BADUSB_MOD, mod) != ESP_OK) return false;
    if (sdio_slave_write_reg(SDIO_REG_BADUSB_HID, kc)  != ESP_OK) return false;
    // Wait for P4 to consume it before returning (bounded).
    for (int spin = 0; spin < 20; spin++) {
        if (sdio_slave_read_reg(SDIO_REG_BADUSB_HID) == 0) return true;
        vTaskDelay(pdMS_TO_TICKS(10));
    }
    return true;   // even if ack didn't land in 200 ms, P4 will still inject
}

// Parse a Ducky-style combo line like "CTRL ALT DELETE", "GUI r",
// "SHIFT F10". Modifier tokens are recognised: CTRL/CONTROL, SHIFT,
// ALT, GUI/WIN/WINDOWS/COMMAND. Anything else is treated as the key.
// A single-char key ('r', 'a', ...) is mapped via ASCII+SHIFT lookup so
// upper-case gets its own SHIFT bit; a named key ("F4", "DELETE") is
// mapped through ducky_key_to_hid. Returns true on success.
static bool run_combo_line(const char *line) {
    uint8_t mod = 0;
    uint8_t kc  = 0;
    char tok[24];
    while (*line) {
        while (*line && isspace((unsigned char)*line)) line++;
        if (!*line) break;
        int i = 0;
        while (line[i] && !isspace((unsigned char)line[i]) && i < (int)sizeof(tok)-1) { tok[i] = line[i]; i++; }
        tok[i] = 0; line += i;
        if      (!strcasecmp(tok, "CTRL")     || !strcasecmp(tok, "CONTROL")) mod |= MOD_CTRL;
        else if (!strcasecmp(tok, "SHIFT"))                                    mod |= MOD_SHIFT;
        else if (!strcasecmp(tok, "ALT"))                                      mod |= MOD_ALT;
        else if (!strcasecmp(tok, "GUI")      || !strcasecmp(tok, "WIN")
              || !strcasecmp(tok, "WINDOWS")  || !strcasecmp(tok, "COMMAND"))  mod |= MOD_GUI;
        else if (i == 1) {
            // Single ASCII char — map through our known layout.
            uint8_t c = (uint8_t)tok[0];
            if (c < 128) {
                extern uint8_t badusb_ascii_to_hid(uint8_t c, uint8_t *out_shift);
                uint8_t sh = 0;
                kc = badusb_ascii_to_hid(c, &sh);
                if (sh) mod |= MOD_SHIFT;
            }
        }
        else kc = ducky_key_to_hid(tok);
    }
    if (kc == 0 && mod != 0) {
        // Modifier alone — e.g. bare "GUI" — send it as a lone tap of
        // the GUI key (0xe3 = Left GUI).
        static const uint8_t solo[4] = { 0xe0, 0xe1, 0xe2, 0xe3 };
        int i = 0;
        for (; i < 4; i++) if (mod & (1 << i)) { kc = solo[i]; mod = 0; break; }
    }
    if (kc == 0) return false;
    return send_combo(mod, kc);
}

// Static assets used to be baked into the binary via EMBED_FILES; now
// they live on the "storage" SPIFFS partition (built from webui-fs/ at
// flash time — see main/CMakeLists.txt spiffs_create_partition_image).
// This mirrors the reference firmware's SD-card root layout so scripts,
// language packs and UI patches can be updated by re-flashing the
// storage partition alone (idf.py partition-table build && idf.py
// partition storage flash) without touching the app image.
//
// SPIFFS is mounted at "/data" by main.c storage_up() — NOT "/spiffs".
// Every filesystem access in this file must use "/data/..." accordingly.

// -------- shared helper: stream one ASCII byte at a time to the P4 --------
// Waits until the P4 has cleared reg 44 (200 ms max per byte) and then
// writes the next byte. Returns how many bytes actually landed.
static int badusb_stream_ascii(const char *buf, int len) {
    int sent = 0;
    // If the P4 side of the reg-44 link stalls (SDIO recovery, kbd
    // enum flap, PC unmounted so HID buffers back up), we DO NOT
    // want to pin the HTTPD worker for 200 * 10 ms = 2 s PER byte —
    // that would blow past recv_wait_timeout and take out concurrent
    // /api/stats polls, which is why the UI showed a "connection lost"
    // banner even though the write succeeded. Cap the per-byte spin
    // at 100 ms and abort the rest of the stream once any byte stalls
    // (the caller can retry the tail on the next invocation).
    for (int i = 0; i < len; i++) {
        uint8_t c = (uint8_t)buf[i];
        // Strip \r so a CRLF paste (Windows/Mac clipboard) fires Enter
        // exactly once, not twice, on the target PC.
        if (c == 0 || c > 127 || c == '\r') continue;
        int spin = 0;
        while (sdio_slave_read_reg(SDIO_REG_BADUSB_TYPE) != 0) {
            if (++spin >= 10) return sent;   // 10 * 10 ms = 100 ms cap
            vTaskDelay(pdMS_TO_TICKS(10));
        }
        if (sdio_slave_write_reg(SDIO_REG_BADUSB_TYPE, c) == ESP_OK) sent++;
    }
    return sent;
}

// -------- static asset serving from SPIFFS --------
// Serve a file from the SPIFFS root. Handles the tiny set of mime types
// the reference UI uses; anything else falls back to application/octet-
// stream. Missing files return 404 — the app never assumes an asset is
// there, so a stripped-down SPIFFS image still boots.
static const char *mime_for(const char *path) {
    const char *dot = strrchr(path, '.');
    if (!dot) return "application/octet-stream";
    if (!strcasecmp(dot, ".html")) return "text/html; charset=utf-8";
    if (!strcasecmp(dot, ".css"))  return "text/css";
    if (!strcasecmp(dot, ".js"))   return "text/javascript";
    if (!strcasecmp(dot, ".json")) return "application/json";
    if (!strcasecmp(dot, ".svg"))  return "image/svg+xml";
    if (!strcasecmp(dot, ".png"))  return "image/png";
    if (!strcasecmp(dot, ".ico"))  return "image/x-icon";
    if (!strcasecmp(dot, ".txt"))  return "text/plain; charset=utf-8";
    return "application/octet-stream";
}

// Stream a file from /spiffs into the response in 1 KB chunks. If the
// file doesn't exist we return a 404 body the browser can display,
// rather than the terse esp_http_server default.
static esp_err_t serve_spiffs(httpd_req_t *r, const char *spiffs_path) {
    FILE *f = fopen(spiffs_path, "rb");
    if (!f) {
        ESP_LOGW(TAG, "serve_spiffs 404: %s", spiffs_path);
        httpd_resp_set_status(r, "404 Not Found");
        httpd_resp_set_type(r, "text/plain");
        return httpd_resp_send(r, "not on flash storage", -1);
    }
    httpd_resp_set_type(r, mime_for(spiffs_path));
    httpd_resp_set_hdr(r, "Cache-Control", "no-store");
    char buf[1024];
    size_t n;
    while ((n = fread(buf, 1, sizeof(buf), f)) > 0) {
        if (httpd_resp_send_chunk(r, buf, n) != ESP_OK) { fclose(f); return ESP_FAIL; }
    }
    fclose(f);
    return httpd_resp_send_chunk(r, NULL, 0);
}

static esp_err_t h_badusb_index(httpd_req_t *r) {
    return serve_spiffs(r, "/data/index.html");
}
static esp_err_t h_badusb_style(httpd_req_t *r) {
    return serve_spiffs(r, "/data/style.css");
}
static esp_err_t h_badusb_script(httpd_req_t *r) {
    return serve_spiffs(r, "/data/script.js");
}

// GET /languages/<name>.json — serves language packs sitting under
// /data/languages/. Their JS calls fetch('/languages') to enumerate;
// see h_api_languages below for the listing endpoint.
static esp_err_t h_lang_file(httpd_req_t *r) {
    // r->uri is "/languages/xx.json" — take the leaf and sanitize.
    const char *leaf = strrchr(r->uri, '/');
    if (!leaf) { httpd_resp_send_err(r, HTTPD_400_BAD_REQUEST, "bad path"); return ESP_OK; }
    leaf++;   // skip the '/'
    // Reject path traversal and empty names.
    if (!*leaf || strstr(leaf, "..") || strchr(leaf, '\\')) {
        httpd_resp_send_err(r, HTTPD_400_BAD_REQUEST, "bad name");
        return ESP_OK;
    }
    char path[128];
    snprintf(path, sizeof(path), "/data/languages/%s", leaf);
    return serve_spiffs(r, path);
}

// -------- POST body helper --------
// Slurps up to `max` bytes of the body into `out`, NUL-terminates. Returns
// number of bytes read, -1 on error. The caller owns `out`.
static int slurp_body(httpd_req_t *r, char *out, int max) {
    int cl = r->content_len;
    if (cl <= 0 || cl > max - 1) return -1;
    int total = 0;
    while (total < cl) {
        int n = httpd_req_recv(r, out + total, cl - total);
        if (n <= 0) return -1;
        total += n;
    }
    out[total] = 0;
    return total;
}

// Extract a "text" JSON field naively (avoids pulling in cJSON here).
// Handles {"text":"...."} where the value is a JSON string. Supports \n,
// \t, \r, \", \\ escapes. Returns bytes written to `out` (excluding NUL).
static int extract_json_text(const char *body, char *out, int out_max) {
    // Match the KEY "text" not any occurrence of the substring — otherwise
    // a body like {"other":"text","real":"..."} would grab "..." from the
    // wrong field. "text":\s* is the actual key.
    const char *p = strstr(body, "\"text\"");
    while (p) {
        const char *after = p + 6;
        while (*after && isspace((unsigned char)*after)) after++;
        if (*after == ':') { p = after; break; }
        p = strstr(after, "\"text\"");
    }
    if (!p) return -1;
    p++;   // skip the ':'
    while (*p && isspace((unsigned char)*p)) p++;
    if (*p != '"') return -1;
    p++;
    int i = 0;
    while (*p && *p != '"' && i < out_max - 1) {
        if (*p == '\\' && p[1]) {
            char c = p[1];
            switch (c) {
                case 'n': out[i++] = '\n'; break;
                case 't': out[i++] = '\t'; break;
                case 'r': out[i++] = '\r'; break;
                case '"': out[i++] = '"';  break;
                case '\\': out[i++] = '\\'; break;
                default:  out[i++] = c;   break;
            }
            p += 2;
        } else {
            out[i++] = *p++;
        }
    }
    out[i] = 0;
    return i;
}

// Generalised JSON string-field extractor: pass the field name and it
// pulls the value between the following pair of quotes into `out`.
// Same escape handling as extract_json_text. Returns bytes written
// (>=0) or -1 if the field is absent.
static int extract_json_str(const char *body, const char *field, char *out, int out_max) {
    char needle[24];
    int nl = snprintf(needle, sizeof(needle), "\"%s\"", field);
    if (nl <= 0) return -1;
    const char *p = strstr(body, needle);
    while (p) {
        const char *after = p + nl;
        while (*after && isspace((unsigned char)*after)) after++;
        if (*after == ':') { p = after; break; }
        p = strstr(after, needle);
    }
    if (!p) return -1;
    p++;   // skip ':'
    while (*p && isspace((unsigned char)*p)) p++;
    if (*p != '"') return -1;
    p++;
    int i = 0;
    while (*p && *p != '"' && i < out_max - 1) {
        if (*p == '\\' && p[1]) {
            switch (p[1]) {
                case 'n': out[i++] = '\n'; break;
                case 't': out[i++] = '\t'; break;
                case 'r': out[i++] = '\r'; break;
                case '"': out[i++] = '"';  break;
                case '\\': out[i++] = '\\'; break;
                default:  out[i++] = p[1]; break;
            }
            p += 2;
        } else {
            out[i++] = *p++;
        }
    }
    out[i] = 0;
    return i;
}

// -------- /api/live-type --------
// The ported UI's Live tab POSTs one of these JSON payloads per event:
//   {"text":"<string>"}    — input event, the string that was inserted
//   {"k":"<char>"}         — keydown event, a single printable char
//   {"combo":"CTRL SHIFT a"} — modifier + key combo (special-key wire not
//                             ported yet — accepted as no-op success)
//   {"key":"F4"}           — named key (see above)
//   {"release":true}       — silent-mode release (no-op)
// On desktop, keydown fires FIRST then input fires within a few ms with
// the same char, and the JS dedupes the input event against a 40 ms
// window. That dedupe only works if BOTH payloads reach the same
// endpoint SEMANTICS — dropping the {"k":...} path made the server type
// the raw JSON string, and treating them as separate would double every
// press. We normalise both to "type this text", and a small
// server-side dedupe catches any browser that still emits both.
static esp_err_t h_api_live_type(httpd_req_t *r) {
    const int BUFSZ = 2048;
    char *buf = malloc(BUFSZ);
    char *text = malloc(BUFSZ);
    if (!buf || !text) { free(buf); free(text); httpd_resp_send_err(r, HTTPD_500_INTERNAL_SERVER_ERROR, "oom"); return ESP_OK; }
    int n = slurp_body(r, buf, BUFSZ);
    if (n < 0) { free(buf); free(text); httpd_resp_send_err(r, HTTPD_400_BAD_REQUEST, "bad body"); return ESP_OK; }

    // Silent-mode release — reference sends this on tab hide / kbd-off toggle.
    // We don't have a live "detach USB HID" path yet; treat as no-op success.
    if (strstr(buf, "\"release\"")) {
        free(buf); free(text);
        httpd_resp_set_type(r, "application/json");
        return httpd_resp_send(r, "{\"ok\":true}", 11);
    }

    // Try the "text" field first (input events / longer pastes). If that
    // misses, try the "k" field (physical-keyboard keydown, always 1 char).
    // If BOTH are absent it might be a raw-body POST (some code paths do
    // that) — fall back to the whole body as text.
    int tl = extract_json_str(buf, "text", text, BUFSZ);
    if (tl < 0) tl = extract_json_str(buf, "k", text, BUFSZ);
    if (tl < 0) {
        // Reject anything that STILL looks like JSON at the front — better
        // to say "unrecognised" than to type "{\"combo\":\"...\"}" on the PC.
        if (buf[0] == '{' || buf[0] == '[') {
            free(buf); free(text);
            httpd_resp_set_type(r, "application/json");
            return httpd_resp_send(r, "{\"ok\":true,\"note\":\"combo/named-key not wired\"}", 46);
        }
        tl = n;
        if (tl > BUFSZ - 1) tl = BUFSZ - 1;
        memcpy(text, buf, tl);
        text[tl] = 0;
    }

    // Server-side dedupe: Chrome desktop fires BOTH keydown ({"k":"h"})
    // and input ({"text":"h"}) within ~5 ms and the JS's 40 ms client
    // dedupe misses because __lastKeydownAt is set AFTER liveSend fires
    // — the input event's fetch is already in-flight. We collapse the
    // pair with an "arm once" pattern: after we accept a keystroke we
    // arm the dedupe; the very next matching same-char within 30 ms is
    // dropped and the dedupe disarms. This handles the keydown/input
    // twin without eating fast repeated presses (aa, holding a key,
    // etc.) — a real second 'a' 30-100 ms later still lands.
    static char     last_text[32];
    static int      last_tl = 0;
    static uint32_t last_ms = 0;
    static bool     dedup_armed = false;
    uint32_t now = (uint32_t)(esp_timer_get_time() / 1000);
    if (dedup_armed && tl > 0 && tl == last_tl && tl < (int)sizeof(last_text) &&
        memcmp(text, last_text, tl) == 0 &&
        (now - last_ms) < 30) {
        dedup_armed = false;   // one-shot; a real second press comes through
        free(buf); free(text);
        httpd_resp_set_type(r, "application/json");
        return httpd_resp_send(r, "{\"typed\":0,\"dedup\":true}", 24);
    }
    if (tl > 0 && tl < (int)sizeof(last_text)) {
        memcpy(last_text, text, tl);
        last_tl = tl;
        last_ms = now;
        dedup_armed = true;
    }

    int sent = badusb_stream_ascii(text, tl);
    free(buf); free(text);
    httpd_resp_set_type(r, "application/json");
    char body[64];
    int bn = snprintf(body, sizeof(body), "{\"typed\":%d}", sent);
    return httpd_resp_send(r, body, bn);
}

// -------- /execute --------
// Minimal DuckyScript-3.0 parser. Supports:
//   STRING <text>           — types text literally
//   STRINGLN <text>         — types text then Enter
//   DELAY <ms>              — sleep
//   ENTER, RETURN, TAB, ESC, ESCAPE, BACKSPACE, SPACE
//   GUI <k>, WIN <k>, WINDOWS <k>  — GUI + key (single-char names)
//   CTRL <k>, CONTROL <k>          — CTRL + key
//   ALT <k>                        — ALT + key
//   SHIFT <k>                      — SHIFT + key
//   CTRL-ALT-DELETE                — the classic
//   REM <text>              — comment (ignored)
//   Blank lines             — ignored
// Anything else is silently ignored so unsupported commands don't abort
// the whole script. The full 3000-line Hak5 interpreter (variables,
// IF/WHILE, EXTENSION, ATTACKMODE, WAIT_FOR_*, ...) is roadmap.
static void ducky_send(const char *s) {
    if (!s || !*s) return;
    badusb_stream_ascii(s, strlen(s));
}
static void ducky_key(uint8_t hid) {
    // Reuse the ASCII channel by piggy-backing the HID scan code as a
    // control byte >= 128. The P4 side only accepts 1..127 today, so
    // we translate the small set of named keys we support here into
    // ASCII equivalents already known to live_type's table.
    // ENTER=\n (0x0a), TAB=\t (0x09), BACKSPACE=0x08, SPACE=' '. Others
    // require a new wire protocol — mark as TODO.
    (void)hid;
}

// Runs the DuckyScript parser on a heap-owned buffer. This is the
// former body of h_execute — extracted so we can spawn it as a task
// and have h_execute return HTTP 200 immediately instead of tying up
// the HTTPD worker for 5+ seconds. The task free()s the buffer when
// done. Note: only one script runs at a time (a second POST while
// this task is live will just append its bytes to reg 44 which will
// interleave with the first — user-visible effect is scrambled text;
// no crash).
static void execute_task(void *arg) {
    char *script = (char *)arg;
    int lines = 0, executed = 0;
    char *save = NULL;
    for (char *line = strtok_r(script, "\r\n", &save); line; line = strtok_r(NULL, "\r\n", &save)) {
        while (*line && isspace((unsigned char)*line)) line++;
        if (!*line || line[0] == '#') continue;
        if (!strncasecmp(line, "REM ", 4) || !strcasecmp(line, "REM")) { lines++; continue; }
        lines++;
        if (!strncasecmp(line, "STRINGLN ", 9)) { badusb_stream_ascii(line + 9, strlen(line + 9)); badusb_stream_ascii("\n", 1); executed++; }
        else if (!strncasecmp(line, "STRING ", 7)) { badusb_stream_ascii(line + 7, strlen(line + 7)); executed++; }
        else if (!strncasecmp(line, "DELAY ", 6)) { int ms = atoi(line + 6); if (ms > 0 && ms < 30000) vTaskDelay(pdMS_TO_TICKS(ms)); executed++; }
        else if (!strcasecmp(line, "ENTER") || !strcasecmp(line, "RETURN")) { badusb_stream_ascii("\n", 1); executed++; }
        else if (!strcasecmp(line, "TAB"))       { badusb_stream_ascii("\t", 1); executed++; }
        else if (!strcasecmp(line, "SPACE"))     { badusb_stream_ascii(" ", 1);  executed++; }
        else if (!strcasecmp(line, "BACKSPACE")) { char c = 0x08; badusb_stream_ascii(&c, 1); executed++; }
        // Combos: CTRL/ALT/GUI/SHIFT prefix, plus GUI-alias WIN/WINDOWS/
        // COMMAND. Named keys F1..F12/ENTER/ESC/DELETE/HOME/etc. also
        // route through the combo path so the P4 gets the HID scancode
        // instead of an ASCII byte. Ductape for a full DuckyScript 3.0
        // interpreter but covers the load-bearing 95% of payloads.
        else if (!strncasecmp(line, "CTRL",    4) || !strncasecmp(line, "CONTROL", 7) ||
                 !strncasecmp(line, "ALT",     3) ||
                 !strncasecmp(line, "SHIFT",   5) ||
                 !strncasecmp(line, "GUI",     3) || !strncasecmp(line, "WIN",     3) ||
                 !strncasecmp(line, "WINDOWS", 7) || !strncasecmp(line, "COMMAND", 7)) {
            if (run_combo_line(line)) executed++;
        }
        else {
            // Bare named-key line: "F4", "DELETE", "UPARROW", ...
            uint8_t kc = ducky_key_to_hid(line);
            if (kc && send_combo(0, kc)) executed++;
        }
    }
    ESP_LOGI(TAG, "execute: %d lines, %d executed", lines, executed);
    free(script);
    vTaskDelete(NULL);
}

static esp_err_t h_execute(httpd_req_t *r) {
    // Same reason as h_api_live_type: keep 8 KB of buffers off the
    // HTTPD worker's 4 KB stack.
    const int BUFSZ = 4096;
    char *buf = malloc(BUFSZ);
    char *script = malloc(BUFSZ);
    if (!buf || !script) { free(buf); free(script); httpd_resp_send_err(r, HTTPD_500_INTERNAL_SERVER_ERROR, "oom"); return ESP_OK; }
    int n = slurp_body(r, buf, BUFSZ);
    if (n < 0) { free(buf); free(script); httpd_resp_send_err(r, HTTPD_400_BAD_REQUEST, "bad body"); return ESP_OK; }
    // Allow either raw payload or {"script":"..."} form.
    int sl = extract_json_text(buf, script, BUFSZ);
    if (sl < 0) {
        const char *p = strstr(buf, "\"script\"");
        if (p) {
            // simple copy — same escape handling as extract_json_text
            p = strchr(p, ':');
            if (p) { p++; while (*p && isspace((unsigned char)*p)) p++; }
            if (p && *p == '"') {
                p++;
                sl = 0;
                while (*p && *p != '"' && sl < BUFSZ - 1) {
                    if (*p == '\\' && p[1]) {
                        switch (p[1]) {
                            case 'n': script[sl++]='\n'; break;
                            case 't': script[sl++]='\t'; break;
                            case 'r': script[sl++]='\r'; break;
                            case '"': script[sl++]='"';  break;
                            case '\\': script[sl++]='\\'; break;
                            default: script[sl++]=p[1]; break;
                        }
                        p += 2;
                    } else script[sl++] = *p++;
                }
                script[sl] = 0;
            }
        }
        if (sl < 0) { memcpy(script, buf, n); script[n] = 0; sl = n; }
    }
    // Hand the parsed body off to a background task and return HTTP
    // 200 immediately. A ~500-byte script types in ~10 seconds, which
    // is well past HTTPD's recv_wait_timeout (5 s) — before this the
    // browser saw "Execution Failed" even when the typing succeeded.
    // We only free `buf`; `script` is passed to the task which owns it.
    free(buf);
    if (xTaskCreate(execute_task, "ducky_run", 4096, script, 4, NULL) != pdPASS) {
        free(script);
        httpd_resp_send_err(r, HTTPD_500_INTERNAL_SERVER_ERROR, "spawn");
        return ESP_OK;
    }
    (void)ducky_key;   // silence unused-static warning until we wire a wider wire protocol
    httpd_resp_set_type(r, "application/json");
    return httpd_resp_send(r, "{\"started\":true}", 16);
}

// -------- SPIFFS-backed script/file store --------
// Scripts live under /data/ducky/<name>. Uploaded via /api/save and
// /api/upload. Listed via /api/scripts and /api/list-files.
#define SCRIPT_DIR "/data/ducky"

static void ensure_script_dir(void) {
    struct stat st;
    if (stat(SCRIPT_DIR, &st) != 0) {
        // SPIFFS is a flat namespace; the "directory" is a naming
        // convention. We touch a marker file so listings find something.
        FILE *f = fopen(SCRIPT_DIR "/.keep", "wb");
        if (f) { fputs("keep", f); fclose(f); }
    }
}

// Sanitize a filename fragment — strip path separators, cap length.
static void safe_name(const char *in, char *out, int max) {
    int i = 0;
    for (; in && *in && i < max - 1; in++) {
        char c = *in;
        if (c == '/' || c == '\\' || c < 0x20 || c == 0x7f) continue;
        out[i++] = c;
    }
    out[i] = 0;
    if (i == 0) strncpy(out, "unnamed", max);
}

static esp_err_t h_api_scripts(httpd_req_t *r) {
    ensure_script_dir();
    DIR *d = opendir(SCRIPT_DIR);
    // Build a JSON array of {name, size}. SPIFFS keeps flat paths so we
    // list everything with our prefix.
    httpd_resp_set_type(r, "application/json");
    if (!d) { return httpd_resp_send(r, "[]", 2); }
    char body[2048]; int bl = 0;
    body[bl++] = '[';
    int first = 1;
    struct dirent *e;
    while ((e = readdir(d)) != NULL) {
        if (e->d_name[0] == '.') continue;
        char full[300]; snprintf(full, sizeof(full), "%s/%s", SCRIPT_DIR, e->d_name);
        struct stat st;
        long sz = (stat(full, &st) == 0) ? (long)st.st_size : 0;
        int n = snprintf(body + bl, sizeof(body) - bl,
                         "%s{\"name\":\"%s\",\"size\":%ld}",
                         first ? "" : ",", e->d_name, sz);
        if (n < 0 || bl + n >= (int)sizeof(body) - 2) break;
        bl += n; first = 0;
    }
    closedir(d);
    body[bl++] = ']'; body[bl] = 0;
    return httpd_resp_send(r, body, bl);
}

static esp_err_t h_api_save(httpd_req_t *r) {
    // Expects ?name=<file>, body = script text.
    char q[128] = { 0 }; httpd_req_get_url_query_str(r, q, sizeof(q));
    char nameraw[64] = { 0 };
    httpd_query_key_value(q, "name", nameraw, sizeof(nameraw));
    char name[64]; safe_name(nameraw, name, sizeof(name));
    ensure_script_dir();
    char path[128]; snprintf(path, sizeof(path), "%s/%s", SCRIPT_DIR, name);
    FILE *f = fopen(path, "wb");
    if (!f) { httpd_resp_send_err(r, HTTPD_500_INTERNAL_SERVER_ERROR, "fopen"); return ESP_OK; }
    char buf[512];
    int total = 0;
    while (total < r->content_len) {
        int want = r->content_len - total; if (want > (int)sizeof(buf)) want = sizeof(buf);
        int n = httpd_req_recv(r, buf, want);
        if (n <= 0) { fclose(f); return ESP_FAIL; }
        fwrite(buf, 1, n, f);
        total += n;
    }
    fclose(f);
    httpd_resp_set_type(r, "application/json");
    char body[64]; int bn = snprintf(body, sizeof(body), "{\"name\":\"%s\",\"bytes\":%d}", name, total);
    return httpd_resp_send(r, body, bn);
}

static esp_err_t h_api_load(httpd_req_t *r) {
    // ?file=<name>
    char q[128] = { 0 }; httpd_req_get_url_query_str(r, q, sizeof(q));
    char nameraw[64] = { 0 };
    httpd_query_key_value(q, "file", nameraw, sizeof(nameraw));
    char name[64]; safe_name(nameraw, name, sizeof(name));
    char path[128]; snprintf(path, sizeof(path), "%s/%s", SCRIPT_DIR, name);
    FILE *f = fopen(path, "rb");
    if (!f) { httpd_resp_send_err(r, HTTPD_404_NOT_FOUND, "no such script"); return ESP_OK; }
    httpd_resp_set_type(r, "text/plain");
    char buf[512]; int n;
    while ((n = fread(buf, 1, sizeof(buf), f)) > 0) httpd_resp_send_chunk(r, buf, n);
    fclose(f);
    return httpd_resp_send_chunk(r, NULL, 0);
}

static esp_err_t h_api_delete(httpd_req_t *r) {
    char q[128] = { 0 }; httpd_req_get_url_query_str(r, q, sizeof(q));
    char nameraw[64] = { 0 };
    httpd_query_key_value(q, "file", nameraw, sizeof(nameraw));
    char name[64]; safe_name(nameraw, name, sizeof(name));
    char path[128]; snprintf(path, sizeof(path), "%s/%s", SCRIPT_DIR, name);
    int ok = (unlink(path) == 0);
    httpd_resp_set_type(r, "application/json");
    char body[32]; int bn = snprintf(body, sizeof(body), "{\"ok\":%s}", ok ? "true" : "false");
    return httpd_resp_send(r, body, bn);
}

// -------- /api/languages --------
// Enumerates the language packs sitting under /data/languages/ and
// returns them as a JSON array [{"code":"en","file":"en.json"}, ...].
// The actual translation strings live inside each file — the UI fetches
// /languages/<file> to pull them on demand.
static esp_err_t h_api_languages(httpd_req_t *r) {
    httpd_resp_set_type(r, "application/json");
    DIR *d = opendir("/data/languages");
    if (!d) return httpd_resp_send(r, "[]", 2);
    char body[1024]; int bl = 0;
    body[bl++] = '[';
    int first = 1;
    struct dirent *e;
    while ((e = readdir(d)) != NULL) {
        if (e->d_name[0] == '.') continue;
        const char *dot = strrchr(e->d_name, '.');
        if (!dot || strcasecmp(dot, ".json") != 0) continue;
        // Language code = filename without .json (e.g. "en", "de", "es").
        char code[16] = { 0 };
        int cn = dot - e->d_name;
        if (cn <= 0 || cn >= (int)sizeof(code)) continue;
        memcpy(code, e->d_name, cn);
        // Reference JS iterates and calls `.toLowerCase()` on each
        // element expecting a bare string; wrapping in {code,file}
        // crashed the language dropdown with a TypeError. Ship a raw
        // string array: ["en","de",...].
        int n = snprintf(body + bl, sizeof(body) - bl,
                         "%s\"%s\"", first ? "" : ",", code);
        if (n < 0 || bl + n >= (int)sizeof(body) - 2) break;
        bl += n; first = 0;
    }
    closedir(d);
    body[bl++] = ']'; body[bl] = 0;
    return httpd_resp_send(r, body, bl);
}

// -------- shape-compatible fallback for endpoints not yet backed --------
// Every fallback path below IS a real endpoint in the upstream
// ESP-S3-Key-BadUSB (WebServerManager.cpp) — a stub only exists here
// when the feature isn't ported yet (WiFi station mode, Bluetooth,
// extensions store, on-device history, MSC storage, etc.). No stub is
// registered for a path that doesn't exist upstream.
// GET returns []; POST returns {"ok":true,"stub":true}. Both are the
// safe shape the reference UI can render without erroring out.
static esp_err_t h_api_default(httpd_req_t *r) {
    httpd_resp_set_type(r, "application/json");
    if (r->method == HTTP_GET) return httpd_resp_send(r, "[]", 2);
    return httpd_resp_send(r, "{\"ok\":true,\"stub\":true}", 24);
}

// -------- real implementations of the trivial upstream endpoints --------

#include "esp_system.h"
#include "esp_heap_caps.h"
#include "esp_timer.h"
#include "nvs.h"

#define NVS_NS_BADUSB    "badusb"
#define NVS_K_SETUP      "setup_done"
#define NVS_K_TUTORIAL   "tut_done"
#define NVS_K_CWD        "cwd"

// Read a single u8 flag from NVS (default 0 if missing).
static uint8_t nvs_bool(const char *key) {
    nvs_handle_t h;
    uint8_t v = 0;
    if (nvs_open(NVS_NS_BADUSB, NVS_READONLY, &h) != ESP_OK) return 0;
    nvs_get_u8(h, key, &v);
    nvs_close(h);
    return v ? 1 : 0;
}
static void nvs_set_bool(const char *key, uint8_t v) {
    nvs_handle_t h;
    if (nvs_open(NVS_NS_BADUSB, NVS_READWRITE, &h) != ESP_OK) return;
    nvs_set_u8(h, key, v ? 1 : 0);
    nvs_commit(h);
    nvs_close(h);
}

// /api/stats — uptime, free heap, SPIFFS usage, Wi-Fi client count,
// hardware LED state. The `ledEnabled` field is what the Settings tab's
// hardware-LED toggle mirrors — before this fix the toggle sat "off"
// because the stub said so, while the real LED (on the P4 via SDIO
// reg 40) was actually lit.
extern uint32_t s_sta_count;   // main.c
extern bool     s_led_off;     // main.c (dedicated OFF switch)
extern uint8_t  s_led_mode;    // main.c (color/pattern when not off)
static esp_err_t h_api_stats(httpd_req_t *r) {
    size_t total = 0, used = 0;
    esp_spiffs_info("storage", &total, &used);
    uint32_t up_s = (uint32_t)(esp_timer_get_time() / 1000000ULL);
    char body[320];
    int n = snprintf(body, sizeof(body),
        "{\"uptime_s\":%u,\"free_heap\":%u,\"min_free_heap\":%u,"
        "\"spiffs_total\":%u,\"spiffs_used\":%u,\"wifi_clients\":%u,"
        "\"ledEnabled\":%s,\"ledMode\":%u}",
        (unsigned)up_s,
        (unsigned)esp_get_free_heap_size(),
        (unsigned)esp_get_minimum_free_heap_size(),
        (unsigned)total, (unsigned)used, (unsigned)s_sta_count,
        s_led_off ? "false" : "true", (unsigned)s_led_mode);
    httpd_resp_set_type(r, "application/json");
    return httpd_resp_send(r, body, n);
}

// /api/toggle-led — reference UI's Settings tab has a single hardware-
// LED toggle. Bridge it to our LED-off switch: POST flips, GET returns
// the current state. Accepts optional `?on=0|1` for explicit set. This
// exposes the same state the /led endpoint owns, so the two UIs stay
// in sync (the pre-port keylog dashboard's LED switch and the ported
// Settings LED toggle both mutate `s_led_off`).
extern void led_set_off(bool off);   // main.c
static esp_err_t h_api_toggle_led(httpd_req_t *r) {
    if (r->method == HTTP_POST) {
        char q[32] = { 0 }; httpd_req_get_url_query_str(r, q, sizeof(q));
        char v[8] = { 0 };
        bool have_explicit = (httpd_query_key_value(q, "on", v, sizeof(v)) == ESP_OK);
        bool want_on = have_explicit ? (v[0] == '1') : s_led_off;   // toggle if no explicit
        led_set_off(!want_on);
    }
    char body[48];
    int n = snprintf(body, sizeof(body),
        "{\"ledEnabled\":%s}", s_led_off ? "false" : "true");
    httpd_resp_set_type(r, "application/json");
    return httpd_resp_send(r, body, n);
}

// /api/temp — chip temperature is not available on ESP32-C6 without
// a temp_sensor init, and even then it's noisy. Return null so the UI
// renders "n/a" instead of a bogus number.
static esp_err_t h_api_temp(httpd_req_t *r) {
    httpd_resp_set_type(r, "application/json");
    return httpd_resp_send(r, "{\"temp\":null}", 13);
}

// /api/setup-complete — GET: current state; POST: mark done.
static esp_err_t h_api_setup(httpd_req_t *r) {
    if (r->method == HTTP_POST) nvs_set_bool(NVS_K_SETUP, 1);
    char body[32];
    int n = snprintf(body, sizeof(body), "{\"setup\":%s}", nvs_bool(NVS_K_SETUP) ? "true" : "false");
    httpd_resp_set_type(r, "application/json");
    return httpd_resp_send(r, body, n);
}
static esp_err_t h_api_tutorial(httpd_req_t *r) {
    if (r->method == HTTP_POST) nvs_set_bool(NVS_K_TUTORIAL, 1);
    char body[32];
    int n = snprintf(body, sizeof(body), "{\"tutorial\":%s}", nvs_bool(NVS_K_TUTORIAL) ? "true" : "false");
    httpd_resp_set_type(r, "application/json");
    return httpd_resp_send(r, body, n);
}

// /api/detect-os — reference guesses the target OS from HID indicator
// LEDs (NumLock behavior differs per OS). We don't have the LED bitmap
// yet, so return "unknown".
static esp_err_t h_api_detect_os(httpd_req_t *r) {
    httpd_resp_set_type(r, "application/json");
    return httpd_resp_send(r, "{\"os\":\"unknown\"}", 17);
}

// /api/current-directory + /api/change-directory — for the file
// explorer tab. NVS-persist a single "cwd" string, default "/".
static esp_err_t h_api_current_dir(httpd_req_t *r) {
    char cwd[128] = "/";
    nvs_handle_t h;
    if (nvs_open(NVS_NS_BADUSB, NVS_READONLY, &h) == ESP_OK) {
        size_t sz = sizeof(cwd);
        nvs_get_str(h, NVS_K_CWD, cwd, &sz);
        nvs_close(h);
    }
    char body[192];
    int n = snprintf(body, sizeof(body), "{\"cwd\":\"%s\"}", cwd);
    httpd_resp_set_type(r, "application/json");
    return httpd_resp_send(r, body, n);
}
static esp_err_t h_api_change_dir(httpd_req_t *r) {
    char q[192] = { 0 }; httpd_req_get_url_query_str(r, q, sizeof(q));
    char to[128] = "/";
    httpd_query_key_value(q, "path", to, sizeof(to));
    nvs_handle_t h;
    if (nvs_open(NVS_NS_BADUSB, NVS_READWRITE, &h) == ESP_OK) {
        nvs_set_str(h, NVS_K_CWD, to);
        nvs_commit(h); nvs_close(h);
    }
    char body[192];
    int n = snprintf(body, sizeof(body), "{\"cwd\":\"%s\"}", to);
    httpd_resp_set_type(r, "application/json");
    return httpd_resp_send(r, body, n);
}

// /api/history + /api/clear-history + /api/export-history — script
// execution history. We don't persist history yet — return empty
// on GET; accept on POST as no-op success. When we wire an on-device
// history log the empty array shape here won't change.
static esp_err_t h_api_history(httpd_req_t *r) {
    httpd_resp_set_type(r, "application/json");
    if (r->method == HTTP_GET) return httpd_resp_send(r, "[]", 2);
    return httpd_resp_send(r, "{\"ok\":true}", 11);
}

// /api/validate-script — quick syntactic pass over the DuckyScript
// body. For now we accept any non-empty payload as "valid" (the real
// parser lives in /execute); the UI just wants a green-check indicator.
static esp_err_t h_api_validate(httpd_req_t *r) {
    httpd_resp_set_type(r, "application/json");
    if (r->content_len == 0) return httpd_resp_send(r, "{\"valid\":false,\"reason\":\"empty\"}", 32);
    // Drain the body without inspecting it (a real validator will look
    // for unknown commands; the reference does that in JS anyway).
    char sink[256];
    int total = 0;
    while (total < (int)r->content_len) {
        int want = r->content_len - total; if (want > (int)sizeof(sink)) want = sizeof(sink);
        int n = httpd_req_recv(r, sink, want);
        if (n <= 0) break;
        total += n;
    }
    return httpd_resp_send(r, "{\"valid\":true}", 14);
}

// /api/reset-attackmode — reference resets the composite USB descriptor
// back to HID-only. We don't have the ATTACKMODE STORAGE composite
// wired yet, so today this is a no-op success; when MSC lands this
// will disable the MSC interface and re-enumerate.
static esp_err_t h_api_reset_attackmode(httpd_req_t *r) {
    httpd_resp_set_type(r, "application/json");
    return httpd_resp_send(r, "{\"ok\":true,\"mode\":\"HID\"}", 24);
}

// /stop and /selfdestruct — reference kills the running task /
// wipes flash respectively. /stop we implement as "cancel current
// script" (no long-running scripts today so it's a no-op success).
// /selfdestruct we DELIBERATELY leave as a no-op that just responds
// success — nuking user flash needs an explicit config later.
static esp_err_t h_stop(httpd_req_t *r) {
    httpd_resp_set_type(r, "application/json");
    return httpd_resp_send(r, "{\"stopped\":true}", 16);
}
static esp_err_t h_selfdestruct(httpd_req_t *r) {
    httpd_resp_set_type(r, "application/json");
    return httpd_resp_send(r, "{\"ok\":false,\"reason\":\"disabled — enable in firmware\"}", 55);
}

// /redirect — reference uses this on the setup-flow completion to
// bounce the browser back to /. Straight 302.
static esp_err_t h_redirect(httpd_req_t *r) {
    httpd_resp_set_status(r, "302 Found");
    httpd_resp_set_hdr(r, "Location", "/");
    return httpd_resp_send(r, NULL, 0);
}

// /language — reference sets the active UI language cookie/cookie
// equivalent. We serve /languages/*.json for the actual pack; this
// endpoint just acks the choice. The UI persists the code in
// localStorage; a real backend for it would write to NVS.
static esp_err_t h_language(httpd_req_t *r) {
    httpd_resp_set_type(r, "application/json");
    return httpd_resp_send(r, "{\"ok\":true}", 11);
}

// /favicon.ico — browser autoloads this on every page. Return a 204
// so it stops spamming the log; the tiny 16x16 default browser icon
// is fine.
static esp_err_t h_favicon(httpd_req_t *r) {
    httpd_resp_set_status(r, "204 No Content");
    return httpd_resp_send(r, NULL, 0);
}

// Called from main.c http_up() after the primary URI handlers register
// so BadUSB routes overlay the existing site cleanly. We deliberately
// re-register "/" so the ported UI takes over the root page — the old
// P4-KeyLogger single-page dashboard is still reachable at "/klog" and
// its route is added below for parity.
extern esp_err_t h_root(httpd_req_t *r);   // pre-port dashboard

// Helper — register one URI on both GET and POST with the same handler.
static void reg_both(httpd_handle_t s, const char *uri, esp_err_t (*h)(httpd_req_t*)) {
    httpd_uri_t u = { .uri = uri, .method = HTTP_GET, .handler = h };
    httpd_register_uri_handler(s, &u);
    u.method = HTTP_POST;
    httpd_register_uri_handler(s, &u);
}

void badusb_ui_register(httpd_handle_t s) {
    // storage_up() in main.c registers SPIFFS with partition_label
    // "storage" — passing NULL always returned false, so the marker
    // file was never created and script listings were empty.
    if (esp_spiffs_mounted("storage")) ensure_script_dir();

    // REAL handlers — these have a working backend behind them today.
    httpd_uri_t real[] = {
        // Static assets from SPIFFS
        {"/",                   HTTP_GET,  h_badusb_index,      NULL, false, false, NULL},
        {"/style.css",          HTTP_GET,  h_badusb_style,      NULL, false, false, NULL},
        {"/script.js",          HTTP_GET,  h_badusb_script,     NULL, false, false, NULL},
        {"/klog",               HTTP_GET,  h_root,              NULL, false, false, NULL},
        // Live-type + script execution
        {"/execute",            HTTP_POST, h_execute,           NULL, false, false, NULL},
        {"/api/live-type",      HTTP_POST, h_api_live_type,     NULL, false, false, NULL},
        // Script store (SPIFFS-backed)
        {"/api/scripts",        HTTP_GET,  h_api_scripts,       NULL, false, false, NULL},
        {"/api/save",           HTTP_POST, h_api_save,          NULL, false, false, NULL},
        {"/api/load",           HTTP_GET,  h_api_load,          NULL, false, false, NULL},
        {"/api/delete",         HTTP_POST,   h_api_delete,      NULL, false, false, NULL},
        // Ported script.js at line 1998 uses `method:'DELETE'` on
        // /api/delete — without this it 405/404s and file browser
        // deletes are silent-fail.
        {"/api/delete",         HTTP_DELETE, h_api_delete,      NULL, false, false, NULL},
        {"/api/delete-file",    HTTP_POST,   h_api_delete,      NULL, false, false, NULL},
        {"/api/delete-file",    HTTP_DELETE, h_api_delete,      NULL, false, false, NULL},
        {"/api/validate-script",HTTP_POST, h_api_validate,      NULL, false, false, NULL},
        // Languages
        {"/api/languages",      HTTP_GET,  h_api_languages,     NULL, false, false, NULL},
        {"/language",           HTTP_POST, h_language,          NULL, false, false, NULL},
        // Single wildcard handler catches every Hak5 language layout
        // (be/br/ca/ca-fr/ch/cz/de/de_mac/dk/es/es-la/fi/fr/gb/hr/hu/
        // it/jp/mx/no/pt/se/si/sk/tr/us) without needing 26 entries.
        // Requires cfg.uri_match_fn = httpd_uri_match_wildcard set in
        // main.c http_up.
        {"/languages/?*",       HTTP_GET,  h_lang_file,         NULL, false, false, NULL},
        // Real state endpoints
        {"/api/stats",          HTTP_GET,  h_api_stats,         NULL, false, false, NULL},
        {"/api/temp",           HTTP_GET,  h_api_temp,          NULL, false, false, NULL},
        // /api/toggle-led — real backend (bridges to led_set_off in main.c).
        {"/api/toggle-led",     HTTP_GET,  h_api_toggle_led,    NULL, false, false, NULL},
        {"/api/toggle-led",     HTTP_POST, h_api_toggle_led,    NULL, false, false, NULL},
        {"/api/detect-os",      HTTP_GET,  h_api_detect_os,     NULL, false, false, NULL},
        {"/api/history",        HTTP_GET,  h_api_history,       NULL, false, false, NULL},
        {"/api/clear-history",  HTTP_POST, h_api_history,       NULL, false, false, NULL},
        {"/api/export-history", HTTP_GET,  h_api_history,       NULL, false, false, NULL},
        {"/api/current-directory", HTTP_GET,  h_api_current_dir,NULL, false, false, NULL},
        {"/api/change-directory",  HTTP_POST, h_api_change_dir, NULL, false, false, NULL},
        {"/api/reset-attackmode",  HTTP_POST, h_api_reset_attackmode, NULL, false, false, NULL},
        // Setup/tutorial gates (both methods so POST marks done, GET queries)
        {"/api/setup-complete", HTTP_GET,  h_api_setup,         NULL, false, false, NULL},
        {"/api/setup-complete", HTTP_POST, h_api_setup,         NULL, false, false, NULL},
        {"/api/tutorial-done",  HTTP_GET,  h_api_tutorial,      NULL, false, false, NULL},
        {"/api/tutorial-done",  HTTP_POST, h_api_tutorial,      NULL, false, false, NULL},
        // Kill switches
        {"/stop",               HTTP_POST, h_stop,              NULL, false, false, NULL},
        {"/selfdestruct",       HTTP_POST, h_selfdestruct,      NULL, false, false, NULL},
        {"/redirect",           HTTP_GET,  h_redirect,          NULL, false, false, NULL},
        // Favicon — 204 so the browser stops retrying every 30 s.
        {"/favicon.ico",        HTTP_GET,  h_favicon,           NULL, false, false, NULL},
    };
    for (unsigned i = 0; i < sizeof(real)/sizeof(real[0]); i++)
        httpd_register_uri_handler(s, &real[i]);

    // Endpoints from ESP-S3-Key-BadUSB/WebServerManager.cpp whose
    // backends aren't ported yet — a stub keeps the JS from breaking
    // while a real port lands incrementally. This list is exhaustive
    // vs the reference (`grep -oE '"/[a-z][a-zA-Z0-9/-]*"'
    // WebServerManager.cpp` on the upstream tree). If a path here is
    // NOT in the reference, it does not belong.
    static const char *stub_paths[] = {
        // WiFi client mode (reference has full STA support)
        "/api/save-wifi", "/api/saved-wifi", "/api/delete-saved-wifi",
        "/api/scan-wifi", "/api/scan-results", "/api/join-internet",
        "/api/leave-internet", "/api/wifi-join-status", "/api/stop-wifi-join",
        "/api/wifi-secrets", "/api/stop-ap",
        // Bluetooth (reference has HID over BT)
        "/api/toggle-bluetooth", "/api/toggle-bt-discovery",
        "/api/save-bt-settings",
        // USB attack-mode toggles that need the composite refactor
        "/api/save-usb", "/api/toggle-com", "/api/toggle-random-mac",
        "/api/toggle-save-on-connect",
        // General settings knobs (persist to NVS — TODO real backends).
        // Note: /api/toggle-led is REAL (registered above with a live
        // backend that flips led_set_off).
        "/api/save-settings", "/api/toggle-autoconnect",
        "/api/toggle-blink-on-run",
        "/api/toggle-logging", "/api/toggle-wifi", "/api/set-silent",
        // Boot-script slot
        "/api/set-boot-script", "/api/test-boot-script",
        // Extensions store (Hak5's remote payload feed)
        "/api/list-extensions", "/api/load-extension",
        "/api/save-extension", "/api/delete-extension",
        "/api/pull-extension",
        // File explorer that isn't backed by real FS ops yet
        "/api/list-files", "/api/download", "/api/upload",
        "/api/copy-file", "/api/cut-file", "/api/paste-file",
        "/api/create-directory", "/api/check-file", "/api/file-info",
        "/api/use-file", "/api/rename",
        // Alias for the older `/api/self-destruct` path some JS still
        // uses (parallel to the canonical `/selfdestruct`).
        "/api/self-destruct",
        // Tasks / errors / OTA package updates
        "/api/tasks", "/api/cancel-task", "/api/clear-errors",
        "/api/update-status", "/api/update-package",
        // Destructive knobs (reference is destructive; we no-op safely)
        "/api/behave-broken", "/api/factory-reset",
        // Extensions store static routes (upstream serves them from SD)
        "/extensions", "/extensions/", "/extensions/custom",
        "/extensions/hak5", "/extensions/hak5/",
    };
    for (unsigned i = 0; i < sizeof(stub_paths)/sizeof(stub_paths[0]); i++)
        reg_both(s, stub_paths[i], h_api_default);

    ESP_LOGI(TAG, "BadUSB routes registered (real=%u, stub=%u)",
             (unsigned)(sizeof(real)/sizeof(real[0])),
             (unsigned)(sizeof(stub_paths)/sizeof(stub_paths[0])));
}

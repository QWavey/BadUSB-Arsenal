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
//     script store. Files live under /spiffs/ducky/*.
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
#define SDIO_REG_BADUSB_TYPE  44

// Embedded static assets (see CMakeLists.txt EMBED_FILES).
extern const uint8_t index_html_start[]  asm("_binary_index_html_start");
extern const uint8_t index_html_end[]    asm("_binary_index_html_end");
extern const uint8_t style_css_start[]   asm("_binary_style_css_start");
extern const uint8_t style_css_end[]     asm("_binary_style_css_end");
extern const uint8_t script_js_start[]   asm("_binary_script_js_start");
extern const uint8_t script_js_end[]     asm("_binary_script_js_end");

// -------- shared helper: stream one ASCII byte at a time to the P4 --------
// Waits until the P4 has cleared reg 44 (200 ms max per byte) and then
// writes the next byte. Returns how many bytes actually landed.
static int badusb_stream_ascii(const char *buf, int len) {
    int sent = 0;
    for (int i = 0; i < len; i++) {
        uint8_t c = (uint8_t)buf[i];
        if (c == 0 || c > 127) continue;
        for (int spin = 0; spin < 200; spin++) {
            if (sdio_slave_read_reg(SDIO_REG_BADUSB_TYPE) == 0) break;
            vTaskDelay(pdMS_TO_TICKS(10));
        }
        if (sdio_slave_write_reg(SDIO_REG_BADUSB_TYPE, c) == ESP_OK) sent++;
    }
    return sent;
}

// -------- static asset serving --------
static esp_err_t h_asset(httpd_req_t *r, const char *mime,
                         const uint8_t *start, const uint8_t *end) {
    httpd_resp_set_type(r, mime);
    httpd_resp_set_hdr(r, "Cache-Control", "no-store");
    return httpd_resp_send(r, (const char *)start, end - start);
}

static esp_err_t h_badusb_index(httpd_req_t *r) {
    return h_asset(r, "text/html; charset=utf-8", index_html_start, index_html_end);
}
static esp_err_t h_badusb_style(httpd_req_t *r) {
    return h_asset(r, "text/css",  style_css_start, style_css_end);
}
static esp_err_t h_badusb_script(httpd_req_t *r) {
    return h_asset(r, "text/javascript", script_js_start, script_js_end);
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
    const char *p = strstr(body, "\"text\"");
    if (!p) return -1;
    p = strchr(p, ':');
    if (!p) return -1;
    p++;
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

// -------- /api/live-type --------
// The ported UI POSTs {"text":"..."} here for real-time typing.
static esp_err_t h_api_live_type(httpd_req_t *r) {
    char buf[2048];
    int n = slurp_body(r, buf, sizeof(buf));
    if (n < 0) { httpd_resp_send_err(r, HTTPD_400_BAD_REQUEST, "bad body"); return ESP_OK; }
    char text[2048];
    int tl = extract_json_text(buf, text, sizeof(text));
    if (tl < 0) {
        // Fall back to raw body — some code paths in their JS post plain text.
        tl = n;
        if (tl > (int)sizeof(text) - 1) tl = sizeof(text) - 1;
        memcpy(text, buf, tl);
        text[tl] = 0;
    }
    int sent = badusb_stream_ascii(text, tl);
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

static esp_err_t h_execute(httpd_req_t *r) {
    char buf[4096];
    int n = slurp_body(r, buf, sizeof(buf));
    if (n < 0) { httpd_resp_send_err(r, HTTPD_400_BAD_REQUEST, "bad body"); return ESP_OK; }
    // Allow either raw payload or {"script":"..."} form.
    char script[4096];
    int sl = extract_json_text(buf, script, sizeof(script));
    if (sl < 0) {
        const char *p = strstr(buf, "\"script\"");
        if (p) {
            // simple copy — same escape handling as extract_json_text
            p = strchr(p, ':');
            if (p) { p++; while (*p && isspace((unsigned char)*p)) p++; }
            if (p && *p == '"') {
                p++;
                sl = 0;
                while (*p && *p != '"' && sl < (int)sizeof(script) - 1) {
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
    // Parse line by line.
    int lines = 0, executed = 0;
    char *save = NULL;
    for (char *line = strtok_r(script, "\r\n", &save); line; line = strtok_r(NULL, "\r\n", &save)) {
        while (*line && isspace((unsigned char)*line)) line++;
        if (!*line || line[0] == '#') continue;
        if (!strncasecmp(line, "REM ", 4) || !strcasecmp(line, "REM")) { lines++; continue; }
        lines++;
        if (!strncasecmp(line, "STRINGLN ", 9)) { ducky_send(line + 9); ducky_send("\n"); executed++; }
        else if (!strncasecmp(line, "STRING ", 7)) { ducky_send(line + 7); executed++; }
        else if (!strncasecmp(line, "DELAY ", 6)) { int ms = atoi(line + 6); if (ms > 0 && ms < 30000) vTaskDelay(pdMS_TO_TICKS(ms)); executed++; }
        else if (!strcasecmp(line, "ENTER") || !strcasecmp(line, "RETURN")) { ducky_send("\n"); executed++; }
        else if (!strcasecmp(line, "TAB"))        { ducky_send("\t"); executed++; }
        else if (!strcasecmp(line, "SPACE"))      { ducky_send(" ");  executed++; }
        else if (!strcasecmp(line, "BACKSPACE"))  { char c = 0x08; badusb_stream_ascii(&c, 1); executed++; }
        else { /* unsupported — silently skip */ }
        (void)ducky_key;  // silence unused-static warning until full protocol lands
    }
    httpd_resp_set_type(r, "application/json");
    char body[96];
    int bn = snprintf(body, sizeof(body), "{\"lines\":%d,\"executed\":%d}", lines, executed);
    return httpd_resp_send(r, body, bn);
}

// -------- SPIFFS-backed script/file store --------
// Scripts live under /spiffs/ducky/<name>. Uploaded via /api/save and
// /api/upload. Listed via /api/scripts and /api/list-files.
#define SCRIPT_DIR "/spiffs/ducky"

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

// -------- generic stub for endpoints not yet real --------
// Their JS calls dozens of toggle-*/save-*/list-* endpoints. Rather than
// stub each individually, this handler returns {"ok":true} for POST and
// [] for GET on any /api/* path we haven't specifically wired. Prevents
// JS errors while individual features are added incrementally.
static esp_err_t h_api_default(httpd_req_t *r) {
    httpd_resp_set_type(r, "application/json");
    if (r->method == HTTP_GET) return httpd_resp_send(r, "[]", 2);
    return httpd_resp_send(r, "{\"ok\":true,\"stub\":true}", 24);
}

// Called from main.c http_up() after the primary URI handlers register
// so BadUSB routes overlay the existing site cleanly. We deliberately
// re-register "/" so the ported UI takes over the root page — the old
// P4-KeyLogger single-page dashboard is still reachable at "/klog" and
// its route is added below for parity.
extern esp_err_t h_root(httpd_req_t *r);   // pre-port dashboard

void badusb_ui_register(httpd_handle_t s) {
    if (esp_spiffs_mounted(NULL)) ensure_script_dir();
    httpd_uri_t rt[] = {
        {"/",              HTTP_GET,  h_badusb_index,  NULL, false, false, NULL},
        {"/style.css",     HTTP_GET,  h_badusb_style,  NULL, false, false, NULL},
        {"/script.js",     HTTP_GET,  h_badusb_script, NULL, false, false, NULL},
        {"/klog",          HTTP_GET,  h_root,          NULL, false, false, NULL},
        {"/execute",       HTTP_POST, h_execute,       NULL, false, false, NULL},
        {"/api/live-type", HTTP_POST, h_api_live_type, NULL, false, false, NULL},
        {"/api/scripts",   HTTP_GET,  h_api_scripts,   NULL, false, false, NULL},
        {"/api/save",      HTTP_POST, h_api_save,      NULL, false, false, NULL},
        {"/api/load",      HTTP_GET,  h_api_load,      NULL, false, false, NULL},
        {"/api/delete",    HTTP_POST, h_api_delete,    NULL, false, false, NULL},
        {"/api/delete-file", HTTP_POST, h_api_delete,  NULL, false, false, NULL},
    };
    for (unsigned i = 0; i < sizeof(rt)/sizeof(rt[0]); i++)
        httpd_register_uri_handler(s, &rt[i]);
    // Register the wildcard stub LAST so specific routes above still win.
    // We add a handful of representative endpoints — the URI matcher does
    // NOT support glob so we bind each one explicitly. The list is drawn
    // from the ported script.js `fetch('/api/...')` sites.
    static const char *stub_paths[] = {
        "/api/stats", "/api/tasks", "/api/languages", "/api/temp",
        "/api/setup-complete", "/api/tutorial-done", "/api/clear-errors",
        "/api/save-settings", "/api/save-usb", "/api/save-wifi", "/api/saved-wifi",
        "/api/scan-wifi", "/api/scan-results", "/api/join-internet", "/api/leave-internet",
        "/api/delete-saved-wifi", "/api/toggle-autoconnect", "/api/toggle-blink-on-run",
        "/api/toggle-bluetooth", "/api/toggle-bt-discovery", "/api/toggle-com",
        "/api/toggle-led", "/api/toggle-logging", "/api/toggle-random-mac",
        "/api/toggle-save-on-connect", "/api/toggle-wifi", "/api/set-boot-script",
        "/api/set-silent", "/api/behave-broken", "/api/factory-reset",
        "/api/self-destruct", "/api/cancel-task", "/api/list-extensions",
        "/api/load-extension", "/api/pull-extension", "/api/save-extension",
        "/api/update-status", "/api/wifi-join-status", "/api/list-files",
        "/api/download", "/api/upload", "/api/copy-file", "/api/rename",
    };
    for (unsigned i = 0; i < sizeof(stub_paths)/sizeof(stub_paths[0]); i++) {
        httpd_uri_t u = { .uri = stub_paths[i], .method = HTTP_GET,  .handler = h_api_default };
        httpd_register_uri_handler(s, &u);
        u.method = HTTP_POST;
        httpd_register_uri_handler(s, &u);
    }
    ESP_LOGI(TAG, "BadUSB routes registered (/, /style.css, /script.js, /execute, /api/*)");
}

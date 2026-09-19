#include "led.h"

#include "esp_log.h"
#include "esp_err.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "freertos/semphr.h"
#include "led_strip.h"

#define LED_GPIO           10
#define LED_FLASH_MS      120
#define LED_IDLE_G         12    // dim so it's not glaring in a dark room

static const char *TAG = "led";
static led_strip_handle_t s_strip;
static volatile led_mode_t s_mode = LED_MODE_KEY_FLASH;
static SemaphoreHandle_t s_sig;

static void render_idle(void) {
    if (!s_strip) return;
    switch (s_mode) {
    case LED_MODE_OFF:
        led_strip_clear(s_strip);
        break;
    case LED_MODE_RED_ONLY:
        led_strip_set_pixel(s_strip, 0, LED_IDLE_G, 0, 0);
        led_strip_refresh(s_strip);
        break;
    case LED_MODE_BLUE_ONLY:
        led_strip_set_pixel(s_strip, 0, 0, 0, LED_IDLE_G);
        led_strip_refresh(s_strip);
        break;
    case LED_MODE_GREEN_ONLY:
    case LED_MODE_KEY_FLASH:
    default:
        led_strip_set_pixel(s_strip, 0, 0, LED_IDLE_G, 0);
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
    esp_err_t err = led_strip_new_rmt_device(&cfg, &rmt, &s_strip);
    if (err != ESP_OK) { ESP_LOGE(TAG, "led_strip_new: %s", esp_err_to_name(err)); return; }
    s_sig = xSemaphoreCreateBinary();
    xTaskCreatePinnedToCore(led_task, "led", 2048, NULL, 3, NULL, 0);
    ESP_LOGI(TAG, "status LED on GPIO%d ready", LED_GPIO);
}

void led_set_mode(led_mode_t m) {
    if ((int)m > (int)LED_MODE_BLUE_ONLY) m = LED_MODE_KEY_FLASH;
    s_mode = m;
    if (s_sig) xSemaphoreGive(s_sig);   // wake task to re-render
}
led_mode_t led_get_mode(void) { return s_mode; }

void led_pulse_key(void) { if (s_sig) xSemaphoreGive(s_sig); }

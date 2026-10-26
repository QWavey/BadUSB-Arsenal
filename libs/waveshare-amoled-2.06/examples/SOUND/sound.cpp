#include <Arduino.h>
#include <lvgl.h>
#include <Wire.h>
#include "ESP32-S3-Screen-AMOLED-2.06.h"
#include "ESP32-S3-Touch-AMOLED-2.06.h"
#include "esp_check.h"
#include "es8311.h"
#include "ESP_I2S.h"
#include "canon.h"

// --- Keep the required external libs ---
#include "demos/lv_demos.h"

// === Global Objects ===
ScreenClass Screen;
TouchClass  Touch;
I2SClass    i2s;

// === LVGL ===
#define LVGL_TICK_MS 2
lv_obj_t* label;

// === Audio / ES8311 Config ===
#define EXAMPLE_SAMPLE_RATE     16000
#define EXAMPLE_VOICE_VOLUME    90
#define EXAMPLE_MIC_GAIN        (es8311_mic_gain_t)(3)

esp_err_t es8311_codec_init(void) {
    es8311_handle_t es_handle = es8311_create(0, ES8311_ADDRRES_0);
    ESP_RETURN_ON_FALSE(es_handle, ESP_FAIL, "ES8311", "create failed");

    const es8311_clock_config_t es_clk = {
        .mclk_inverted       = false,
        .sclk_inverted       = false,
        .mclk_from_mclk_pin  = true,
        .mclk_frequency      = EXAMPLE_SAMPLE_RATE * 256,
        .sample_frequency    = EXAMPLE_SAMPLE_RATE
    };

    ESP_ERROR_CHECK(es8311_init(es_handle, &es_clk, ES8311_RESOLUTION_16, ES8311_RESOLUTION_16));
    ESP_ERROR_CHECK(es8311_sample_frequency_config(es_handle, es_clk.mclk_frequency, es_clk.sample_frequency));
    ESP_ERROR_CHECK(es8311_microphone_config(es_handle, false));
    ESP_ERROR_CHECK(es8311_voice_volume_set(es_handle, EXAMPLE_VOICE_VOLUME, NULL));
    ESP_ERROR_CHECK(es8311_microphone_gain_set(es_handle, EXAMPLE_MIC_GAIN));
    return ESP_OK;
}

void audio_task(void* param) {
    // Configure I2S pins from your board definition
    i2s.setPins(BCLKPIN, WSPIN, DIPIN, DOPIN, MCLKPIN);
    if (!i2s.begin(I2S_MODE_STD, EXAMPLE_SAMPLE_RATE, I2S_DATA_BIT_WIDTH_16BIT,
                   I2S_SLOT_MODE_STEREO, I2S_STD_SLOT_BOTH)) {
        Serial.println("I2S init failed!");
        vTaskDelete(NULL);
    }

    Wire.begin(15, 14);
    if (es8311_codec_init() != ESP_OK) {
        Serial.println("ES8311 init failed!");
        vTaskDelete(NULL);
    }

    // Loop forever writing PCM data
    while (true) {
        i2s.write((uint8_t*)canon_pcm, canon_pcm_len);
        vTaskDelay(1);
    }
}

// === LVGL Tick ===
void lvgl_tick_task(void* arg) {
    while (true) {
        lv_tick_inc(LVGL_TICK_MS);
        vTaskDelay(pdMS_TO_TICKS(LVGL_TICK_MS));
    }
}

// === Setup ===
void setup() {
    Serial.begin(115200);
    delay(500);
    Serial.println("Starting LVGL + Audio + Touch demo...");

    // Initialize display (LVGL inside)
    Screen.on();
    Serial.println("Display initialized");

    // Initialize touch
    Touch.on();
    Serial.println("Touch initialized");

    // Create simple LVGL UI
    lv_obj_t* scr = lv_scr_act();
    lv_obj_set_style_bg_color(scr, lv_color_black(), 0);

    label = lv_label_create(scr);
    lv_label_set_text(label, "Hello LVGL + ES8311!");
    lv_obj_align(label, LV_ALIGN_CENTER, 0, 0);

    // You can switch to another demo here if desired
    // lv_demo_widgets();
    // lv_demo_music();

    // Start LVGL tick task
    xTaskCreatePinnedToCore(lvgl_tick_task, "lv_tick_task", 2048, NULL, 1, NULL, 0);

    // Start audio playback
    xTaskCreatePinnedToCore(audio_task, "audio_task", 4096, NULL, 1, NULL, 1);

    Serial.println("Setup complete.");
}

// === Loop ===
void loop() {
    lv_task_handler();

    // Optional: read and log touch coordinates
    if (Touch.isTouched()) {
        int32_t tx, ty;
        Touch.getTouchPoint(tx, ty);
        Serial.printf("Touch at x=%ld y=%ld\n", tx, ty);
    }

    delay(5);
}

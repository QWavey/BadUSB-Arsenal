#include <Arduino.h>
#include "ESP32-S3-Screen-AMOLED-2.06.h"
#include "ESP32-S3-Touch-AMOLED-2.06.h"
#include <SD_MMC.h>
#include <FS.h>

ScreenClass Screen;
TouchClass Touch;

lv_obj_t* label;
unsigned long last_tick = 0;

void setup() {
    Serial.begin(115200);
    delay(1000);
    Serial.println("Starting SD card listing test...");

    // Initialize display
    Screen.on();
    Serial.println("Display initialized");

    // Initialize touch
    Touch.on();
    Serial.println("Touch initialized");

    // Initialize SD_MMC
    SD_MMC.setPins(SDMMC_CLK, SDMMC_CMD, SDMMC_DATA);
    if (!SD_MMC.begin("/sdcard", true)) {
        Serial.println("Card Mount Failed");
    } else {
        Serial.println("SD card mounted successfully");
    }

    // Create UI
    lv_obj_t* scr = lv_scr_act();
    lv_obj_set_style_bg_color(scr, lv_color_hex(0x000000), 0);

    label = lv_label_create(scr);
    lv_label_set_long_mode(label, LV_LABEL_LONG_WRAP);
    lv_obj_set_width(label, 300);
    lv_obj_align(label, LV_ALIGN_CENTER, 0, 0);
    lv_obj_set_style_text_color(label, lv_color_hex(0xFFFFFF), 0);

    // List root directory
    String sd_info = "SD Card Contents:\n";
    File root = SD_MMC.open("/");
    if (root && root.isDirectory()) {
        File file = root.openNextFile();
        while (file) {
            if (file.isDirectory()) {
                sd_info += "DIR : " + String(file.name()) + "\n";
            } else {
                sd_info += "FILE: " + String(file.name()) + " SIZE: " + String(file.size()) + "\n";
            }
            file = root.openNextFile();
        }
    } else {
        sd_info += "Failed to open root directory";
    }

    lv_label_set_text(label, sd_info.c_str());
}

void loop() {
    unsigned long current_millis = millis();
    if (current_millis - last_tick >= 5) {
        lv_tick_inc(5);
        last_tick = current_millis;
    }
    lv_task_handler();

    // Optional: react to touch
    if (Touch.isTouched()) {
        int32_t x, y;
        Touch.getTouchPoint(x, y);
        Serial.printf("Touch at x=%ld y=%ld\n", x, y);
    }

    delay(2); // Keep loop responsive
}

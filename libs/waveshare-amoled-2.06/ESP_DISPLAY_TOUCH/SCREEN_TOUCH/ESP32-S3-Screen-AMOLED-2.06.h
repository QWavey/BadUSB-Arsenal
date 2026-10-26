#pragma once
#include <Arduino.h>
#include "Arduino_GFX_Library.h"
#include "Arduino_DriveBus_Library.h"
#include <lvgl.h>
#include "pin_config.h"

class ScreenClass {
public:
    ScreenClass() : bus(nullptr), gfx(nullptr), disp(nullptr) {}

    void on() {
        if (!gfx) initDisplay();
        gfx->begin();
        panelOn() = true;
        gfx->fillScreen(RGB565_BLACK);
        // LVGL only redraws areas that changed, so mark the active screen dirty
        // to repaint the whole UI once the panel is back on.
        lv_obj_t* scr = lv_screen_active();
        if (scr) lv_obj_invalidate(scr);
        Serial.println("Display powered on");
    }

    void off() {
        if (gfx) {
            // flush_cb stops drawing while the panel is off
            panelOn() = false;
            gfx->fillScreen(RGB565_BLACK);
            Serial.println("Display powered off");
        }
    }

    lv_obj_t* button_create(lv_obj_t* parent, const char* text, lv_event_cb_t cb, int w, int h, int x=0, int y=0) {
        lv_obj_t* btn = lv_button_create(parent);
        lv_obj_set_size(btn, w, h);
        lv_obj_align(btn, LV_ALIGN_CENTER, x, y);
        if(cb) lv_obj_add_event_cb(btn, cb, LV_EVENT_CLICKED, nullptr);

        lv_obj_t* label = lv_label_create(btn);
        lv_label_set_text(label, text);
        lv_obj_center(label);
        return btn;
    }

    lv_obj_t* slider_create(lv_obj_t* parent, int w, int h, int x=0, int y=0, lv_event_cb_t cb=NULL) {
        lv_obj_t* slider = lv_slider_create(parent);
        lv_obj_set_size(slider, w, h);
        lv_obj_align(slider, LV_ALIGN_CENTER, x, y);
        if(cb) lv_obj_add_event_cb(slider, cb, LV_EVENT_VALUE_CHANGED, nullptr);
        return slider;
    }

    lv_obj_t* checkbox_create(lv_obj_t* parent, const char* text, int x=0, int y=0) {
        lv_obj_t* cb = lv_checkbox_create(parent);
        lv_checkbox_set_text(cb, text);
        lv_obj_align(cb, LV_ALIGN_CENTER, x, y);
        return cb;
    }

    lv_display_t* getDisplay() { return disp; }
    Arduino_GFX* getGfx() { return gfx; }

private:
    Arduino_DataBus* bus;
    Arduino_GFX* gfx;
    lv_display_t* disp;
    lv_color_t buf[LCD_WIDTH * 40]; // KEEP original buffer size

    // Panel power state. A function-local static keeps this header C++11-safe (no static inline).
    static bool& panelOn() {
        static bool on = true;
        return on;
    }

    static void flush_cb(lv_display_t* disp, const lv_area_t* area, uint8_t* pixel_map) {
        if (panelOn()) {
            Arduino_GFX* gfx = (Arduino_GFX*)lv_display_get_user_data(disp);
            uint32_t w = area->x2 - area->x1 + 1;
            uint32_t h = area->y2 - area->y1 + 1;
            gfx->draw16bitRGBBitmap(area->x1, area->y1, (uint16_t*)pixel_map, w, h);
        }
        lv_display_flush_ready(disp);
    }

    void initDisplay() {
        Serial.println("Initializing display hardware...");
        
        bus = new Arduino_ESP32QSPI(LCD_CS, LCD_SCLK, LCD_SDIO0, LCD_SDIO1, LCD_SDIO2, LCD_SDIO3);
        gfx = new Arduino_CO5300(bus, LCD_RESET, 0, LCD_WIDTH, LCD_HEIGHT, 22, 0, 0, 0);

        Serial.println("Initializing LVGL...");
        lv_init();

        disp = lv_display_create(LCD_WIDTH, LCD_HEIGHT);
        lv_display_set_flush_cb(disp, flush_cb);
        // KEEP original buffer configuration
        lv_display_set_buffers(disp, buf, NULL, sizeof(buf), LV_DISPLAY_RENDER_MODE_PARTIAL);
        lv_display_set_user_data(disp, gfx);
        lv_display_set_color_format(disp, LV_COLOR_FORMAT_RGB565);
        
        Serial.printf("Display initialized: %dx%d\n", LCD_WIDTH, LCD_HEIGHT);
    }
};

extern ScreenClass Screen;

#pragma once
#include <Arduino.h>
#include "Arduino_GFX_Library.h"
#include "Arduino_DriveBus_Library.h"
#include <lvgl.h>
#include <esp_heap_caps.h>   // Watch port: heap_caps_aligned_alloc for PSRAM buffer
#include "pin_config.h"

class ScreenClass {
public:
    ScreenClass() : bus(nullptr), gfx(nullptr), disp(nullptr), _brightOverlay(nullptr) {}

    void on() {
        if (!gfx) initDisplay();
        gfx->begin();
        gfx->fillScreen(RGB565_BLACK);
        Serial.println("Display powered on");
    }

    void off() {
        if (gfx) {
            gfx->fillScreen(RGB565_BLACK);
            Serial.println("Display powered off");
        }
    }

    // Brightness via SOFTWARE OVERLAY — QWavey's AMOLEDBrightness
    // recipe (github.com/QWavey/ESP32-S3-Waveshare-OLED-Watch, path
    // ESP_DISPLAY_TOUCH/BRIGHTNESS). The CO5300 panel on this watch
    // ignores MIPI DCS 0x51, so we lay a black lv_obj on lv_layer_top
    // and vary its opacity: brightness 100% = fully transparent, 10% =
    // ~90% opacity black covering the pixels. Non-clickable so touches
    // pass through to widgets underneath.
    //
    // pct is 0..100 percent. 0 = full black overlay (screen "off"),
    // 100 = no overlay (native brightness).
    void setBrightness(uint8_t pct) {
        if (pct > 100) pct = 100;
        lv_obj_t* top = lv_layer_top();
        if (!top) return;
        // Lazy-create the overlay on first call.
        if (!_brightOverlay) {
            _brightOverlay = lv_obj_create(top);
            lv_obj_remove_style_all(_brightOverlay);
            lv_obj_set_size(_brightOverlay, LCD_WIDTH, LCD_HEIGHT);
            lv_obj_set_pos(_brightOverlay, 0, 0);
            lv_obj_set_style_bg_color(_brightOverlay, lv_color_black(), 0);
            lv_obj_set_style_border_width(_brightOverlay, 0, 0);
            lv_obj_clear_flag(_brightOverlay, LV_OBJ_FLAG_CLICKABLE);
            lv_obj_clear_flag(_brightOverlay, LV_OBJ_FLAG_SCROLLABLE);
            // Not focusable, not scrollable, no ripple.
            lv_obj_clear_flag(_brightOverlay, LV_OBJ_FLAG_CLICK_FOCUSABLE);
        }
        // Opacity: (100 - pct) * 255 / 100. pct=100 → opa=0 (off);
        // pct=10 → opa=229 (~90% black).
        uint8_t opa = (uint8_t)((100 - (int)pct) * 255 / 100);
        lv_obj_set_style_bg_opa(_brightOverlay, opa, 0);
        // Keep the overlay above every UI child that might get created
        // later (toasts, modals) — LVGL puts new lv_layer_top() children
        // above existing ones by default, so re-foreground us each set.
        lv_obj_move_foreground(_brightOverlay);
    }

    lv_obj_t* button_create(lv_obj_t* parent, const char* text, lv_event_cb_t cb, int w, int h, int x=0, int y=0) {
        lv_obj_t* btn = lv_btn_create(parent);
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
    // Watch port RAM opt (scout finding #1): the LVGL flush buffer used to
    // be `lv_color_t buf[LCD_WIDTH * 40]` — a static member sitting in .bss
    // that ate ~49 KB of internal DRAM. QSPI flush is CPU-driven so PSRAM
    // backing works fine; we now allocate it in initDisplay() via
    // heap_caps_aligned_alloc with MALLOC_CAP_SPIRAM. Also bumped from 40
    // rows to 60 for smoother partial redraws (still just 49 KB, cheap in
    // PSRAM). Falls back to internal on alloc failure.
    lv_color_t* buf;
    size_t      buf_bytes;
    lv_obj_t*   _brightOverlay;   // black dimming overlay on lv_layer_top

    static void flush_cb(lv_display_t* disp, const lv_area_t* area, uint8_t* pixel_map) {
        Arduino_GFX* gfx = (Arduino_GFX*)lv_display_get_user_data(disp);
        uint32_t w = area->x2 - area->x1 + 1;
        uint32_t h = area->y2 - area->y1 + 1;
        gfx->draw16bitRGBBitmap(area->x1, area->y1, (uint16_t*)pixel_map, w, h);
        lv_display_flush_ready(disp);
    }

    void initDisplay() {
        Serial.println("Initializing display hardware...");

        bus = new Arduino_ESP32QSPI(LCD_CS, LCD_SCLK, LCD_SDIO0, LCD_SDIO1, LCD_SDIO2, LCD_SDIO3);
        gfx = new Arduino_CO5300(bus, LCD_RESET, 0, LCD_WIDTH, LCD_HEIGHT, 22, 0, 0, 0);

        Serial.println("Initializing LVGL...");
        lv_init();

        // Try PSRAM first (48-64 KB depending on row count).
        const int LINES = 60;
        buf_bytes = (size_t)LCD_WIDTH * LINES * sizeof(lv_color_t);
        buf = (lv_color_t*)heap_caps_aligned_alloc(4, buf_bytes,
                             MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
        if (!buf) {
            Serial.println("[Screen] PSRAM alloc failed — falling back to DRAM");
            buf = (lv_color_t*)heap_caps_aligned_alloc(4, buf_bytes, MALLOC_CAP_8BIT);
        }
        if (!buf) {
            Serial.println("[Screen] FATAL: no memory for LVGL flush buffer");
            return;
        }
        Serial.printf("[Screen] flush buffer %u B in %s\n", (unsigned)buf_bytes,
                      heap_caps_get_allocated_size(buf) > 0 &&
                      esp_ptr_external_ram(buf) ? "PSRAM" : "DRAM");

        disp = lv_display_create(LCD_WIDTH, LCD_HEIGHT);
        lv_display_set_flush_cb(disp, flush_cb);
        lv_display_set_buffers(disp, buf, NULL, buf_bytes, LV_DISPLAY_RENDER_MODE_PARTIAL);
        lv_display_set_user_data(disp, gfx);
        lv_display_set_color_format(disp, LV_COLOR_FORMAT_RGB565);

        Serial.printf("Display initialized: %dx%d\n", LCD_WIDTH, LCD_HEIGHT);
    }
};

extern ScreenClass Screen;

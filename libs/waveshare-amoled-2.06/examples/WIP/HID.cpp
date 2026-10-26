// This file types something when the button has been pressed. Its just like the others, but with added syntaxes
#include <Arduino.h>
#include "ESP32-S3-Screen-AMOLED-2.06.h"
#include "ESP32-S3-Touch-AMOLED-2.06.h"
#include <lvgl.h>
#include <USB.h>
#include <USBHIDKeyboard.h>

// --- Global objects ---
ScreenClass Screen;
TouchClass Touch;
USBHIDKeyboard Keyboard;
USBHID HID;

lv_obj_t* label;
unsigned long last_tick = 0;
unsigned long last_debug = 0;

// --- Forward declarations ---
void sendHello();
lv_obj_t* button_create(lv_obj_t* parent, const char* text, lv_event_cb_t cb);
lv_obj_t* slider_create(lv_obj_t* parent, int min, int max, int value);
lv_obj_t* checkbox_create(lv_obj_t* parent, const char* label_text);

// --- Callback: button pressed ---
void button_event_cb(lv_event_t* e) {
  if (lv_event_get_code(e) == LV_EVENT_CLICKED) {
    lv_label_set_text(label, "Button pressed!\nTyping HELLO...");
    Serial.println("Button pressed — sending HELLO");

    // Correct cast for LVGL v9.x
    lv_obj_t* btn = (lv_obj_t*)lv_event_get_target(e);

    // Change color temporarily
    lv_obj_set_style_bg_color(btn, lv_palette_main(LV_PALETTE_RED), 0);
    sendHello();
    delay(200);
    lv_obj_set_style_bg_color(btn, lv_palette_main(LV_PALETTE_BLUE), 0);
  }
}

// --- Function: Send HELLO over USB keyboard ---
void sendHello() {
  Keyboard.print("HELLO");
  Serial.println("Typed: HELLO");
}

// --- LVGL UI creation helpers ---
lv_obj_t* button_create(lv_obj_t* parent, const char* text, lv_event_cb_t cb) {
  lv_obj_t* btn = lv_btn_create(parent);
  lv_obj_set_size(btn, 160, 70);
  lv_obj_set_style_bg_color(btn, lv_palette_main(LV_PALETTE_BLUE), 0);
  lv_obj_align(btn, LV_ALIGN_CENTER, 0, 60);
  lv_obj_add_event_cb(btn, cb, LV_EVENT_CLICKED, NULL);

  lv_obj_t* lbl = lv_label_create(btn);
  lv_label_set_text(lbl, text);
  lv_obj_center(lbl);
  lv_obj_set_style_text_color(lbl, lv_color_white(), 0);
  lv_obj_set_style_text_font(lbl, &lv_font_montserrat_16, 0);
  return btn;
}

lv_obj_t* slider_create(lv_obj_t* parent, int min, int max, int value) {
  lv_obj_t* slider = lv_slider_create(parent);
  lv_obj_set_size(slider, 200, 20);
  lv_slider_set_range(slider, min, max);
  lv_slider_set_value(slider, value, LV_ANIM_OFF);
  lv_obj_align(slider, LV_ALIGN_CENTER, 0, 140);
  return slider;
}

lv_obj_t* checkbox_create(lv_obj_t* parent, const char* label_text) {
  lv_obj_t* cb = lv_checkbox_create(parent);
  lv_checkbox_set_text(cb, label_text);
  lv_obj_align(cb, LV_ALIGN_CENTER, 0, 200);
  return cb;
}

// --- SETUP ---
void setup() {
  Serial.begin(115200);
  delay(1000);
  Serial.println("Starting responsive touch + USB keyboard test...");

  // --- Initialize USB keyboard ---
  HID.begin();
  Keyboard.begin();
  USB.begin();
  Serial.println("USB Keyboard initialized");

  // --- Initialize display and touch ---
  Screen.on();
  Touch.on();
  Serial.println("Display & Touch initialized");

  // --- Create LVGL UI ---
  lv_obj_t* scr = lv_scr_act();
  lv_obj_set_style_bg_color(scr, lv_color_hex(0x000000), 0);

  label = lv_label_create(scr);
  lv_label_set_text(label, "Touch the button!\nShould send 'HELLO' instantly");
  lv_obj_align(label, LV_ALIGN_CENTER, 0, -30);
  lv_obj_set_style_text_color(label, lv_color_hex(0xFFFFFF), 0);
  lv_obj_set_style_text_font(label, &lv_font_montserrat_16, 0);

  // --- Use modular UI creation ---
  button_create(scr, "PRESS ME", button_event_cb);
  slider_create(scr, 0, 100, 50);
  checkbox_create(scr, "Enable Feature");

  Serial.println("UI created — touch and USB keyboard ready");
}

// --- LOOP ---
void loop() {
  unsigned long current_millis = millis();

  if (current_millis - last_tick >= 5) {
    lv_tick_inc(5);
    last_tick = current_millis;
  }

  if (current_millis - last_debug >= 2000) {
    Serial.println("System running...");
    last_debug = current_millis;
  }

  lv_task_handler();
  delay(2);
}

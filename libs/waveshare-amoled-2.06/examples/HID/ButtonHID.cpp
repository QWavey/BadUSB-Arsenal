// This file types something when the button has been pressed
#include <Arduino.h>
#include "ESP32-S3-Screen-AMOLED-2.06.h"
#include "ESP32-S3-Touch-AMOLED-2.06.h"
#include <lvgl.h>
#include <USB.h>
#include <USBHIDKeyboard.h>

ScreenClass Screen;
TouchClass Touch;
USBHIDKeyboard Keyboard;
USBHID HID;

lv_obj_t* label;
unsigned long last_tick = 0;
unsigned long last_debug = 0;

// Forward declaration
void sendHello();

void setup() {
  Serial.begin(115200);
  delay(1000);
  Serial.println("Starting responsive touch + USB keyboard test...");

  // --- Initialize USB HID keyboard ---
  HID.begin();          // No argument needed
  Keyboard.begin();
  USB.begin();
  Serial.println("USB Keyboard initialized");

  // --- Initialize display ---
  Screen.on();
  Serial.println("Display initialized");

  // --- Initialize touch ---
  Touch.on();
  Serial.println("Touch initialized");

  // --- Create LVGL UI ---
  lv_obj_t* scr = lv_scr_act();
  lv_obj_set_style_bg_color(scr, lv_color_hex(0x000000), 0);

  // Status label
  label = lv_label_create(scr);
  lv_label_set_text(label, "Touch the button!\nShould send 'HELLO' instantly");
  lv_obj_align(label, LV_ALIGN_CENTER, 0, -30);
  lv_obj_set_style_text_color(label, lv_color_hex(0xFFFFFF), 0);
  lv_obj_set_style_text_font(label, &lv_font_montserrat_16, 0); // FIXED: readable font

  // Button setup
  lv_obj_t* btn = lv_btn_create(scr);
  lv_obj_set_size(btn, 160, 70);
  lv_obj_align(btn, LV_ALIGN_CENTER, 0, 60);
  lv_obj_set_style_bg_color(btn, lv_palette_main(LV_PALETTE_BLUE), 0);

  // --- Event callback for button press ---
  lv_obj_add_event_cb(btn, [](lv_event_t* e) {
    if (lv_event_get_code(e) == LV_EVENT_CLICKED) {
      lv_label_set_text(label, "Button pressed!\nTyping HELLO...");
      Serial.println("Button pressed — sending HELLO");

      // Change button color to red briefly
      lv_obj_set_style_bg_color((lv_obj_t*)lv_event_get_target(e),
                                lv_palette_main(LV_PALETTE_RED), 0);

      // --- Send text over USB keyboard ---
      sendHello();

      delay(200);
      lv_obj_set_style_bg_color((lv_obj_t*)lv_event_get_target(e),
                                lv_palette_main(LV_PALETTE_BLUE), 0);
    }
  }, LV_EVENT_CLICKED, NULL);

  // Button label
  lv_obj_t* btn_label = lv_label_create(btn);
  lv_label_set_text(btn_label, "PRESS ME");
  lv_obj_center(btn_label);
  lv_obj_set_style_text_color(btn_label, lv_color_white(), 0);
  lv_obj_set_style_text_font(btn_label, &lv_font_montserrat_16, 0); // FIXED: readable font

  Serial.println("UI created — touch and USB keyboard ready");
}

void loop() {
  unsigned long current_millis = millis();

  // LVGL tick (every 5ms)
  if (current_millis - last_tick >= 5) {
    lv_tick_inc(5);
    last_tick = current_millis;
  }

  // Debug (every 2s)
  if (current_millis - last_debug >= 2000) {
    Serial.println("System running...");
    last_debug = current_millis;
  }

  lv_task_handler();
  delay(2);
}

// --- Function to send "HELLO" over USB keyboard ---
void sendHello() {
  Keyboard.print("HELLO");
  Serial.println("Typed: HELLO");
}

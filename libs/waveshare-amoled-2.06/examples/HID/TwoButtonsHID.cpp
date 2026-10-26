// This file types something different, depends on the button which has been pressed
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
void sendhi();

void setup() {
  Serial.begin(115200);
  delay(1000);
  Serial.println("Starting responsive touch + USB keyboard HI!...");

  // --- Initialize LVGL ---
  lv_init();

  // --- Initialize USB HID keyboard ---
  HID.begin();
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
  lv_label_set_text(label, "Touch a button!\nShould send text instantly.");
  lv_obj_align(label, LV_ALIGN_CENTER, 0, -80);
  lv_obj_set_style_text_color(label, lv_color_hex(0xFFFFFF), 0);
  lv_obj_set_style_text_font(label, &lv_font_montserrat_16, 0);

  // --- HELLO button ---
  lv_obj_t* btn_hello = lv_btn_create(scr);
  lv_obj_set_size(btn_hello, 160, 70);
  lv_obj_align(btn_hello, LV_ALIGN_CENTER, -90, 40);
  lv_obj_set_style_bg_color(btn_hello, lv_palette_main(LV_PALETTE_BLUE), 0);

  lv_obj_t* btn_label_hello = lv_label_create(btn_hello);
  lv_label_set_text(btn_label_hello, "SEND HELLO");
  lv_obj_center(btn_label_hello);
  lv_obj_set_style_text_color(btn_label_hello, lv_color_white(), 0);
  lv_obj_set_style_text_font(btn_label_hello, &lv_font_montserrat_16, 0);

  lv_obj_add_event_cb(btn_hello, [](lv_event_t* e) {
    if (lv_event_get_code(e) == LV_EVENT_CLICKED) {
      lv_label_set_text(label, "Button pressed!\nTyping HELLO...");
      Serial.println("Button pressed — sending HELLO");

      lv_obj_t* btn = (lv_obj_t*)lv_event_get_target(e);
      lv_obj_set_style_bg_color(btn, lv_palette_main(LV_PALETTE_RED), 0);

      sendHello();

      delay(200);
      lv_obj_set_style_bg_color(btn, lv_palette_main(LV_PALETTE_BLUE), 0);
      lv_label_set_text(label, "Sent: HELLO");
    }
  }, LV_EVENT_CLICKED, NULL);

  // --- HI! button ---
  lv_obj_t* btn_test = lv_btn_create(scr);
  lv_obj_set_size(btn_test, 160, 70);
  lv_obj_align(btn_test, LV_ALIGN_CENTER, 90, 40);
  lv_obj_set_style_bg_color(btn_test, lv_palette_main(LV_PALETTE_GREEN), 0);

  lv_obj_t* btn_label_test = lv_label_create(btn_test);
  lv_label_set_text(btn_label_test, "SEND HI!");
  lv_obj_center(btn_label_test);
  lv_obj_set_style_text_color(btn_label_test, lv_color_white(), 0);
  lv_obj_set_style_text_font(btn_label_test, &lv_font_montserrat_16, 0);

  lv_obj_add_event_cb(btn_test, [](lv_event_t* e) {
    if (lv_event_get_code(e) == LV_EVENT_CLICKED) {
      lv_label_set_text(label, "Button pressed!\nTyping HI!...");
      Serial.println("Button pressed — sending HI!");

      lv_obj_t* btn = (lv_obj_t*)lv_event_get_target(e);
      lv_obj_set_style_bg_color(btn, lv_palette_main(LV_PALETTE_RED), 0);

      sendhi();

      delay(200);
      lv_obj_set_style_bg_color(btn, lv_palette_main(LV_PALETTE_GREEN), 0);
      lv_label_set_text(label, "Sent: HI!");
    }
  }, LV_EVENT_CLICKED, NULL);

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
  Keyboard.releaseAll();
  Serial.println("Typed: HELLO");
}

// --- Function to send "HI!" over USB keyboard ---
void sendhi() {
  Keyboard.print("Hi!");
  Keyboard.releaseAll();
  Serial.println("Typed: HI!");
}

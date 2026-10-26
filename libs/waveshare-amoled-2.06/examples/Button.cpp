#include <Arduino.h>
#include "ESP32-S3-Screen-AMOLED-2.06.h"
#include "ESP32-S3-Touch-AMOLED-2.06.h"

ScreenClass Screen;
TouchClass Touch;

lv_obj_t* label;
unsigned long last_tick = 0;
unsigned long last_debug = 0;

void setup() {
  Serial.begin(115200);
  delay(1000);
  Serial.println("Starting responsive touch test...");

  // Initialize display
  Screen.on();
  Serial.println("Display initialized");

  // Initialize touch
  Touch.on();
  Serial.println("Touch initialized");

  // Create simple UI
  lv_obj_t* scr = lv_scr_act();
  lv_obj_set_style_bg_color(scr, lv_color_hex(0x000000), 0);
  
  // Status label
  label = lv_label_create(scr);
  lv_label_set_text(label, "Touch the button!\nShould respond instantly");
  lv_obj_align(label, LV_ALIGN_CENTER, 0, -30);
  lv_obj_set_style_text_color(label, lv_color_hex(0xFFFFFF), 0);
  
  // Responsive button
  lv_obj_t* btn = lv_btn_create(scr);
  lv_obj_set_size(btn, 120, 60);
  lv_obj_align(btn, LV_ALIGN_CENTER, 0, 50);
  lv_obj_set_style_bg_color(btn, lv_palette_main(LV_PALETTE_BLUE), 0);
  
  lv_obj_add_event_cb(btn, [](lv_event_t* e) {
    if (lv_event_get_code(e) == LV_EVENT_CLICKED) {
      lv_label_set_text(label, "Button pressed!\nInstant response!");
      Serial.println("Button pressed - instant!");
      
      // Visual feedback
      lv_obj_set_style_bg_color((lv_obj_t*)lv_event_get_target(e), 
                               lv_palette_main(LV_PALETTE_RED), 0);
      
      // Reset color after 200ms
      delay(200);
      lv_obj_set_style_bg_color((lv_obj_t*)lv_event_get_target(e), 
                               lv_palette_main(LV_PALETTE_BLUE), 0);
    }
  }, LV_EVENT_CLICKED, NULL);
  
  lv_obj_t* btn_label = lv_label_create(btn);
  lv_label_set_text(btn_label, "PRESS ME");
  lv_obj_center(btn_label);
  lv_obj_set_style_text_color(btn_label, lv_color_white(), 0);

  Serial.println("Test UI created - touch should be instant now");
}

void loop() {
  // Better timing management
  unsigned long current_millis = millis();
  
  // LVGL tick (every 5ms)
  if (current_millis - last_tick >= 5) {
    lv_tick_inc(5);
    last_tick = current_millis;
  }
  
  // Debug output (every 2 seconds)
  if (current_millis - last_debug >= 2000) {
    Serial.println("System running...");
    last_debug = current_millis;
  }
  
  lv_task_handler();
  delay(2); // Reduced delay for better responsiveness
}

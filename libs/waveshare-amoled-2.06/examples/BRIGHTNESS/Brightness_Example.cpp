#include <Arduino.h>
#include "ESP32-S3-Screen-AMOLED-2.06 (1).h"
#include "ESP32-S3-Touch-AMOLED-2.06 (1).h"
#include "AMOLEDBrightness.h"

// Global instances
ScreenClass Screen;
TouchClass Touch;
AMOLEDBrightness Brightness;

// UI objects
lv_obj_t* statusLabel;
lv_obj_t* brightnessLabel;
lv_obj_t* touchLabel;
lv_obj_t* autoDimLabel;
lv_obj_t* instructionLabel;

// Variables
unsigned long lastUpdate = 0;
unsigned long lastTouchCheck = 0;
unsigned long lastActivityTime = 0;
unsigned long lastBrightnessUpdate = 0;
unsigned long statusResetTime = 0;
unsigned long lastCountdownUpdate = 0;
int touchCount = 0;
int32_t lastTouchX = 0;
int32_t lastTouchY = 0;
uint8_t currentBrightness = 75;
bool isDimmed = false;
char autoDimBuffer[50] = "Auto-dim in: 5 sec";  // FIXED: Pre-allocated buffer
char brightnessBuffer[30] = "Brightness: 75%";   // FIXED: Pre-allocated buffer
char touchBuffer[50] = "Touches: 0\nLast: (0, 0)"; // FIXED: Pre-allocated buffer

// Pre-defined strings to avoid format issues
const char* statusActive = "Status: Active";
const char* statusTouch = "Status: TOUCH DETECTED";
const char* instructionText = "Touch screen to brighten\nReset auto-dim timer";
const char* footerText = "Touch screen to test brightness";
const char* titleText = "Touch Brightness Demo";

// Previous values for comparison
char prevAutoDimBuffer[50] = "";
char prevBrightnessBuffer[30] = "";
char prevTouchBuffer[50] = "";

void setup() {
  Serial.begin(115200);
  delay(1000);
  Serial.println("=== AMOLED Brightness Demo ===");
  
  // Initialize display
  Screen.on();
  Serial.println("Display initialized");
  
  // Initialize touch
  Touch.on();
  Serial.println("Touch initialized");
  
  // Initialize brightness control
  Brightness.begin();
  Serial.println("Brightness control initialized");
  
  // Set initial brightness
  currentBrightness = 75;
  Brightness.setBrightness(currentBrightness);
  
  // Create main screen
  lv_obj_t* scr = lv_scr_act();
  lv_obj_set_style_bg_color(scr, lv_color_hex(0x000000), 0);
  
  // Create title
  lv_obj_t* title = lv_label_create(scr);
  lv_label_set_text(title, titleText);
  lv_obj_set_style_text_color(title, lv_color_hex(0xFFFFFF), 0);
  lv_obj_set_style_text_font(title, &lv_font_montserrat_20, 0);
  lv_obj_set_style_text_align(title, LV_TEXT_ALIGN_CENTER, 0);
  lv_obj_align(title, LV_ALIGN_TOP_MID, 0, 30);
  
  // Create instructions
  instructionLabel = lv_label_create(scr);
  lv_label_set_text(instructionLabel, instructionText);
  lv_obj_set_style_text_color(instructionLabel, lv_color_hex(0xAAAAAA), 0);
  lv_obj_set_style_text_align(instructionLabel, LV_TEXT_ALIGN_CENTER, 0);
  lv_obj_align(instructionLabel, LV_ALIGN_TOP_MID, 0, 70);
  
  // Create status label - FIXED: Set fixed width
  statusLabel = lv_label_create(scr);
  lv_obj_set_width(statusLabel, 250); // FIX: Set fixed width
  lv_label_set_text(statusLabel, statusActive);
  lv_obj_set_style_text_color(statusLabel, lv_color_hex(0x00FF00), 0);
  lv_obj_set_style_text_align(statusLabel, LV_TEXT_ALIGN_CENTER, 0);
  lv_obj_align(statusLabel, LV_ALIGN_TOP_MID, 0, 120);
  
  // Create brightness label - FIXED: Set fixed width
  brightnessLabel = lv_label_create(scr);
  lv_obj_set_width(brightnessLabel, 200); // FIX: Set fixed width
  lv_label_set_text(brightnessLabel, brightnessBuffer);
  lv_obj_set_style_text_color(brightnessLabel, lv_color_hex(0xFFFF00), 0);
  lv_obj_set_style_text_align(brightnessLabel, LV_TEXT_ALIGN_CENTER, 0);
  lv_obj_align(brightnessLabel, LV_ALIGN_TOP_MID, 0, 150);
  
  // Create auto-dim label - FIXED: Set fixed width
  autoDimLabel = lv_label_create(scr);
  lv_obj_set_width(autoDimLabel, 200); // FIX: Set fixed width to prevent distortion
  lv_label_set_text(autoDimLabel, autoDimBuffer);
  lv_obj_set_style_text_color(autoDimLabel, lv_color_hex(0xFF8800), 0);
  lv_obj_set_style_text_align(autoDimLabel, LV_TEXT_ALIGN_CENTER, 0);
  lv_obj_align(autoDimLabel, LV_ALIGN_TOP_MID, 0, 180);
  
  // Create touch label - FIXED: Set fixed width
  touchLabel = lv_label_create(scr);
  lv_obj_set_width(touchLabel, 200); // FIX: Set fixed width
  lv_label_set_text(touchLabel, touchBuffer);
  lv_obj_set_style_text_color(touchLabel, lv_color_hex(0x8888FF), 0);
  lv_obj_set_style_text_align(touchLabel, LV_TEXT_ALIGN_CENTER, 0);
  lv_obj_align(touchLabel, LV_ALIGN_BOTTOM_MID, 0, -30);
  
  // Create footer
  lv_obj_t* footer = lv_label_create(scr);
  lv_label_set_text(footer, footerText);
  lv_obj_set_style_text_color(footer, lv_color_hex(0x666666), 0);
  lv_obj_set_style_text_align(footer, LV_TEXT_ALIGN_CENTER, 0);
  lv_obj_align(footer, LV_ALIGN_BOTTOM_MID, 0, -5);
  
  // Initialize previous buffers
  strcpy(prevAutoDimBuffer, autoDimBuffer);
  strcpy(prevBrightnessBuffer, brightnessBuffer);
  strcpy(prevTouchBuffer, touchBuffer);
  
  // Start with auto-dim enabled (dim after 5 seconds)
  Brightness.setAutoDim(5000, 20);
  lastActivityTime = millis();
  
  Serial.println("Demo ready!");
}

void loop() {
  unsigned long currentMillis = millis();
  
  // Update LVGL tick every 5ms
  if (currentMillis - lastUpdate >= 5) {
    lv_tick_inc(5);
    lastUpdate = currentMillis;
  }
  
  // Check for touch every 50ms
  if (currentMillis - lastTouchCheck >= 50) {
    checkForTouch();
    lastTouchCheck = currentMillis;
  }
  
  // Update brightness display every 100ms
  if (currentMillis - lastBrightnessUpdate >= 100) {
    updateBrightnessDisplay();
    lastBrightnessUpdate = currentMillis;
  }
  
  // Update auto-dim display every 250ms
  if (currentMillis - lastCountdownUpdate >= 250) {
    updateAutoDimDisplay(currentMillis);
    lastCountdownUpdate = currentMillis;
  }
  
  // Reset status message if needed
  if (statusResetTime > 0 && currentMillis > statusResetTime) {
    lv_label_set_text(statusLabel, statusActive);
    lv_obj_set_style_text_color(statusLabel, lv_color_hex(0x00FF00), 0);
    statusResetTime = 0;
  }
  
  // Handle LVGL tasks
  lv_task_handler();
  
  delay(2);
}

void checkForTouch() {
  int32_t x, y;
  Touch.getTouchPoint(x, y);
  
  // Check if screen was touched
  if (x > 10 && y > 10 && x < 4000 && y < 4000 && 
      (x != lastTouchX || y != lastTouchY)) {
    
    touchCount++;
    lastTouchX = x;
    lastTouchY = y;
    lastActivityTime = millis();
    
    // Update touch display - FIXED: Only update if changed
    snprintf(touchBuffer, sizeof(touchBuffer), "Touches: %d\nLast: (%d, %d)", 
             touchCount, x, y);
    
    if (strcmp(touchBuffer, prevTouchBuffer) != 0) {
      lv_label_set_text(touchLabel, touchBuffer);
      strcpy(prevTouchBuffer, touchBuffer);
    }
    
    // Update status
    lv_label_set_text(statusLabel, statusTouch);
    lv_obj_set_style_text_color(statusLabel, lv_color_hex(0x00AAFF), 0);
    statusResetTime = millis() + 1000;
    
    // If screen is dimmed, brighten it immediately
    if (isDimmed || Brightness.getBrightness() < 50) {
      Brightness.fadeTo(75, 300);
      isDimmed = false;
      Serial.println("Screen brightened by touch");
    } else {
      // Reset auto-dim timer
      Brightness.setAutoDim(5000, 20);
      Serial.println("Touch detected - auto-dim timer reset");
    }
  }
}

void updateBrightnessDisplay() {
  uint8_t newBrightness = Brightness.getBrightness();
  
  if (newBrightness != currentBrightness) {
    currentBrightness = newBrightness;
    
    // Update brightness label - FIXED: Only update if changed
    snprintf(brightnessBuffer, sizeof(brightnessBuffer), "Brightness: %d%%", currentBrightness);
    
    if (strcmp(brightnessBuffer, prevBrightnessBuffer) != 0) {
      lv_label_set_text(brightnessLabel, brightnessBuffer);
      strcpy(prevBrightnessBuffer, brightnessBuffer);
    }
    
    // Update color based on brightness
    lv_color_t newColor;
    if (currentBrightness > 70) {
      newColor = lv_color_hex(0xFFFFFF);
    } else if (currentBrightness > 40) {
      newColor = lv_color_hex(0xFFFF00);
    } else {
      newColor = lv_color_hex(0xFF8800);
    }
    
    lv_obj_set_style_text_color(brightnessLabel, newColor, 0);
  }
}

void updateAutoDimDisplay(unsigned long currentMillis) {
  unsigned long timeSinceLastTouch = currentMillis - lastActivityTime;
  unsigned long timeUntilDim = 5000 - timeSinceLastTouch;
  
  // Update auto-dim label - FIXED: Always use the same buffer size
  char newAutoDimBuffer[50];
  lv_color_t newColor;
  
  if (timeUntilDim > 5000) {
    // Active state - FIXED: Use same format as other states
    snprintf(newAutoDimBuffer, sizeof(newAutoDimBuffer), "Auto-dim: Active");
    newColor = lv_color_hex(0x00FF00);
    isDimmed = false;
  } else if (timeUntilDim > 0) {
    int seconds = (timeUntilDim + 999) / 1000;
    snprintf(newAutoDimBuffer, sizeof(newAutoDimBuffer), "Auto-dim in: %d sec", seconds);
    
    // Change color based on urgency
    if (timeUntilDim < 2000) {
      newColor = lv_color_hex(0xFF0000);
    } else if (timeUntilDim < 4000) {
      newColor = lv_color_hex(0xFF8800);
    } else {
      newColor = lv_color_hex(0xFFFF00);
    }
    isDimmed = false;
  } else {
    snprintf(newAutoDimBuffer, sizeof(newAutoDimBuffer), "Screen dimmed");
    newColor = lv_color_hex(0x888888);
    isDimmed = true;
  }
  
  // FIXED: Only update if the text or color has changed
  bool textChanged = strcmp(newAutoDimBuffer, prevAutoDimBuffer) != 0;
  
  if (textChanged) {
    strcpy(autoDimBuffer, newAutoDimBuffer);
    strcpy(prevAutoDimBuffer, newAutoDimBuffer);
    lv_label_set_text(autoDimLabel, autoDimBuffer);
  }
  
  lv_obj_set_style_text_color(autoDimLabel, newColor, 0);
}

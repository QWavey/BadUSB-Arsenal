#include <Arduino.h>
#include <lvgl.h>
#include <WiFi.h>
#include <time.h>
#include <Preferences.h>
#include <USB.h>
#include <USBHIDKeyboard.h>
#include <ArduinoJson.h>
#include "ESP32-S3-Screen-AMOLED-2.06.h"
#include "ESP32-S3-Touch-AMOLED-2.06.h"
#include "SensorPCF85063.hpp"
#include "SDMounter.h"
#include "pin_config.h"

// Hardware objects
ScreenClass Screen;
TouchClass Touch;
SensorPCF85063 rtc;
Preferences preferences;
USBHIDKeyboard Keyboard;
USBHID HID;

// UI objects for watch mode
lv_obj_t *time_label;
lv_obj_t *date_label;
lv_obj_t *status_label;
lv_obj_t *wifi_btn;

// UI objects for menu mode
lv_obj_t *menu_screen = nullptr;
lv_obj_t *menu_container = nullptr;
std::vector<String> script_files;

// State management
enum AppMode { MODE_WATCH, MODE_MENU };
AppMode current_mode = MODE_WATCH;
bool boot_button_pressed = false;
unsigned long boot_button_press_time = 0;
const unsigned long DEBOUNCE_DELAY = 200;

// DuckyScript execution state
struct DuckyState {
  uint32_t default_delay = 0;
  uint32_t repeat_count = 0;
  String last_command = "";
  String current_locale = "US";
  StaticJsonDocument<8192> layout_map;
  bool layout_loaded = false;
};
DuckyState ducky_state;

// Timing
unsigned long last_tick = 0;
unsigned long last_time_update = 0;

// WiFi/NTP settings
const char* ssid = "Hotspot";
const char* password = "987654321";
const char* ntpServer = "pool.ntp.org";

// Forward declarations
void setupWatchUI();
void setupMenuUI();
void switchMode(AppMode new_mode);
void scanSDCardForScripts();
void executeScript(const String& filename);
bool loadKeyboardLayout(const String& locale);
void parseDuckyCommand(const String& line);
void sendKeyCombo(const String& keys);
void sendString(const String& text);
void sendKeycode(uint8_t modifier, uint8_t keycode);
bool getKeycodeFromLayout(const char c, uint8_t& modifier, uint8_t& keycode);
bool getKeycodeFromString(const String& key, uint8_t& modifier, uint8_t& keycode);
void saveTimeToFlash();
bool loadTimeFromFlash();
bool syncTimeWithNTP_blocking();

uint32_t millis_cb() { return millis(); }

void IRAM_ATTR bootButtonISR() {
  unsigned long current = millis();
  if (current - boot_button_press_time > DEBOUNCE_DELAY) {
    boot_button_pressed = true;
    boot_button_press_time = current;
  }
}

void saveTimeToFlash() {
  RTC_DateTime dt = rtc.getDateTime();
  preferences.begin("clock", false);
  preferences.putUInt("year", dt.getYear());
  preferences.putUInt("month", dt.getMonth());
  preferences.putUInt("day", dt.getDay());
  preferences.putUInt("hour", dt.getHour());
  preferences.putUInt("minute", dt.getMinute());
  preferences.putUInt("second", dt.getSecond());
  preferences.end();
  Serial.println("Time saved to flash");
}

bool loadTimeFromFlash() {
  preferences.begin("clock", true);
  if (!preferences.isKey("year")) {
    preferences.end();
    Serial.println("No saved time in flash");
    return false;
  }
  uint16_t year = preferences.getUInt("year", 2024);
  uint8_t month = preferences.getUInt("month", 1);
  uint8_t day = preferences.getUInt("day", 1);
  uint8_t hour = preferences.getUInt("hour", 0);
  uint8_t minute = preferences.getUInt("minute", 0);
  uint8_t second = preferences.getUInt("second", 0);
  preferences.end();
  rtc.setDateTime(year, month, day, hour, minute, second);
  Serial.printf("Loaded time from flash: %02d:%02d:%02d %02d-%02d-%04d\n",
                hour, minute, second, day, month, year);
  return true;
}

bool syncTimeWithNTP_blocking() {
  lv_label_set_text(status_label, "Starting WiFi...");
  lv_task_handler();
  Serial.println("=== Starting WiFi sync ===");
  
  WiFi.disconnect(true);
  delay(200);
  WiFi.mode(WIFI_STA);
  delay(200);
  Serial.printf("Connecting to SSID: %s\n", ssid);
  WiFi.begin(ssid, password);
  
  unsigned long start = millis();
  const unsigned long wifiTimeout = 45000UL;
  
  while (WiFi.status() != WL_CONNECTED) {
    Serial.print(".");
    lv_label_set_text(status_label, "Connecting WiFi...");
    lv_task_handler();
    delay(500);
    if (millis() - start > wifiTimeout) {
      Serial.println("\nWiFi connect TIMEOUT");
      lv_label_set_text(status_label, "WiFi Timeout");
      lv_task_handler();
      WiFi.disconnect(true);
      WiFi.mode(WIFI_OFF);
      return false;
    }
  }
  
  Serial.println("\nWiFi CONNECTED");
  char buf[64];
  snprintf(buf, sizeof(buf), "IP: %s", WiFi.localIP().toString().c_str());
  lv_label_set_text(status_label, buf);
  lv_task_handler();
  
  lv_label_set_text(status_label, "Configuring NTP...");
  lv_task_handler();
  configTime(0, 0, ntpServer);
  
  struct tm timeinfo;
  bool gotTime = false;
  unsigned long ntpStart = millis();
  const unsigned long ntpTimeout = 15000UL;
  
  while (millis() - ntpStart < ntpTimeout) {
    if (getLocalTime(&timeinfo, 2000)) {
      gotTime = true;
      break;
    }
    Serial.print(".");
    lv_label_set_text(status_label, "Waiting NTP...");
    lv_task_handler();
  }
  
  if (!gotTime) {
    Serial.println("\nNTP failed");
    lv_label_set_text(status_label, "NTP Failed");
    lv_task_handler();
    WiFi.disconnect(true);
    WiFi.mode(WIFI_OFF);
    return false;
  }
  
  uint16_t year = timeinfo.tm_year + 1900;
  uint8_t month = timeinfo.tm_mon + 1;
  uint8_t day = timeinfo.tm_mday;
  uint8_t hour = timeinfo.tm_hour;
  uint8_t minute = timeinfo.tm_min;
  uint8_t second = timeinfo.tm_sec;
  
  rtc.setDateTime(year, month, day, hour, minute, second);
  saveTimeToFlash();
  
  Serial.printf("\nTime synced: %02d:%02d:%02d %02d-%02d-%04d\n",
                hour, minute, second, day, month, year);
  lv_label_set_text(status_label, "Time Synced");
  lv_task_handler();
  
  delay(200);
  WiFi.disconnect(true);
  WiFi.mode(WIFI_OFF);
  return true;
}

static void wifi_sync_event(lv_event_t* e) {
  if (lv_event_get_code(e) != LV_EVENT_CLICKED) return;
  
  lv_obj_add_state(wifi_btn, LV_STATE_DISABLED);
  lv_label_set_text(status_label, "Starting sync...");
  lv_task_handler();
  
  bool ok = syncTimeWithNTP_blocking();
  
  lv_obj_clear_state(wifi_btn, LV_STATE_DISABLED);
  
  if (ok) {
    lv_label_set_text(status_label, "Sync OK");
    lv_task_handler();
    lv_obj_set_style_bg_color(wifi_btn, lv_color_hex(0x00AA00), 0);
    delay(700);
    lv_obj_set_style_bg_color(wifi_btn, lv_palette_main(LV_PALETTE_BLUE), 0);
  } else {
    lv_label_set_text(status_label, "Sync Failed");
    lv_task_handler();
    lv_obj_set_style_bg_color(wifi_btn, lv_color_hex(0xAA0000), 0);
    delay(700);
    lv_obj_set_style_bg_color(wifi_btn, lv_palette_main(LV_PALETTE_BLUE), 0);
  }
}

void setupWatchUI() {
  lv_obj_t* scr = lv_scr_act();
  lv_obj_clean(scr);
  lv_obj_set_style_bg_color(scr, lv_color_hex(0x000000), 0);
  lv_obj_set_style_bg_opa(scr, LV_OPA_COVER, 0);
  
  time_label = lv_label_create(scr);
  lv_label_set_text(time_label, "00:00:00");
  lv_obj_align(time_label, LV_ALIGN_CENTER, 0, -50);
  lv_obj_set_style_text_color(time_label, lv_color_hex(0xFFFFFF), 0);
  lv_obj_set_style_text_font(time_label, &lv_font_montserrat_32, 0);
  
  date_label = lv_label_create(scr);
  lv_label_set_text(date_label, "01-01-2024");
  lv_obj_align(date_label, LV_ALIGN_CENTER, 0, 0);
  lv_obj_set_style_text_color(date_label, lv_color_hex(0xAAAAAA), 0);
  lv_obj_set_style_text_font(date_label, &lv_font_montserrat_24, 0);
  
  status_label = lv_label_create(scr);
  lv_label_set_text(status_label, "");
  lv_obj_align(status_label, LV_ALIGN_BOTTOM_MID, 0, -60);
  lv_obj_set_style_text_color(status_label, lv_color_hex(0x00FF00), 0);
  lv_obj_set_style_text_font(status_label, &lv_font_montserrat_16, 0);
  
  wifi_btn = lv_btn_create(scr);
  lv_obj_set_size(wifi_btn, 140, 50);
  lv_obj_align(wifi_btn, LV_ALIGN_BOTTOM_MID, 0, -10);
  lv_obj_set_style_bg_color(wifi_btn, lv_palette_main(LV_PALETTE_BLUE), 0);
  lv_obj_add_event_cb(wifi_btn, wifi_sync_event, LV_EVENT_CLICKED, NULL);
  
  lv_obj_t* btn_label = lv_label_create(wifi_btn);
  lv_label_set_text(btn_label, "Sync Time");
  lv_obj_center(btn_label);
  lv_obj_set_style_text_color(btn_label, lv_color_white(), 0);
  
  lv_label_set_text(status_label, "Loading time...");
  lv_task_handler();
  
  if (loadTimeFromFlash()) {
    RTC_DateTime dt = rtc.getDateTime();
    char time_buf[12];
    char date_buf[16];
    snprintf(time_buf, sizeof(time_buf), "%02d:%02d:%02d",
             dt.getHour(), dt.getMinute(), dt.getSecond());
    snprintf(date_buf, sizeof(date_buf), "%02d-%02d-%04d",
             dt.getDay(), dt.getMonth(), dt.getYear());
    lv_label_set_text(time_label, time_buf);
    lv_label_set_text(date_label, date_buf);
    lv_label_set_text(status_label, "Press BOOT for scripts");
  } else {
    lv_label_set_text(status_label, "Press Sync or BOOT");
  }
  
  lv_task_handler();
}

static void script_button_event(lv_event_t* e) {
  if (lv_event_get_code(e) != LV_EVENT_CLICKED) return;
  
  int idx = (int)(intptr_t)lv_event_get_user_data(e);
  if (idx >= 0 && idx < script_files.size()) {
    Serial.printf("Executing script: %s\n", script_files[idx].c_str());
    executeScript(script_files[idx]);
  }
}

void setupMenuUI() {
  lv_obj_t* scr = lv_scr_act();
  lv_obj_clean(scr);
  lv_obj_set_style_bg_color(scr, lv_color_hex(0x000000), 0);
  
  lv_obj_t* title = lv_label_create(scr);
  lv_label_set_text(title, "RubberDucky Scripts");
  lv_obj_align(title, LV_ALIGN_TOP_MID, 0, 10);
  lv_obj_set_style_text_color(title, lv_color_hex(0xFFFFFF), 0);
  lv_obj_set_style_text_font(title, &lv_font_montserrat_20, 0);
  
  lv_obj_t* subtitle = lv_label_create(scr);
  lv_label_set_text(subtitle, "Press BOOT to return");
  lv_obj_align(subtitle, LV_ALIGN_TOP_MID, 0, 40);
  lv_obj_set_style_text_color(subtitle, lv_color_hex(0x888888), 0);
  
  menu_container = lv_obj_create(scr);
  lv_obj_set_size(menu_container, LCD_WIDTH - 20, LCD_HEIGHT - 100);
  lv_obj_align(menu_container, LV_ALIGN_CENTER, 0, 20);
  lv_obj_set_style_bg_color(menu_container, lv_color_hex(0x000000), 0);
  lv_obj_set_style_border_width(menu_container, 0, 0);
  lv_obj_set_flex_flow(menu_container, LV_FLEX_FLOW_COLUMN);
  lv_obj_set_flex_align(menu_container, LV_FLEX_ALIGN_START, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);
  lv_obj_set_scrollbar_mode(menu_container, LV_SCROLLBAR_MODE_AUTO);
  
  scanSDCardForScripts();
  
  if (script_files.empty()) {
    lv_obj_t* no_files = lv_label_create(menu_container);
    lv_label_set_text(no_files, "No .txt files found\non SD card");
    lv_obj_set_style_text_color(no_files, lv_color_hex(0xFF0000), 0);
    lv_obj_set_style_text_align(no_files, LV_TEXT_ALIGN_CENTER, 0);
  } else {
    for (int i = 0; i < script_files.size(); i++) {
      lv_obj_t* btn = lv_btn_create(menu_container);
      lv_obj_set_size(btn, LCD_WIDTH - 60, 50);
      lv_obj_set_style_bg_color(btn, lv_palette_main(LV_PALETTE_GREEN), 0);
      lv_obj_add_event_cb(btn, script_button_event, LV_EVENT_CLICKED, (void*)(intptr_t)i);
      
      lv_obj_t* label = lv_label_create(btn);
      String displayName = script_files[i];
      if (displayName.endsWith(".txt")) {
        displayName = displayName.substring(0, displayName.length() - 4);
      }
      if (displayName.length() > 20) {
        displayName = displayName.substring(0, 20) + "...";
      }
      lv_label_set_text(label, displayName.c_str());
      lv_obj_center(label);
      lv_obj_set_style_text_color(label, lv_color_white(), 0);
    }
  }
  
  lv_task_handler();
}

void switchMode(AppMode new_mode) {
  current_mode = new_mode;
  if (new_mode == MODE_WATCH) {
    Serial.println("Switching to WATCH mode");
    setupWatchUI();
  } else {
    Serial.println("Switching to MENU mode");
    setupMenuUI();
  }
}

void scanSDCardForScripts() {
  script_files.clear();
  
  if (!SDCard.isMounted()) {
    Serial.println("SD card not mounted, attempting mount...");
    if (!SDCard.mount(false, "/sdcard", true)) {
      Serial.println("Failed to mount SD card");
      return;
    }
  }
  
  std::vector<String> items = SDCard.listDirVector("/");
  for (const String& item : items) {
    if (item.endsWith(".txt")) {
      script_files.push_back(item);
      Serial.printf("Found script: %s\n", item.c_str());
    }
  }
  
  Serial.printf("Total scripts found: %d\n", script_files.size());
}

bool loadKeyboardLayout(const String& locale) {
  String layout_path = "/languages/" + locale + ".json";
  
  if (!SDCard.existsFile(layout_path.c_str())) {
    Serial.printf("Layout file not found: %s\n", layout_path.c_str());
    return false;
  }
  
  String json_content = SDCard.readFile(layout_path.c_str());
  if (json_content.length() == 0) {
    Serial.println("Failed to read layout file");
    return false;
  }
  
  ducky_state.layout_map.clear();
  DeserializationError error = deserializeJson(ducky_state.layout_map, json_content);
  
  if (error) {
    Serial.printf("Failed to parse layout JSON: %s\n", error.c_str());
    return false;
  }
  
  ducky_state.current_locale = locale;
  ducky_state.layout_loaded = true;
  Serial.printf("Loaded keyboard layout: %s\n", locale.c_str());
  return true;
}

bool getKeycodeFromLayout(const char c, uint8_t& modifier, uint8_t& keycode) {
  if (!ducky_state.layout_loaded) {
    return false;
  }
  
  String key_str = String(c);
  if (!ducky_state.layout_map.containsKey(key_str)) {
    return false;
  }
  
  String hex_str = ducky_state.layout_map[key_str].as<String>();
  int comma1 = hex_str.indexOf(',');
  int comma2 = hex_str.indexOf(',', comma1 + 1);
  
  if (comma1 == -1 || comma2 == -1) {
    return false;
  }
  
  modifier = strtol(hex_str.substring(0, comma1).c_str(), NULL, 16);
  keycode = strtol(hex_str.substring(comma2 + 1).c_str(), NULL, 16);
  
  return true;
}

bool getKeycodeFromString(const String& key, uint8_t& modifier, uint8_t& keycode) {
  if (!ducky_state.layout_loaded) {
    return false;
  }
  
  if (!ducky_state.layout_map.containsKey(key)) {
    return false;
  }
  
  String hex_str = ducky_state.layout_map[key].as<String>();
  int comma1 = hex_str.indexOf(',');
  int comma2 = hex_str.indexOf(',', comma1 + 1);
  
  if (comma1 == -1 || comma2 == -1) {
    return false;
  }
  
  modifier = strtol(hex_str.substring(0, comma1).c_str(), NULL, 16);
  keycode = strtol(hex_str.substring(comma2 + 1).c_str(), NULL, 16);
  
  return true;
}

void sendKeycode(uint8_t modifier, uint8_t keycode) {
  uint8_t report[8] = {modifier, 0, keycode, 0, 0, 0, 0, 0};
  Keyboard.sendReport((KeyReport*)report);
  delay(10);
  uint8_t release[8] = {0, 0, 0, 0, 0, 0, 0, 0};
  Keyboard.sendReport((KeyReport*)release);
  delay(10);
}

void sendString(const String& text) {
  for (int i = 0; i < text.length(); i++) {
    char c = text.charAt(i);
    uint8_t modifier, keycode;
    
    if (getKeycodeFromLayout(c, modifier, keycode)) {
      sendKeycode(modifier, keycode);
    } else {
      Serial.printf("Warning: No mapping for character '%c'\n", c);
    }
    
    if (ducky_state.default_delay > 0) {
      delay(ducky_state.default_delay);
    }
  }
}

void sendKeyCombo(const String& keys) {
  String upper_keys = keys;
  upper_keys.toUpperCase();
  
  uint8_t modifier = 0;
  uint8_t keycode = 0;
  
  if (getKeycodeFromString(upper_keys, modifier, keycode)) {
    sendKeycode(modifier, keycode);
    return;
  }
  
  int plus_pos = upper_keys.indexOf('+');
  if (plus_pos == -1) {
    plus_pos = upper_keys.indexOf('-');
  }
  
  if (plus_pos != -1) {
    String mod_part = upper_keys.substring(0, plus_pos);
    String key_part = upper_keys.substring(plus_pos + 1);
    mod_part.trim();
    key_part.trim();
    
    uint8_t mod1, kc1, mod2, kc2;
    bool has_mod = getKeycodeFromString(mod_part, mod1, kc1);
    bool has_key = getKeycodeFromString(key_part, mod2, kc2);
    
    if (has_mod && has_key) {
      sendKeycode(mod1 | mod2, kc2);
      return;
    }
  }
  
  Serial.printf("Warning: Could not parse key combo: %s\n", keys.c_str());
}

void parseDuckyCommand(const String& line) {
  String trimmed = line;
  trimmed.trim();
  
  if (trimmed.length() == 0 || trimmed.startsWith("REM")) {
    return;
  }
  
  int space_pos = trimmed.indexOf(' ');
  String cmd = (space_pos == -1) ? trimmed : trimmed.substring(0, space_pos);
  String args = (space_pos == -1) ? "" : trimmed.substring(space_pos + 1);
  cmd.trim();
  args.trim();
  
  if (cmd.equalsIgnoreCase("DELAY")) {
    uint32_t delay_ms = args.toInt();
    delay(delay_ms);
  }
  else if (cmd.equalsIgnoreCase("DEFAULTDELAY")) {
    ducky_state.default_delay = args.toInt();
  }
  else if (cmd.equalsIgnoreCase("STRING")) {
    sendString(args);
    ducky_state.last_command = line;
  }
  else if (cmd.equalsIgnoreCase("REPEAT")) {
    uint32_t count = args.toInt();
    for (uint32_t i = 0; i < count; i++) {
      parseDuckyCommand(ducky_state.last_command);
    }
  }
  else if (cmd.equalsIgnoreCase("LOCALE")) {
    loadKeyboardLayout(args);
  }
  else if (cmd.equalsIgnoreCase("KEYCODE")) {
    int first_space = args.indexOf(' ');
    if (first_space != -1) {
      String mod_hex = args.substring(0, first_space);
      String key_hex = args.substring(first_space + 1);
      uint8_t modifier = strtol(mod_hex.c_str(), NULL, 16);
      uint8_t keycode = strtol(key_hex.c_str(), NULL, 16);
      sendKeycode(modifier, keycode);
    }
  }
  else if (cmd.equalsIgnoreCase("ENTER")) {
    sendKeyCombo("ENTER");
    ducky_state.last_command = line;
  }
  else if (cmd.equalsIgnoreCase("TAB")) {
    sendKeyCombo("TAB");
    ducky_state.last_command = line;
  }
  else if (cmd.equalsIgnoreCase("ESC") || cmd.equalsIgnoreCase("ESCAPE")) {
    sendKeyCombo("ESC");
    ducky_state.last_command = line;
  }
  else if (cmd.equalsIgnoreCase("DELETE") || cmd.equalsIgnoreCase("DEL")) {
    sendKeyCombo("DELETE");
    ducky_state.last_command = line;
  }
  else if (cmd.equalsIgnoreCase("UP") || cmd.equalsIgnoreCase("UPARROW")) {
    sendKeyCombo("UP");
    ducky_state.last_command = line;
  }
  else if (cmd.equalsIgnoreCase("DOWN") || cmd.equalsIgnoreCase("DOWNARROW")) {
    sendKeyCombo("DOWN");
    ducky_state.last_command = line;
  }
  else if (cmd.equalsIgnoreCase("LEFT") || cmd.equalsIgnoreCase("LEFTARROW")) {
    sendKeyCombo("LEFT");
    ducky_state.last_command = line;
  }
  else if (cmd.equalsIgnoreCase("RIGHT") || cmd.equalsIgnoreCase("RIGHTARROW")) {
    sendKeyCombo("RIGHT");
    ducky_state.last_command = line;
  }
  else if (cmd.equalsIgnoreCase("CAPSLOCK")) {
    sendKeyCombo("CAPSLOCK");
    ducky_state.last_command = line;
  }
  else if (cmd.equalsIgnoreCase("NUMLOCK")) {
    sendKeyCombo("NUMLOCK");
    ducky_state.last_command = line;
  }
  else if (cmd.equalsIgnoreCase("PRINTSCREEN")) {
    sendKeyCombo("PRINTSCREEN");
    ducky_state.last_command = line;
  }
  else if (cmd.equalsIgnoreCase("SPACE")) {
    sendKeyCombo("SPACE");
    ducky_state.last_command = line;
  }
  else {
    sendKeyCombo(trimmed);
    ducky_state.last_command = line;
  }
  
  if (ducky_state.default_delay > 0) {
    delay(ducky_state.default_delay);
  }
}

void executeScript(const String& filename) {
  String script_path = "/" + filename;
  
  if (!SDCard.existsFile(script_path.c_str())) {
    Serial.printf("Script not found: %s\n", script_path.c_str());
    return;
  }
  
  Serial.printf("Executing script: %s\n", filename.c_str());
  
  if (!ducky_state.layout_loaded) {
    loadKeyboardLayout("US");
  }
  
  String script_content = SDCard.readFile(script_path.c_str());
  if (script_content.length() == 0) {
    Serial.println("Script is empty or failed to read");
    return;
  }
  
  ducky_state.default_delay = 0;
  ducky_state.last_command = "";
  
  int line_start = 0;
  int line_end = 0;
  
  while (line_start < script_content.length()) {
    line_end = script_content.indexOf('\n', line_start);
    if (line_end == -1) {
      line_end = script_content.length();
    }
    
    String line = script_content.substring(line_start, line_end);
    line.trim();
    
    if (line.length() > 0) {
      parseDuckyCommand(line);
    }
    
    line_start = line_end + 1;
  }
  
  Serial.println("Script execution completed");
}

void setup() {
  Serial.begin(115200);
  delay(1500);
  Serial.println("\n=== ESP32-S3 RubberDucky ===");
  
  pinMode(0, INPUT_PULLUP);
  attachInterrupt(0, bootButtonISR, FALLING);
  
  HID.begin();
  Keyboard.begin();
  USB.begin();
  Serial.println("USB HID Keyboard initialized");
  
  Screen.on();
  Serial.println("Display initialized");
  
  Touch.on();
  Serial.println("Touch initialized");
  
  if (!rtc.begin(Wire, IIC_SDA, IIC_SCL)) {
    Serial.println("ERROR: PCF85063 not found!");
    lv_obj_t* err = lv_label_create(lv_scr_act());
    lv_label_set_text(err, "RTC ERROR");
    lv_obj_align(err, LV_ALIGN_CENTER, 0, 0);
    lv_obj_set_style_text_color(err, lv_color_hex(0xFF0000), 0);
    while (1) delay(1000);
  }
  
  lv_tick_set_cb(millis_cb);
  
  Serial.println("Mounting SD card...");
  if (!SDCard.mount(false, "/sdcard", true)) {
    Serial.println("Failed to mount SD card");
  } else {
    Serial.println("SD card mounted successfully");
    SDCard.dumpFsInfo();
  }
  
  setupWatchUI();
  
  Serial.println("Setup complete. Press BOOT button to access scripts.");
}

void loop() {
  unsigned long current_millis = millis();
  
  if (current_millis - last_tick >= 5) {
    lv_tick_inc(5);
    last_tick = current_millis;
  }
  
  if (boot_button_pressed) {
    boot_button_pressed = false;
    Serial.println("BOOT button pressed");
    
    if (current_mode == MODE_WATCH) {
      switchMode(MODE_MENU);
    } else {
      switchMode(MODE_WATCH);
    }
  }
  
  if (current_mode == MODE_WATCH) {
    if (current_millis - last_time_update >= 1000) {
      last_time_update = current_millis;
      RTC_DateTime dt = rtc.getDateTime();
      char time_buf[16], date_buf[20];
      snprintf(time_buf, sizeof(time_buf), "%02d:%02d:%02d", 
               dt.getHour(), dt.getMinute(), dt.getSecond());
      snprintf(date_buf, sizeof(date_buf), "%02d-%02d-%04d", 
               dt.getDay(), dt.getMonth(), dt.getYear());
      lv_label_set_text(time_label, time_buf);
      lv_label_set_text(date_label, date_buf);
    }
  }
  
  lv_task_handler();
  delay(2);
}

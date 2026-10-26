#include <Arduino.h>
#include "ESP32-S3-Screen-AMOLED-2.06.h"
#include "ESP32-S3-Touch-AMOLED-2.06.h"
#include <lvgl.h>
#include <WiFi.h>

// --- Globals & objects ---
ScreenClass Screen;
TouchClass Touch;

// Main UI
lv_obj_t* label;
lv_obj_t* list;
lv_obj_t* progress_bar;
lv_obj_t* scan_status_label;
lv_obj_t* main_screen;
lv_obj_t* details_screen;
lv_obj_t* details_network_label;
lv_obj_t* details_info_label;
lv_obj_t* details_status_label;
lv_obj_t* scan_button;

// Keyboard popup objects (created when needed)
lv_obj_t* kb_container = nullptr;
lv_obj_t* kb_textarea = nullptr;
lv_obj_t* kb_keyboard = nullptr;

unsigned long last_tick = 0;
unsigned long last_debug = 0;

// Scanning variables
bool is_scanning = false;
bool button_held = false;
unsigned long scan_start_time = 0;
unsigned long last_scan_time = 0;
int scan_cycle_count = 0;

// Network storage
struct NetworkInfo {
  String ssid;
  int rssi;
  bool secure;
  unsigned long first_seen;
  String encryption;
  int channel;
};
NetworkInfo networks[100];
int network_count = 0;
int selected_network_index = -1;

// Forward declarations
void startScanning();
void stopScanning();
void updateScanProgress();
void processScanResults();
void addOrUpdateNetwork(const String& ssid, int rssi, bool secure, String encryption = "", int channel = 0);
void displayNetworks();
void showNetworkDetails(int index);
void showMainScreen();
void connectToNetwork(int index);
void cloneNetwork(int index);
void showNetworkInfo(int index);
void safeLabelSetText(lv_obj_t* label, const char* text);
String sanitizeSSID(const String& ssid);
void sortNetworksByRSSI();
void setupDetailsScreen();
void showKeyboardForSSID(const String& ssid);
void keyboard_event_cb(lv_event_t* e);
void attemptWifiConnect(const char* ssid, const char* password);

void setup() {
  Serial.begin(115200);
  delay(500);
  Serial.println("Starting WiFi scanner with improved keyboard");

  setlocale(LC_ALL, "en_US.UTF-8");

  // --- Display & touch init ---
  Screen.on();
  Serial.println("Display initialized");
  Touch.on();
  Serial.println("Touch initialized");

  // --- LVGL UI ---
  main_screen = lv_scr_act();
  lv_obj_set_style_bg_color(main_screen, lv_color_hex(0x000000), 0);

  // details screen
  details_screen = lv_obj_create(NULL);
  lv_obj_set_style_bg_color(details_screen, lv_color_hex(0x000000), 0);

  // Main UI elements
  label = lv_label_create(main_screen);
  safeLabelSetText(label, "Hold the button to scan WiFi\nRelease to show results");
  lv_obj_align(label, LV_ALIGN_TOP_MID, 0, 10);
  lv_obj_set_style_text_color(label, lv_color_hex(0xFFFFFF), 0);
  lv_obj_set_style_text_font(label, &lv_font_montserrat_16, 0);

  scan_status_label = lv_label_create(main_screen);
  safeLabelSetText(scan_status_label, "Ready");
  lv_obj_align(scan_status_label, LV_ALIGN_TOP_MID, 0, 50);
  lv_obj_set_style_text_color(scan_status_label, lv_color_hex(0x00FF00), 0);
  lv_obj_set_style_text_font(scan_status_label, &lv_font_montserrat_14, 0);

  progress_bar = lv_bar_create(main_screen);
  lv_obj_set_size(progress_bar, 200, 20);
  lv_obj_align(progress_bar, LV_ALIGN_TOP_MID, 0, 80);
  lv_bar_set_value(progress_bar, 0, LV_ANIM_OFF);
  lv_obj_set_style_bg_color(progress_bar, lv_color_hex(0x333333), 0);
  lv_obj_set_style_bg_color(progress_bar, lv_color_hex(0x0066FF), LV_PART_INDICATOR);

  list = lv_list_create(main_screen);
  lv_obj_set_size(list, 220, 120);
  lv_obj_align(list, LV_ALIGN_CENTER, 0, 10);
  lv_obj_set_style_bg_color(list, lv_color_hex(0x202020), 0);
  lv_obj_set_style_border_color(list, lv_color_hex(0x505050), 0);

  // Scan button
  scan_button = lv_btn_create(main_screen);
  lv_obj_set_size(scan_button, 160, 60);
  lv_obj_align(scan_button, LV_ALIGN_BOTTOM_MID, 0, -20);
  lv_obj_set_style_bg_color(scan_button, lv_palette_main(LV_PALETTE_BLUE), 0);

  lv_obj_add_event_cb(scan_button, [](lv_event_t* e) {
    lv_event_code_t code = lv_event_get_code(e);
    if (code == LV_EVENT_PRESSED) {
      Serial.println("Button pressed — starting continuous scan");
      button_held = true;
      startScanning();
    } else if (code == LV_EVENT_RELEASED) {
      Serial.println("Button released — stopping scan");
      button_held = false;
      stopScanning();
    }
  }, LV_EVENT_ALL, NULL);

  lv_obj_t* btn_label = lv_label_create(scan_button);
  safeLabelSetText(btn_label, "HOLD TO SCAN");
  lv_obj_center(btn_label);
  lv_obj_set_style_text_color(btn_label, lv_color_white(), 0);
  lv_obj_set_style_text_font(btn_label, &lv_font_montserrat_16, 0);

  // Details screen UI
  setupDetailsScreen();

  // WiFi init
  WiFi.mode(WIFI_STA);
  WiFi.disconnect(true);
  delay(100);
  Serial.println("WiFi initialized (STA mode, disconnected)");

  Serial.println("UI created — ready for touch");
}

void setupDetailsScreen() {
  details_network_label = lv_label_create(details_screen);
  lv_obj_align(details_network_label, LV_ALIGN_TOP_MID, 0, 20);
  lv_obj_set_style_text_color(details_network_label, lv_color_hex(0xFFFFFF), 0);
  lv_obj_set_style_text_font(details_network_label, &lv_font_montserrat_20, 0);
  safeLabelSetText(details_network_label, "Network Name");

  details_info_label = lv_label_create(details_screen);
  lv_obj_align(details_info_label, LV_ALIGN_TOP_MID, 0, 50);
  lv_obj_set_style_text_color(details_info_label, lv_color_hex(0xCCCCCC), 0);
  lv_obj_set_style_text_font(details_info_label, &lv_font_montserrat_14, 0);
  safeLabelSetText(details_info_label, "RSSI: 0dBm | Channel: 0");

  details_status_label = lv_label_create(details_screen);
  lv_obj_set_width(details_status_label, 220);
  lv_obj_align(details_status_label, LV_ALIGN_TOP_MID, 0, 75);
  lv_obj_set_style_text_color(details_status_label, lv_color_hex(0x00FF00), 0);
  lv_obj_set_style_text_font(details_status_label, &lv_font_montserrat_12, 0);
  safeLabelSetText(details_status_label, "Select an action");

  // Button container - properly spaced
  lv_obj_t* btn_container = lv_obj_create(details_screen);
  lv_obj_set_size(btn_container, 230, 300);
  lv_obj_align(btn_container, LV_ALIGN_BOTTOM_MID, 0, -5);
  lv_obj_set_style_bg_color(btn_container, lv_color_hex(0x000000), 0);
  lv_obj_set_style_border_color(btn_container, lv_color_hex(0x000000), 0);
  lv_obj_set_style_pad_all(btn_container, 5, 0);

  // Back button
  lv_obj_t* back_btn = lv_btn_create(btn_container);
  lv_obj_set_size(back_btn, 105, 55);
  lv_obj_set_pos(back_btn, 5, 5);
  lv_obj_set_style_bg_color(back_btn, lv_palette_main(LV_PALETTE_GREY), 0);
  lv_obj_add_event_cb(back_btn, [](lv_event_t* e) {
    if (lv_event_get_code(e) == LV_EVENT_CLICKED) showMainScreen();
  }, LV_EVENT_CLICKED, NULL);
  lv_obj_t* back_label = lv_label_create(back_btn);
  safeLabelSetText(back_label, "BACK");
  lv_obj_center(back_label);

  // Info button
  lv_obj_t* info_btn = lv_btn_create(btn_container);
  lv_obj_set_size(info_btn, 105, 55);
  lv_obj_set_pos(info_btn, 115, 5);
  lv_obj_set_style_bg_color(info_btn, lv_palette_main(LV_PALETTE_BLUE), 0);
  lv_obj_add_event_cb(info_btn, [](lv_event_t* e) {
    if (lv_event_get_code(e) == LV_EVENT_CLICKED) showNetworkInfo(selected_network_index);
  }, LV_EVENT_CLICKED, NULL);
  lv_obj_t* info_label_btn = lv_label_create(info_btn);
  safeLabelSetText(info_label_btn, "INFO");
  lv_obj_center(info_label_btn);

  // Connect button
  lv_obj_t* connect_btn = lv_btn_create(btn_container);
  lv_obj_set_size(connect_btn, 105, 55);
  lv_obj_set_pos(connect_btn, 5, 70);
  lv_obj_set_style_bg_color(connect_btn, lv_palette_main(LV_PALETTE_GREEN), 0);
  lv_obj_add_event_cb(connect_btn, [](lv_event_t* e) {
    if (lv_event_get_code(e) == LV_EVENT_CLICKED) connectToNetwork(selected_network_index);
  }, LV_EVENT_CLICKED, NULL);
  lv_obj_t* connect_label = lv_label_create(connect_btn);
  safeLabelSetText(connect_label, "CONNECT");
  lv_obj_center(connect_label);

  // Clone button
  lv_obj_t* clone_btn = lv_btn_create(btn_container);
  lv_obj_set_size(clone_btn, 105, 55);
  lv_obj_set_pos(clone_btn, 115, 70);
  lv_obj_set_style_bg_color(clone_btn, lv_palette_main(LV_PALETTE_ORANGE), 0);
  lv_obj_add_event_cb(clone_btn, [](lv_event_t* e) {
    if (lv_event_get_code(e) == LV_EVENT_CLICKED) cloneNetwork(selected_network_index);
  }, LV_EVENT_CLICKED, NULL);
  lv_obj_t* clone_label = lv_label_create(clone_btn);
  safeLabelSetText(clone_label, "CLONE");
  lv_obj_center(clone_label);

  // NEW FEATURE: Signal Strength Analyzer
  lv_obj_t* analyze_btn = lv_btn_create(btn_container);
  lv_obj_set_size(analyze_btn, 215, 55);
  lv_obj_set_pos(analyze_btn, 5, 135);
  lv_obj_set_style_bg_color(analyze_btn, lv_palette_main(LV_PALETTE_PURPLE), 0);
  lv_obj_add_event_cb(analyze_btn, [](lv_event_t* e) {
    if (lv_event_get_code(e) == LV_EVENT_CLICKED) {
      if (selected_network_index >= 0 && selected_network_index < network_count) {
        Serial.printf("Analyzing signal: %s\n", networks[selected_network_index].ssid.c_str());
        if (details_status_label) {
          int rssi = networks[selected_network_index].rssi;
          const char* quality = rssi > -50 ? "EXCELLENT" : rssi > -60 ? "GOOD" : rssi > -70 ? "FAIR" : "WEAK";
          char buf[128];
          snprintf(buf, sizeof(buf), "Signal: %s (%ddBm)", quality, rssi);
          safeLabelSetText(details_status_label, buf);
          lv_obj_set_style_text_color(details_status_label, lv_color_hex(0xFF00FF), 0);
        }
      }
    }
  }, LV_EVENT_CLICKED, NULL);
  lv_obj_t* analyze_label = lv_label_create(analyze_btn);
  safeLabelSetText(analyze_label, "ANALYZE SIGNAL");
  lv_obj_center(analyze_label);
}

void loop() {
  unsigned long current_millis = millis();

  if (current_millis - last_tick >= 5) {
    lv_tick_inc(5);
    last_tick = current_millis;
  }

  if (is_scanning) {
    updateScanProgress();
    int scan_status = WiFi.scanComplete();
    if (scan_status != WIFI_SCAN_RUNNING && scan_status != WIFI_SCAN_FAILED) {
      processScanResults();
      if (button_held) {
        Serial.println("Starting next scan cycle...");
        WiFi.scanNetworks(true, true);
        last_scan_time = current_millis;
        scan_cycle_count++;
      }
    }
    if (current_millis - last_scan_time > 5000 && button_held) {
      Serial.println("Scan timeout - restarting...");
      WiFi.scanDelete();
      WiFi.scanNetworks(true, true);
      last_scan_time = current_millis;
    }
  }

  if (current_millis - last_debug >= 2000) {
    if (is_scanning) {
      Serial.printf("Continuous scanning... %lu ms elapsed, %d cycles, %d networks found\n",
                   current_millis - scan_start_time, scan_cycle_count, network_count);
    } else {
      Serial.println("System running...");
    }
    last_debug = current_millis;
  }

  lv_task_handler();
  delay(2);
}

void startScanning() {
  if (is_scanning) {
    scan_start_time = millis();
    return;
  }
  is_scanning = true;
  button_held = true;
  scan_start_time = millis();
  last_scan_time = millis();
  scan_cycle_count = 0;
  network_count = 0;
  lv_obj_clean(list);

  safeLabelSetText(scan_status_label, "SCANNING...");
  lv_obj_set_style_text_color(scan_status_label, lv_color_hex(0xFFFF00), 0);
  lv_bar_set_value(progress_bar, 0, LV_ANIM_OFF);

  if (scan_button) lv_obj_set_style_bg_color(scan_button, lv_palette_main(LV_PALETTE_RED), 0);
  WiFi.scanNetworks(true, true);
  Serial.println("Continuous WiFi scan started");
}

void stopScanning() {
  if (!is_scanning) return;
  is_scanning = false;
  button_held = false;

  int n = WiFi.scanComplete();
  if (n == WIFI_SCAN_RUNNING) {
    Serial.println("Waiting for final scan completion...");
    delay(500);
    n = WiFi.scanComplete();
  }
  if (n > 0) processScanResults();

  char status[32];
  snprintf(status, sizeof(status), "FOUND %d NETWORKS", network_count);
  safeLabelSetText(scan_status_label, status);
  lv_obj_set_style_text_color(scan_status_label, lv_color_hex(0x00FF00), 0);
  lv_bar_set_value(progress_bar, 100, LV_ANIM_ON);

  if (scan_button) lv_obj_set_style_bg_color(scan_button, lv_palette_main(LV_PALETTE_BLUE), 0);

  displayNetworks();
  Serial.printf("Scanning stopped. Total: %d scan cycles, %d unique networks found\n", scan_cycle_count, network_count);
  WiFi.scanDelete();
}

void processScanResults() {
  int n = WiFi.scanComplete();
  if (n > 0) {
    Serial.printf("Scan cycle %d completed: found %d networks\n", scan_cycle_count + 1, n);
    for (int i = 0; i < n; ++i) {
      String ssid = WiFi.SSID(i);
      int rssi = WiFi.RSSI(i);
      bool secure = (WiFi.encryptionType(i) != WIFI_AUTH_OPEN);
      String encryption = "UNKNOWN";
      int channel = WiFi.channel(i);
      wifi_auth_mode_t etype = WiFi.encryptionType(i);
      switch (etype) {
        case WIFI_AUTH_OPEN: encryption = "OPEN"; break;
        case WIFI_AUTH_WEP: encryption = "WEP"; break;
        case WIFI_AUTH_WPA_PSK: encryption = "WPA"; break;
        case WIFI_AUTH_WPA2_PSK: encryption = "WPA2"; break;
        case WIFI_AUTH_WPA_WPA2_PSK: encryption = "WPA/WPA2"; break;
        case WIFI_AUTH_WPA2_ENTERPRISE: encryption = "WPA2-ENT"; break;
        default: encryption = "UNKNOWN"; break;
      }
      addOrUpdateNetwork(ssid, rssi, secure, encryption, channel);
    }
    displayNetworks();
  } else if (n == 0) {
    Serial.println("Scan cycle completed: no networks found");
  } else {
    Serial.println("Scan cycle failed");
  }
}

void addOrUpdateNetwork(const String& ssid, int rssi, bool secure, String encryption, int channel) {
  if (ssid.length() == 0) return;
  String clean_ssid = sanitizeSSID(ssid);
  for (int i = 0; i < network_count; i++) {
    if (networks[i].ssid == clean_ssid) {
      if (rssi > networks[i].rssi) networks[i].rssi = rssi;
      return;
    }
  }
  if (network_count < 100) {
    networks[network_count].ssid = clean_ssid;
    networks[network_count].rssi = rssi;
    networks[network_count].secure = secure;
    networks[network_count].encryption = encryption;
    networks[network_count].channel = channel;
    networks[network_count].first_seen = millis();
    network_count++;
    Serial.printf("New network: %s (%ddBm)\n", clean_ssid.c_str(), rssi);
  }
}

void sortNetworksByRSSI() {
  if (network_count <= 1) return;
  for (int i = 0; i < network_count - 1; ++i) {
    for (int j = 0; j < network_count - 1 - i; ++j) {
      if (networks[j].rssi < networks[j + 1].rssi) {
        NetworkInfo tmp = networks[j];
        networks[j] = networks[j + 1];
        networks[j + 1] = tmp;
      }
    }
  }
}

void displayNetworks() {
  lv_obj_clean(list);

  if (network_count == 0) {
    lv_list_add_text(list, "No networks found");
    safeLabelSetText(label, "No WiFi networks found");
    return;
  }

  sortNetworksByRSSI();

  char main_label[64];
  snprintf(main_label, sizeof(main_label), "Found %d networks\n%d scan cycles", network_count, scan_cycle_count);
  safeLabelSetText(label, main_label);

  int display_limit = network_count < 15 ? network_count : 15;
  for (int i = 0; i < display_limit; ++i) {
    char entry[128];
    snprintf(entry, sizeof(entry), "%s (%ddBm)%s",
             networks[i].ssid.c_str(), networks[i].rssi,
             networks[i].secure ? " [Secured]" : "");
    lv_obj_t* list_btn = lv_list_add_btn(list, LV_SYMBOL_WIFI, entry);
    lv_obj_set_user_data(list_btn, (void*)(intptr_t)i);
    lv_obj_add_event_cb(list_btn, [](lv_event_t* e) {
      lv_obj_t* target = (lv_obj_t*)lv_event_get_target(e);
      if (!target) return;
      intptr_t raw = (intptr_t)lv_obj_get_user_data(target);
      int index = (int)raw;
      if (index >= 0 && index < network_count) {
        selected_network_index = index;
        showNetworkDetails(index);
      } else {
        Serial.printf("Clicked list item has invalid index: %d\n", index);
      }
    }, LV_EVENT_CLICKED, NULL);
  }

  if (network_count > 15) {
    char more_msg[32];
    snprintf(more_msg, sizeof(more_msg), "... and %d more", network_count - 15);
    lv_list_add_text(list, more_msg);
  }
}

void showNetworkDetails(int index) {
  if (index < 0 || index >= network_count) {
    Serial.printf("Invalid network index: %d\n", index);
    return;
  }
  Serial.printf("Showing details for network %d: %s\n", index, networks[index].ssid.c_str());
  if (details_network_label) safeLabelSetText(details_network_label, networks[index].ssid.c_str());
  if (details_info_label) {
    char info[64];
    snprintf(info, sizeof(info), "RSSI: %ddBm | Channel: %d | %s",
             networks[index].rssi, networks[index].channel,
             networks[index].secure ? "Secured" : "Open");
    safeLabelSetText(details_info_label, info);
  }
  if (details_status_label) {
    safeLabelSetText(details_status_label, "Select an action");
    lv_obj_set_style_text_color(details_status_label, lv_color_hex(0x00FF00), 0);
  }
  lv_scr_load(details_screen);
  Serial.printf("Details screen loaded for: %s\n", networks[index].ssid.c_str());
}

void showMainScreen() {
  lv_scr_load(main_screen);
  Serial.println("Returned to main screen");
}

void connectToNetwork(int index) {
  if (index < 0 || index >= network_count) return;
  Serial.printf("Connect pressed for: %s\n", networks[index].ssid.c_str());
  showKeyboardForSSID(networks[index].ssid);
}

void cloneNetwork(int index) {
  if (index < 0 || index >= network_count) return;
  Serial.printf("Clone network: %s\n", networks[index].ssid.c_str());
  if (details_status_label) {
    safeLabelSetText(details_status_label, "Clone feature - TODO");
    lv_obj_set_style_text_color(details_status_label, lv_color_hex(0xFFFF00), 0);
  }
}

void showNetworkInfo(int index) {
  if (index < 0 || index >= network_count) return;
  Serial.printf("Showing info for: %s\n", networks[index].ssid.c_str());
  if (details_status_label) {
    char info[256];
    snprintf(info, sizeof(info),
             "SSID: %s\nRSSI: %ddBm\nEncryption: %s\nChannel: %d\nFirst seen: %lu s ago",
             networks[index].ssid.c_str(),
             networks[index].rssi,
             networks[index].encryption.c_str(),
             networks[index].channel,
             (millis() - networks[index].first_seen) / 1000);
    safeLabelSetText(details_status_label, info);
    lv_obj_set_style_text_color(details_status_label, lv_color_hex(0x00FF00), 0);
  }
}

void showKeyboardForSSID(const String& ssid) {
  // Delete existing keyboard if present
  if (kb_container) {
    lv_obj_del(kb_container);
    kb_container = nullptr;
    kb_textarea = nullptr;
    kb_keyboard = nullptr;
  }

  // Create full-screen modal container
  kb_container = lv_obj_create(details_screen);
  lv_obj_set_size(kb_container, LV_PCT(100), LV_PCT(100));
  lv_obj_center(kb_container);
  lv_obj_set_style_bg_color(kb_container, lv_color_hex(0x0A0A0A), 0);
  lv_obj_set_style_bg_opa(kb_container, LV_OPA_90, 0);
  lv_obj_set_style_radius(kb_container, 0, 0);
  lv_obj_set_style_pad_all(kb_container, 10, 0);
  lv_obj_set_style_border_width(kb_container, 0, 0);

  // Title label with network name
  lv_obj_t* title = lv_label_create(kb_container);
  char tbuf[128];
  snprintf(tbuf, sizeof(tbuf), "Enter Password for:\n%s", ssid.c_str());
  safeLabelSetText(title, tbuf);
  lv_obj_set_style_text_align(title, LV_TEXT_ALIGN_CENTER, 0);
  lv_obj_set_style_text_color(title, lv_color_hex(0xFFFFFF), 0);
  lv_obj_set_style_text_font(title, &lv_font_montserrat_14, 0);
  lv_obj_align(title, LV_ALIGN_TOP_MID, 0, 8);

  // Create textarea (masked password input)
  kb_textarea = lv_textarea_create(kb_container);
  lv_obj_set_size(kb_textarea, LV_PCT(90), 40);
  lv_obj_align(kb_textarea, LV_ALIGN_TOP_MID, 0, 50);
  lv_textarea_set_placeholder_text(kb_textarea, "Enter password...");
  lv_textarea_set_password_mode(kb_textarea, false); // Disable password masking to avoid encoding issues
  lv_textarea_set_one_line(kb_textarea, true);
  lv_textarea_set_text(kb_textarea, ""); // Initialize with empty string
  lv_textarea_set_accepted_chars(kb_textarea, "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789!@#$%^&*()_+-=[]{}|;:',.<>?/~` ");
  
  // Style the textarea
  lv_obj_set_style_radius(kb_textarea, 8, 0);
  lv_obj_set_style_pad_all(kb_textarea, 10, 0);
  lv_obj_set_style_bg_color(kb_textarea, lv_color_hex(0x1A1A1A), 0);
  lv_obj_set_style_border_color(kb_textarea, lv_color_hex(0x444444), 0);
  lv_obj_set_style_border_width(kb_textarea, 2, 0);
  lv_obj_set_style_text_color(kb_textarea, lv_color_hex(0xFFFFFF), 0);

  // Create keyboard with improved sizing - MUCH BIGGER
  kb_keyboard = lv_keyboard_create(kb_container);
  lv_obj_set_size(kb_keyboard, LV_PCT(98), 220);
  lv_obj_align(kb_keyboard, LV_ALIGN_BOTTOM_MID, 0, -50);
  lv_keyboard_set_textarea(kb_keyboard, kb_textarea);

  // CRITICAL: MAXIMUM keyboard button spacing and sizing
  // Increase row spacing (vertical gap between button rows)
  lv_obj_set_style_pad_row(kb_keyboard, 14, LV_PART_ITEMS);
  
  // Increase column spacing (horizontal gap between buttons)
  lv_obj_set_style_pad_column(kb_keyboard, 14, LV_PART_ITEMS);
  
  // Make buttons rounder and more touch-friendly
  lv_obj_set_style_radius(kb_keyboard, 10, LV_PART_ITEMS);
  
  // Add internal padding to buttons (makes them MUCH bigger)
  lv_obj_set_style_pad_all(kb_keyboard, 12, LV_PART_ITEMS);
  
  // Increase overall keyboard padding
  lv_obj_set_style_pad_all(kb_keyboard, 12, 0);
  
  // Make button text larger and more readable
  lv_obj_set_style_text_font(kb_keyboard, &lv_font_montserrat_18, LV_PART_ITEMS);
  
  // Style the keyboard background
  lv_obj_set_style_bg_color(kb_keyboard, lv_color_hex(0x1A1A1A), 0);
  lv_obj_set_style_border_color(kb_keyboard, lv_color_hex(0x333333), 0);
  lv_obj_set_style_border_width(kb_keyboard, 1, 0);
  lv_obj_set_style_radius(kb_keyboard, 10, 0);
  
  // Style individual keys for better visibility
  lv_obj_set_style_bg_color(kb_keyboard, lv_color_hex(0x2A2A2A), LV_PART_ITEMS);
  lv_obj_set_style_text_color(kb_keyboard, lv_color_hex(0xFFFFFF), LV_PART_ITEMS);
  lv_obj_set_style_shadow_width(kb_keyboard, 0, LV_PART_ITEMS);

  // Attach event handler to keyboard
  lv_obj_add_event_cb(kb_keyboard, keyboard_event_cb, LV_EVENT_ALL, NULL);

  // Create BACK button to close keyboard
  lv_obj_t* kb_back_btn = lv_btn_create(kb_container);
  lv_obj_set_size(kb_back_btn, 80, 35);
  lv_obj_align(kb_back_btn, LV_ALIGN_BOTTOM_LEFT, 10, -10);
  lv_obj_set_style_bg_color(kb_back_btn, lv_palette_main(LV_PALETTE_RED), 0);
  lv_obj_add_event_cb(kb_back_btn, [](lv_event_t* e) {
    if (lv_event_get_code(e) == LV_EVENT_CLICKED) {
      if (kb_container) {
        lv_obj_del(kb_container);
        kb_container = nullptr;
        kb_textarea = nullptr;
        kb_keyboard = nullptr;
      }
      if (details_status_label) {
        safeLabelSetText(details_status_label, "Keyboard closed");
        lv_obj_set_style_text_color(details_status_label, lv_color_hex(0xFFFF00), 0);
      }
    }
  }, LV_EVENT_CLICKED, NULL);
  lv_obj_t* kb_back_label = lv_label_create(kb_back_btn);
  safeLabelSetText(kb_back_label, "BACK");
  lv_obj_center(kb_back_label);
  lv_obj_set_style_text_color(kb_back_label, lv_color_white(), 0);

  // Update details status
  if (details_status_label) {
    safeLabelSetText(details_status_label, "Enter password and tap OK");
    lv_obj_set_style_text_color(details_status_label, lv_color_hex(0xFFFF00), 0);
  }
}

// Keyboard event callback
void keyboard_event_cb(lv_event_t* e) {
  lv_event_code_t code = lv_event_get_code(e);
  lv_obj_t* kb = (lv_obj_t*)lv_event_get_target(e);
  if (!kb) return;

  if (code == LV_EVENT_VALUE_CHANGED) {
    // Optional: live feedback on typing
  } else if (code == LV_EVENT_CANCEL) {
    // Hide & delete keyboard
    if (kb_container) {
      lv_obj_del(kb_container);
      kb_container = nullptr;
      kb_textarea = nullptr;
      kb_keyboard = nullptr;
    }
    if (details_status_label) {
      safeLabelSetText(details_status_label, "Connect cancelled");
      lv_obj_set_style_text_color(details_status_label, lv_color_hex(0xFF0000), 0);
    }
  } else if (code == LV_EVENT_READY) {
    // User pressed OK
    if (!kb_textarea) return;
    const char* pwd = lv_textarea_get_text(kb_textarea);
    if (details_status_label) {
      safeLabelSetText(details_status_label, "Connecting...");
      lv_obj_set_style_text_color(details_status_label, lv_color_hex(0xFFFF00), 0);
    }
    const char* ssid_text = lv_label_get_text(details_network_label);
    // Close keyboard UI
    if (kb_container) {
      lv_obj_del(kb_container);
      kb_container = nullptr;
      kb_textarea = nullptr;
      kb_keyboard = nullptr;
    }
    // Attempt to connect
    attemptWifiConnect(ssid_text, pwd);
  }
}

void attemptWifiConnect(const char* ssid, const char* password) {
  if (!ssid) return;
  Serial.printf("Attempting WiFi connect to '%s' with password '%s'\n", ssid, password);
  
  // Ensure we're fully disconnected first
  WiFi.disconnect(true);
  delay(100);
  WiFi.mode(WIFI_STA);
  delay(300);
  
  // Begin connection
  Serial.println("Calling WiFi.begin()...");
  WiFi.begin(ssid, password);
  delay(100);

  unsigned long start = millis();
  const unsigned long timeout = 20000; // 20s timeout
  bool connected = false;
  
  while (millis() - start < timeout) {
    wl_status_t st = WiFi.status();
    
    // Status 6 = WL_DISCONNECTED - this means it's still trying
    if (st == WL_CONNECTED) {
      connected = true;
      Serial.println("Status changed to WL_CONNECTED!");
      break;
    }
    
    // Only break early on actual failures
    if (st == WL_CONNECT_FAILED) {
      Serial.println("Status: WL_CONNECT_FAILED - Wrong password");
      break;
    }
    if (st == WL_NO_SSID_AVAIL) {
      Serial.println("Status: WL_NO_SSID_AVAIL - Network not in range");
      break;
    }
    
    // Log status every second
    static unsigned long last_log = 0;
    if (millis() - last_log > 1000) {
      Serial.printf("WiFi status: %d (elapsed: %lus)\n", st, (millis() - start) / 1000);
      last_log = millis();
    }
    
    if (details_status_label) {
      static const char* spinner = "|/-\\";
      static int pos = 0;
      char buf[64];
      unsigned long elapsed = (millis() - start) / 1000;
      snprintf(buf, sizeof(buf), "Connecting %c (%lus)", spinner[pos % 4], elapsed);
      safeLabelSetText(details_status_label, buf);
      pos++;
    }
    
    delay(200);
    lv_task_handler();
  }

  if (connected) {
    IPAddress ip = WiFi.localIP();
    char buf[128];
    snprintf(buf, sizeof(buf), "Connected!\nIP: %s", ip.toString().c_str());
    safeLabelSetText(details_status_label, buf);
    lv_obj_set_style_text_color(details_status_label, lv_color_hex(0x00FF00), 0);
    Serial.printf("WiFi connected! IP: %s\n", ip.toString().c_str());
  } else {
    wl_status_t final_status = WiFi.status();
    char buf[128];
    
    if (final_status == WL_CONNECT_FAILED) {
      snprintf(buf, sizeof(buf), "Wrong password!");
      Serial.println("Connection failed: Wrong password");
    } else if (final_status == WL_NO_SSID_AVAIL) {
      snprintf(buf, sizeof(buf), "Network not found!");
      Serial.println("Connection failed: Network not in range");
    } else if (final_status == WL_DISCONNECTED || final_status == 6) {
      snprintf(buf, sizeof(buf), "Timeout - Check password\nand signal strength");
      Serial.println("Connection timed out while disconnected - likely wrong password or weak signal");
    } else {
      snprintf(buf, sizeof(buf), "Failed (status: %d)\nTry again", final_status);
      Serial.printf("Connection failed with status: %d\n", final_status);
    }
    
    safeLabelSetText(details_status_label, buf);
    lv_obj_set_style_text_color(details_status_label, lv_color_hex(0xFF0000), 0);
    WiFi.disconnect(true);
  }
}

void updateScanProgress() {
  if (!is_scanning) return;
  unsigned long elapsed = millis() - scan_start_time;
  int progress = (elapsed % 30000) * 100 / 30000;
  lv_bar_set_value(progress_bar, progress, LV_ANIM_OFF);
  char status[64];
  snprintf(status, sizeof(status), "SCANNING... %lu.%lus | %d nets | %d cycles",
           elapsed / 1000, (elapsed % 1000) / 100,
           network_count, scan_cycle_count);
  safeLabelSetText(scan_status_label, status);
}

void safeLabelSetText(lv_obj_t* labelObj, const char* text) {
  if (labelObj && text) {
    String safe_text = String(text);
    for (size_t i = 0; i < safe_text.length(); i++) {
      if (safe_text[i] < 32 || safe_text[i] > 126) {
        if (safe_text[i] != '\n' && safe_text[i] != '\t') {
          safe_text.setCharAt(i, '?');
        }
      }
    }
    lv_label_set_text(labelObj, safe_text.c_str());
  }
}

String sanitizeSSID(const String& ssid) {
  String clean_ssid = ssid;
  for (size_t i = 0; i < clean_ssid.length(); i++) {
    if (clean_ssid[i] < 32 || clean_ssid[i] > 126) {
      if (clean_ssid[i] != '\n' && clean_ssid[i] != '\t') {
        clean_ssid.setCharAt(i, '?');
      }
    }
  }
  return clean_ssid;
}

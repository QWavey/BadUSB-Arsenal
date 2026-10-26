#include <Arduino.h>
#include <Wire.h>
#include <WiFi.h>
#include "time.h"
#include "ESP32-S3-Screen-AMOLED-2.06.h"
#include "ESP32-S3-Touch-AMOLED-2.06.h"
#include "SensorPCF85063.hpp"
#include "wifi_secrets.h" // copy wifi_secrets.example.h to wifi_secrets.h and fill it in (git-ignored)

// -------------------- Objects --------------------
ScreenClass Screen;
TouchClass Touch;
SensorPCF85063 rtc;
lv_obj_t* label;

unsigned long lastMillis = 0;
unsigned long last_tick = 0;

// -------------------- Wi-Fi & NTP --------------------
const char* ssid     = WIFI_SSID;
const char* password = WIFI_PASSWORD;
const char* ntpServer = "pool.ntp.org";

// -------------------- Timezone (easy to change) --------------------
// Example: "UTC+1" or "UTC-1"
const String timezoneStr = "UTC+1";  

long gmtOffset_sec = 0;
int daylightOffset_sec = 0;

void parseTimezone() {
  // Accepts strings like "UTC+1", "UTC-1", "UTC+2.5"
  if (timezoneStr.startsWith("UTC")) {
    char sign = timezoneStr[3]; // '+' or '-'
    float hours = timezoneStr.substring(4).toFloat();
    gmtOffset_sec = (long)(hours * 3600.0);
    if (sign == '-') gmtOffset_sec = -gmtOffset_sec;
    daylightOffset_sec = 0; // optional: add 3600 for DST if needed
    Serial.printf("Parsed timezone: %s → GMT offset: %ld seconds\n", timezoneStr.c_str(), gmtOffset_sec);
  } else {
    Serial.println("⚠️ Invalid timezone string, defaulting to UTC");
    gmtOffset_sec = 0;
    daylightOffset_sec = 0;
  }
}

// -------------------- Setup --------------------
void setup() {
  Serial.begin(115200);
  delay(1000);
  Serial.println("=== ESP32-S3 LVGL + RTC + NTP ===");

  // --- Screen & Touch init ---
  Screen.on();   // powers display and initializes LVGL
  Serial.println("Display initialized");
  
  Touch.on();    // initializes touch + LVGL input device
  Serial.println("Touch initialized");

  // --- RTC init ---
  Wire.begin();
  if (!rtc.begin(Wire, IIC_SDA, IIC_SCL)) {
    Serial.println("⚠️ Failed to find PCF85063 RTC");
    while (1) delay(1000);
  }
  Serial.println("RTC initialized");

  // --- Parse timezone ---
  parseTimezone();

  // --- Connect Wi-Fi ---
  Serial.printf("Connecting to Wi-Fi: %s", ssid);
  WiFi.begin(ssid, password);
  while (WiFi.status() != WL_CONNECTED) {
    delay(500);
    Serial.print(".");
  }
  Serial.println("\nWi-Fi connected!");

  // --- Configure NTP and set RTC ---
  configTime(gmtOffset_sec, daylightOffset_sec, ntpServer);
  // NTP over UDP is unauthenticated, so a spoofed reply could set any date.
  // Accept the result only if the year is plausible.
  struct tm timeinfo = {};
  bool ntp_ok = getLocalTime(&timeinfo);
  int ntp_year = timeinfo.tm_year + 1900;
  if (ntp_ok && ntp_year >= 2024 && ntp_year <= 2099) {
    rtc.setDateTime(timeinfo.tm_year + 1900,
                    timeinfo.tm_mon + 1,
                    timeinfo.tm_mday,
                    timeinfo.tm_hour,
                    timeinfo.tm_min,
                    timeinfo.tm_sec);
    Serial.println("RTC set from NTP");
  } else {
    Serial.println("⚠️ NTP failed, using default time");
    rtc.setDateTime(2025, 7, 21, 12, 0, 0);
  }

  // --- LVGL label for time ---
  lv_obj_t* scr = lv_scr_act();
  lv_obj_set_style_bg_color(scr, lv_color_black(), 0);

  label = lv_label_create(scr);
  lv_label_set_long_mode(label, LV_LABEL_LONG_WRAP);
  lv_obj_set_width(label, 400);
  lv_obj_align(label, LV_ALIGN_CENTER, 0, 0);
  lv_obj_set_style_text_color(label, lv_color_white(), 0);
  lv_obj_set_style_text_font(label, &lv_font_montserrat_40, LV_PART_MAIN);
  lv_label_set_text(label, "Initializing...");
}

// -------------------- Loop --------------------
void loop() {
  unsigned long currentMillis = millis();

  // --- LVGL tick ---
  if (currentMillis - last_tick >= 5) {
    lv_tick_inc(5);
    last_tick = currentMillis;
  }
  lv_task_handler();

  // --- Update time every second ---
  if (currentMillis - lastMillis >= 1000) {
    lastMillis = currentMillis;

    RTC_DateTime datetime = rtc.getDateTime();
    char buf[64];
    snprintf(buf, sizeof(buf), "%02d:%02d:%02d\n%02d-%02d-%04d",
             datetime.getHour(), datetime.getMinute(), datetime.getSecond(),
             datetime.getDay(), datetime.getMonth(), datetime.getYear());

    lv_label_set_text(label, buf);

    Serial.printf("RTC: %02d-%02d-%04d %02d:%02d:%02d\n",
                  datetime.getDay(), datetime.getMonth(), datetime.getYear(),
                  datetime.getHour(), datetime.getMinute(), datetime.getSecond());
  }

  // --- Optional touch ---
  if (Touch.isTouched()) {
    int32_t x, y;
    Touch.getTouchPoint(x, y);
    Serial.printf("Touch at x=%ld y=%ld\n", x, y);
  }

  delay(2);
}

#include <Arduino.h>
#include "ESP32-S3-Screen-AMOLED-2.06.h"
#include "ESP32-S3-Touch-AMOLED-2.06.h"
#include "XPowersLib.h"
#include <Wire.h>

ScreenClass Screen;
TouchClass Touch;
XPowersPMU power;

lv_obj_t* info_label;
unsigned long last_tick = 0;

void setup() {
    Serial.begin(115200);
    delay(1000);
    Serial.println("Starting Power Monitor LVGL test...");

    // Initialize display and LVGL
    Screen.on();
    Serial.println("Display initialized");

    // Initialize touch input
    Touch.on();
    Serial.println("Touch initialized");

    // Initialize XPowers
    power.disableIRQ(XPOWERS_AXP2101_ALL_IRQ);
    power.setChargeTargetVoltage(3);
    power.clearIrqStatus();
    power.enableIRQ(XPOWERS_AXP2101_PKEY_SHORT_IRQ); // Power key interrupt
    power.enableTemperatureMeasure();
    power.enableBattDetection();
    power.enableVbusVoltageMeasure();
    power.enableBattVoltageMeasure();
    power.enableSystemVoltageMeasure();

    // Create UI
    lv_obj_t* scr = lv_scr_act();
    lv_obj_set_style_bg_color(scr, lv_color_black(), 0);

    info_label = lv_label_create(scr);
    lv_label_set_long_mode(info_label, LV_LABEL_LONG_WRAP);
    lv_obj_set_width(info_label, 400);
    lv_obj_align(info_label, LV_ALIGN_CENTER, 0, 0);
    lv_obj_set_style_text_color(info_label, lv_color_white(), 0);
    lv_label_set_text(info_label, "Initializing...");
}

void loop() {
    unsigned long currentMillis = millis();

    // LVGL tick
    if (currentMillis - last_tick >= 5) {
        lv_tick_inc(5);
        last_tick = currentMillis;
    }
    lv_task_handler();

    // Build info string
    String info = "";
    uint8_t charge_status = power.getChargerStatus();

    info += "Power Temperature: " + String(power.getTemperature()) + "°C\n";
    info += "Charging: " + String(power.isCharging() ? "YES" : "NO") + "\n";
    info += "Discharge: " + String(power.isDischarge() ? "YES" : "NO") + "\n";
    info += "Standby: " + String(power.isStandby() ? "YES" : "NO") + "\n";
    info += "Vbus In: " + String(power.isVbusIn() ? "YES" : "NO") + "\n";
    info += "Vbus Good: " + String(power.isVbusGood() ? "YES" : "NO") + "\n";

    switch (charge_status) {
        case XPOWERS_AXP2101_CHG_TRI_STATE: info += "Charger Status: tri_charge\n"; break;
        case XPOWERS_AXP2101_CHG_PRE_STATE: info += "Charger Status: pre_charge\n"; break;
        case XPOWERS_AXP2101_CHG_CC_STATE: info += "Charger Status: constant charge\n"; break;
        case XPOWERS_AXP2101_CHG_CV_STATE: info += "Charger Status: constant voltage\n"; break;
        case XPOWERS_AXP2101_CHG_DONE_STATE: info += "Charger Status: charge done\n"; break;
        case XPOWERS_AXP2101_CHG_STOP_STATE: info += "Charger Status: not charging\n"; break;
    }

    info += "Battery Voltage: " + String(power.getBattVoltage()) + " mV\n";
    info += "Vbus Voltage: " + String(power.getVbusVoltage()) + " mV\n";
    info += "System Voltage: " + String(power.getSystemVoltage()) + " mV\n";

    if (power.isBatteryConnect()) {
        info += "Battery Percent: " + String(power.getBatteryPercent()) + "%\n";
    }

    lv_label_set_text(info_label, info.c_str());
    lv_obj_set_style_text_font(info_label, &lv_font_montserrat_20, LV_PART_MAIN);

    // Optional: read touch coordinates
    if (Touch.isTouched()) {
        int32_t x, y;
        Touch.getTouchPoint(x, y);
        Serial.printf("Touch at x=%ld y=%ld\n", x, y);
    }

    delay(2); // Keep loop responsive
}

#pragma once

#include <Arduino.h>
#include <lvgl.h>
#include "XPowersLib.h"

class BatteryDesign {
public:
    enum Style {
        STYLE_MINIMAL,
        STYLE_DETAILED,
        STYLE_COMPACT,
        STYLE_ICON_ONLY
    };

    struct BatteryInfo {
        float voltage;
        float temperature;
        int percentage;
        bool isCharging;
        bool isDischarge;
        bool isVbusIn;
        uint8_t chargeStatus;
        int timeToFull;
        int timeRemaining;
        bool warningLow;
        bool criticalLow;
    };

    BatteryDesign();
    ~BatteryDesign();

    bool begin(XPowersAXP2101* pmu = nullptr);
    void update();
    BatteryInfo getInfo();

    lv_obj_t* createWidget(lv_obj_t* parent, Style style = STYLE_DETAILED);
    lv_obj_t* createDetailWindow(lv_obj_t* parent);
    lv_obj_t* createColorLegend(lv_obj_t* parent);

    static String formatTime(int minutes);
    static lv_color_t getColorForLevel(int percentage);

private:
    XPowersAXP2101* _pmu;
    bool _simulationMode;
    BatteryInfo _info;
    unsigned long _lastUpdate;
    unsigned long _updateInterval;

    lv_obj_t* _mainWidget;
    lv_obj_t* _percentLabel;
    lv_obj_t* _statusLabel;
    lv_obj_t* _voltageLabel;
    lv_obj_t* _detailWindow;

    void updateFromPMU();
    void updateSimulation();
    void updateWidgetDisplay();
    int estimateTimeToFull();
    int estimateTimeRemaining();

    static void detailButtonCallback(lv_event_t* e);
    static void closeButtonCallback(lv_event_t* e);
};

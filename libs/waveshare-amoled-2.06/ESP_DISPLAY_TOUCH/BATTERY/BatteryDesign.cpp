#include "BatteryDesign.h"

BatteryDesign::BatteryDesign() 
    : _pmu(nullptr), _simulationMode(false), _lastUpdate(0), 
      _updateInterval(2000), _mainWidget(nullptr), _percentLabel(nullptr),
      _statusLabel(nullptr), _voltageLabel(nullptr), _detailWindow(nullptr) {
    memset(&_info, 0, sizeof(BatteryInfo));
}

BatteryDesign::~BatteryDesign() {
}

bool BatteryDesign::begin(XPowersAXP2101* pmu) {
    _pmu = pmu;
    
    if (_pmu == nullptr) {
        Serial.println("[Battery] No PMU provided, entering simulation mode");
        _simulationMode = true;
        _info.percentage = 75;
        _info.voltage = 3.85;
        _info.temperature = 25.0;
        _info.isCharging = false;
        return true;
    }

    _pmu->enableBattDetection();
    _pmu->enableBattVoltageMeasure();
    _pmu->enableSystemVoltageMeasure();
    _pmu->enableTemperatureMeasure();
    _pmu->enableVbusVoltageMeasure();

    Serial.println("[Battery] Initialized with AXP2101 PMU");
    _simulationMode = false;
    
    update();
    return true;
}

void BatteryDesign::update() {
    unsigned long now = millis();
    if (now - _lastUpdate < _updateInterval) {
        return;
    }
    _lastUpdate = now;

    if (_simulationMode) {
        updateSimulation();
    } else {
        updateFromPMU();
    }

    updateWidgetDisplay();
}

void BatteryDesign::updateFromPMU() {
    if (_pmu == nullptr) return;

    _info.voltage = _pmu->getBattVoltage() / 1000.0;
    _info.temperature = _pmu->getTemperature();
    _info.percentage = _pmu->isBatteryConnect() ? _pmu->getBatteryPercent() : 0;
    _info.isCharging = _pmu->isCharging();
    _info.isDischarge = _pmu->isDischarge();
    _info.isVbusIn = _pmu->isVbusIn();
    _info.chargeStatus = _pmu->getChargerStatus();

    _info.warningLow = _info.percentage < 20;
    _info.criticalLow = _info.percentage < 5;

    if (_info.isCharging) {
        _info.timeToFull = estimateTimeToFull();
        _info.timeRemaining = 0;
    } else {
        _info.timeToFull = 0;
        _info.timeRemaining = estimateTimeRemaining();
    }
}

void BatteryDesign::updateSimulation() {
    static int direction = -1;
    _info.percentage += direction;
    
    if (_info.percentage <= 5) {
        direction = 1;
        _info.isCharging = true;
    } else if (_info.percentage >= 100) {
        direction = -1;
        _info.isCharging = false;
    }

    _info.voltage = 3.3 + (_info.percentage * 0.9 / 100.0);
    _info.temperature = 24.0 + (random(0, 20) / 10.0);
    _info.isDischarge = !_info.isCharging;
    _info.warningLow = _info.percentage < 20;
    _info.criticalLow = _info.percentage < 5;
}

int BatteryDesign::estimateTimeToFull() {
    if (_info.percentage >= 95) return 0;
    return (100 - _info.percentage) * 2;
}

int BatteryDesign::estimateTimeRemaining() {
    if (_info.percentage <= 5) return 0;
    return _info.percentage * 3;
}

BatteryDesign::BatteryInfo BatteryDesign::getInfo() {
    return _info;
}

lv_color_t BatteryDesign::getColorForLevel(int percentage) {
    if (percentage >= 80) return lv_color_hex(0x00FF00);
    if (percentage >= 50) return lv_color_hex(0xFFFF00);
    if (percentage >= 30) return lv_color_hex(0xFFA500);
    if (percentage >= 10) return lv_color_hex(0xFF4500);
    return lv_color_hex(0x8B0000);
}

String BatteryDesign::formatTime(int minutes) {
    if (minutes == 0) return "N/A";
    int hours = minutes / 60;
    int mins = minutes % 60;
    return String(hours) + "h " + String(mins) + "m";
}

lv_obj_t* BatteryDesign::createWidget(lv_obj_t* parent, Style style) {
    _mainWidget = lv_obj_create(parent);
    lv_obj_set_size(_mainWidget, 200, 120);
    lv_obj_align(_mainWidget, LV_ALIGN_TOP_RIGHT, -10, 10);
    lv_obj_set_style_bg_color(_mainWidget, lv_color_hex(0x202020), LV_PART_MAIN);
    lv_obj_set_style_border_width(_mainWidget, 2, LV_PART_MAIN);
    lv_obj_set_style_border_color(_mainWidget, lv_color_hex(0x404040), LV_PART_MAIN);
    lv_obj_set_style_radius(_mainWidget, 8, LV_PART_MAIN);

    _percentLabel = lv_label_create(_mainWidget);
    lv_obj_align(_percentLabel, LV_ALIGN_CENTER, 0, -20);
    lv_obj_set_style_text_font(_percentLabel, &lv_font_montserrat_32, LV_PART_MAIN);

    _statusLabel = lv_label_create(_mainWidget);
    lv_obj_align(_statusLabel, LV_ALIGN_CENTER, 0, 15);
    lv_obj_set_style_text_font(_statusLabel, &lv_font_montserrat_14, LV_PART_MAIN);

    _voltageLabel = lv_label_create(_mainWidget);
    lv_obj_align(_voltageLabel, LV_ALIGN_BOTTOM_MID, 0, -5);
    lv_obj_set_style_text_font(_voltageLabel, &lv_font_montserrat_12, LV_PART_MAIN);
    lv_obj_set_style_text_color(_voltageLabel, lv_color_hex(0x808080), LV_PART_MAIN);

    lv_obj_t* detailBtn = lv_button_create(_mainWidget);
    lv_obj_set_size(detailBtn, 60, 30);
    lv_obj_align(detailBtn, LV_ALIGN_BOTTOM_RIGHT, -5, -5);
    lv_obj_add_event_cb(detailBtn, detailButtonCallback, LV_EVENT_CLICKED, this);

    lv_obj_t* btnLabel = lv_label_create(detailBtn);
    lv_label_set_text(btnLabel, "Info");
    lv_obj_center(btnLabel);

    updateWidgetDisplay();
    return _mainWidget;
}

void BatteryDesign::updateWidgetDisplay() {
    if (_percentLabel == nullptr) return;

    char buf[64];
    snprintf(buf, sizeof(buf), "%d%%", _info.percentage);
    lv_label_set_text(_percentLabel, buf);
    lv_obj_set_style_text_color(_percentLabel, getColorForLevel(_info.percentage), LV_PART_MAIN);

    String status = _info.isCharging ? "Charging" : (_info.isDischarge ? "Discharging" : "Standby");
    lv_label_set_text(_statusLabel, status.c_str());

    snprintf(buf, sizeof(buf), "%.2fV | %.1fC", _info.voltage, _info.temperature);
    lv_label_set_text(_voltageLabel, buf);
}

lv_obj_t* BatteryDesign::createDetailWindow(lv_obj_t* parent) {
    _detailWindow = lv_obj_create(parent);
    lv_obj_set_size(_detailWindow, 380, 450);
    lv_obj_center(_detailWindow);
    lv_obj_set_style_bg_color(_detailWindow, lv_color_hex(0x1a1a1a), LV_PART_MAIN);
    lv_obj_set_style_border_width(_detailWindow, 3, LV_PART_MAIN);
    lv_obj_set_style_border_color(_detailWindow, lv_color_hex(0x00ff00), LV_PART_MAIN);
    lv_obj_set_style_radius(_detailWindow, 10, LV_PART_MAIN);

    lv_obj_t* title = lv_label_create(_detailWindow);
    lv_label_set_text(title, "Battery Details");
    lv_obj_align(title, LV_ALIGN_TOP_MID, 0, 10);
    lv_obj_set_style_text_font(title, &lv_font_montserrat_20, LV_PART_MAIN);
    lv_obj_set_style_text_color(title, lv_color_hex(0x00ff00), LV_PART_MAIN);

    lv_obj_t* infoLabel = lv_label_create(_detailWindow);
    lv_obj_align(infoLabel, LV_ALIGN_TOP_LEFT, 20, 50);
    lv_obj_set_style_text_font(infoLabel, &lv_font_montserrat_14, LV_PART_MAIN);

    char buf[512];
    const char* chargeStatusStr = "Unknown";
    if (_pmu) {
        switch (_pmu->getChargerStatus()) {
            case XPOWERS_AXP2101_CHG_TRI_STATE: chargeStatusStr = "Trickle Charge"; break;
            case XPOWERS_AXP2101_CHG_PRE_STATE: chargeStatusStr = "Pre-charge"; break;
            case XPOWERS_AXP2101_CHG_CC_STATE: chargeStatusStr = "Constant Current"; break;
            case XPOWERS_AXP2101_CHG_CV_STATE: chargeStatusStr = "Constant Voltage"; break;
            case XPOWERS_AXP2101_CHG_DONE_STATE: chargeStatusStr = "Charge Complete"; break;
            case XPOWERS_AXP2101_CHG_STOP_STATE: chargeStatusStr = "Not Charging"; break;
        }
    }

    snprintf(buf, sizeof(buf),
        "Percentage: %d%%\n"
        "Voltage: %.2fV\n"
        "Temperature: %.1fC\n"
        "Status: %s\n"
        "Charge State: %s\n"
        "USB Connected: %s\n"
        "Time to Full: %s\n"
        "Time Remaining: %s\n"
        "Warning Low: %s\n"
        "Critical Low: %s\n"
        "Mode: %s",
        _info.percentage,
        _info.voltage,
        _info.temperature,
        _info.isCharging ? "Charging" : (_info.isDischarge ? "Discharging" : "Standby"),
        chargeStatusStr,
        _info.isVbusIn ? "Yes" : "No",
        formatTime(_info.timeToFull).c_str(),
        formatTime(_info.timeRemaining).c_str(),
        _info.warningLow ? "Yes" : "No",
        _info.criticalLow ? "Yes" : "No",
        _simulationMode ? "Simulation" : "Live"
    );
    lv_label_set_text(infoLabel, buf);

    lv_obj_t* closeBtn = lv_button_create(_detailWindow);
    lv_obj_set_size(closeBtn, 100, 40);
    lv_obj_align(closeBtn, LV_ALIGN_BOTTOM_MID, 0, -10);
    lv_obj_add_event_cb(closeBtn, closeButtonCallback, LV_EVENT_CLICKED, this);

    lv_obj_t* closeBtnLabel = lv_label_create(closeBtn);
    lv_label_set_text(closeBtnLabel, "Close");
    lv_obj_center(closeBtnLabel);

    return _detailWindow;
}

lv_obj_t* BatteryDesign::createColorLegend(lv_obj_t* parent) {
    lv_obj_t* legend = lv_obj_create(parent);
    lv_obj_set_size(legend, 180, 200);
    lv_obj_align(legend, LV_ALIGN_BOTTOM_LEFT, 10, -10);
    lv_obj_set_style_bg_color(legend, lv_color_hex(0x202020), LV_PART_MAIN);

    lv_obj_t* title = lv_label_create(legend);
    lv_label_set_text(title, "Color Legend");
    lv_obj_align(title, LV_ALIGN_TOP_MID, 0, 5);
    lv_obj_set_style_text_font(title, &lv_font_montserrat_14, LV_PART_MAIN);

    const char* levels[] = {"80-100%", "50-79%", "30-49%", "10-29%", "0-9%"};
    lv_color_t colors[] = {
        lv_color_hex(0x00FF00), lv_color_hex(0xFFFF00),
        lv_color_hex(0xFFA500), lv_color_hex(0xFF4500),
        lv_color_hex(0x8B0000)
    };

    for (int i = 0; i < 5; i++) {
        lv_obj_t* indicator = lv_obj_create(legend);
        lv_obj_set_size(indicator, 20, 20);
        lv_obj_align(indicator, LV_ALIGN_TOP_LEFT, 10, 35 + i * 30);
        lv_obj_set_style_bg_color(indicator, colors[i], LV_PART_MAIN);
        lv_obj_set_style_radius(indicator, 10, LV_PART_MAIN);

        lv_obj_t* label = lv_label_create(legend);
        lv_label_set_text(label, levels[i]);
        lv_obj_align(label, LV_ALIGN_TOP_LEFT, 40, 35 + i * 30 + 2);
        lv_obj_set_style_text_font(label, &lv_font_montserrat_12, LV_PART_MAIN);
    }

    return legend;
}

void BatteryDesign::detailButtonCallback(lv_event_t* e) {
    BatteryDesign* self = (BatteryDesign*)lv_event_get_user_data(e);
    if (self && self->_detailWindow == nullptr) {
        self->createDetailWindow(lv_screen_active());
    }
}

void BatteryDesign::closeButtonCallback(lv_event_t* e) {
    BatteryDesign* self = (BatteryDesign*)lv_event_get_user_data(e);
    if (self && self->_detailWindow) {
        lv_obj_delete(self->_detailWindow);
        self->_detailWindow = nullptr;
    }
}

#ifndef AMOLEDBrightness_h
#define AMOLEDBrightness_h

#include <Arduino.h>
#include <lvgl.h>

class AMOLEDBrightness {
public:
    // Constructor
    AMOLEDBrightness();
    
    // Initialization methods
    bool begin();
    bool begin(lv_obj_t* screen);
    
    // Basic brightness control
    void setBrightness(uint8_t level);
    uint8_t getBrightness() const;
    void toggle();
    
    // Preset controls
    void setPresetLow();
    void setPresetMedium();
    void setPresetHigh();
    void setPresetOff();
    void setPresetMax();
    void cyclePresets();
    
    // Animation controls
    void fadeTo(uint8_t target, uint16_t durationMs = 500);
    void fadeIn(uint16_t durationMs = 300);
    void fadeOut(uint16_t durationMs = 300);
    void pulse(uint8_t minLevel = 20, uint8_t maxLevel = 80, uint16_t durationMs = 1000);
    
    // State control
    bool isOn() const;
    bool isAnimating() const;
    void saveCurrentLevel();
    void restoreSavedLevel();
    
    // Advanced controls
    void setDimmingStyle(lv_style_t* style);
    void setInverted(bool inverted);
    void setAutoDim(uint16_t timeoutMs, uint8_t dimLevel);
    void stopAutoDim();
    
    // Utility methods
    static uint8_t convertToOpacity(uint8_t brightness);
    static uint8_t convertToBrightness(uint8_t opacity);
    
    // Event handler
    static void setBrightnessChangeCallback(void (*callback)(uint8_t));
    
private:
    lv_obj_t* _screen;
    lv_obj_t* _overlay;
    uint8_t _currentBrightness;
    uint8_t _previousBrightness;
    uint8_t _savedBrightness;
    bool _isOn;
    bool _inverted;
    
    // Animation variables
    bool _isAnimating;
    uint8_t _animStartLevel;
    uint8_t _animTargetLevel;
    unsigned long _animStartTime;
    uint16_t _animDuration;
    
    // Auto dim variables
    bool _autoDimEnabled;
    uint16_t _autoDimTimeout;
    uint8_t _autoDimLevel;
    unsigned long _lastActivityTime;
    
    // Callback
    static void (*_brightnessChangeCallback)(uint8_t);
    
    void createOverlay();
    void updateOverlay(uint8_t brightness);
    void startAnimation(uint8_t startLevel, uint8_t targetLevel, uint16_t duration);
    void updateAnimation();
    float easeInOutCubic(float t);
    
    // LVGL event wrapper
    static void animTimerCallback(lv_timer_t* timer);
    lv_timer_t* _animTimer;
};

#endif

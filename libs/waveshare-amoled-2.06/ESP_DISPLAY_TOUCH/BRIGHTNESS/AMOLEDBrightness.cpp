#include "AMOLEDBrightness.h"

// Static member initialization
void (*AMOLEDBrightness::_brightnessChangeCallback)(uint8_t) = nullptr;

// Constructor
AMOLEDBrightness::AMOLEDBrightness() 
    : _screen(nullptr), _overlay(nullptr), _currentBrightness(75), 
      _previousBrightness(75), _savedBrightness(75), _isOn(true),
      _inverted(false), _isAnimating(false), _animStartLevel(0),
      _animTargetLevel(0), _animStartTime(0), _animDuration(0),
      _autoDimEnabled(false), _autoDimTimeout(0), _autoDimLevel(20),
      _lastActivityTime(0), _animTimer(nullptr) {
}

// Begin with default screen
bool AMOLEDBrightness::begin() {
    _screen = lv_screen_active();
    if (!_screen) {
        Serial.println("[AMOLEDBrightness] Error: No active screen found");
        return false;
    }
    return begin(_screen);
}

// Begin with specified screen
bool AMOLEDBrightness::begin(lv_obj_t* screen) {
    if (!screen) {
        Serial.println("[AMOLEDBrightness] Error: Invalid screen");
        return false;
    }
    
    _screen = screen;
    createOverlay();
    updateOverlay(_currentBrightness);
    
    // Create animation timer
    _animTimer = lv_timer_create(animTimerCallback, 16, this); // ~60 FPS
    
    Serial.println("[AMOLEDBrightness] Library initialized");
    return true;
}

// Create the overlay object
void AMOLEDBrightness::createOverlay() {
    if (!_screen) return;
    
    _overlay = lv_obj_create(_screen);
    lv_obj_set_size(_overlay, LV_PCT(100), LV_PCT(100));
    lv_obj_set_pos(_overlay, 0, 0);
    
    // Basic style
    lv_obj_set_style_bg_color(_overlay, lv_color_hex(0x000000), 0);
    lv_obj_set_style_bg_opa(_overlay, LV_OPA_TRANSP, 0);
    lv_obj_set_style_border_width(_overlay, 0, 0);
    
    // Make it non-interactive
    lv_obj_clear_flag(_overlay, LV_OBJ_FLAG_CLICKABLE);
    lv_obj_clear_flag(_overlay, LV_OBJ_FLAG_SCROLLABLE);
    
    // Always on top
    lv_obj_move_foreground(_overlay);
}

// Set brightness (0-100)
void AMOLEDBrightness::setBrightness(uint8_t level) {
    if (level > 100) level = 100;
    
    _previousBrightness = _currentBrightness;
    _currentBrightness = level;
    _isOn = (level > 0);
    
    // Stop any ongoing animation
    _isAnimating = false;
    
    updateOverlay(level);
    
    // Update auto-dim timer
    if (level > 0) {
        _lastActivityTime = millis();
    }
    
    // Call callback if set
    if (_brightnessChangeCallback) {
        _brightnessChangeCallback(level);
    }
    
    Serial.printf("[AMOLEDBrightness] Brightness set to %d%%\n", level);
}

// Get current brightness
uint8_t AMOLEDBrightness::getBrightness() const {
    return _currentBrightness;
}

// Toggle between 0 and previous non-zero brightness
void AMOLEDBrightness::toggle() {
    if (_isOn) {
        _previousBrightness = _currentBrightness;
        setBrightness(0);
    } else {
        uint8_t restoreLevel = (_previousBrightness > 0) ? _previousBrightness : 75;
        setBrightness(restoreLevel);
    }
}

// Preset: Low (25%)
void AMOLEDBrightness::setPresetLow() {
    setBrightness(25);
}

// Preset: Medium (50%)
void AMOLEDBrightness::setPresetMedium() {
    setBrightness(50);
}

// Preset: High (75%)
void AMOLEDBrightness::setPresetHigh() {
    setBrightness(75);
}

// Preset: Off (0%)
void AMOLEDBrightness::setPresetOff() {
    setBrightness(0);
}

// Preset: Max (100%)
void AMOLEDBrightness::setPresetMax() {
    setBrightness(100);
}

// Cycle through presets
void AMOLEDBrightness::cyclePresets() {
    static uint8_t presets[] = {0, 25, 50, 75, 100};
    static uint8_t index = 0;
    
    index = (index + 1) % 5;
    setBrightness(presets[index]);
}

// Fade to target brightness
void AMOLEDBrightness::fadeTo(uint8_t target, uint16_t durationMs) {
    if (target > 100) target = 100;
    
    startAnimation(_currentBrightness, target, durationMs);
    
    Serial.printf("[AMOLEDBrightness] Fading to %d%% over %dms\n", target, durationMs);
}

// Fade in from current to 100%
void AMOLEDBrightness::fadeIn(uint16_t durationMs) {
    fadeTo(100, durationMs);
}

// Fade out from current to 0%
void AMOLEDBrightness::fadeOut(uint16_t durationMs) {
    fadeTo(0, durationMs);
}

// Pulse animation between min and max
void AMOLEDBrightness::pulse(uint8_t minLevel, uint8_t maxLevel, uint16_t durationMs) {
    if (minLevel > 100) minLevel = 100;
    if (maxLevel > 100) maxLevel = 100;
    
    // Pulse by animating to min, then to max, repeatedly
    static bool pulseToMin = true;
    
    if (pulseToMin) {
        fadeTo(minLevel, durationMs);
    } else {
        fadeTo(maxLevel, durationMs);
    }
    
    pulseToMin = !pulseToMin;
}

// Check if display is on
bool AMOLEDBrightness::isOn() const {
    return _isOn;
}

// Check if animation is running
bool AMOLEDBrightness::isAnimating() const {
    return _isAnimating;
}

// Save current brightness level
void AMOLEDBrightness::saveCurrentLevel() {
    _savedBrightness = _currentBrightness;
    Serial.printf("[AMOLEDBrightness] Saved level: %d%%\n", _savedBrightness);
}

// Restore saved brightness level
void AMOLEDBrightness::restoreSavedLevel() {
    setBrightness(_savedBrightness);
}

// Set custom dimming style
void AMOLEDBrightness::setDimmingStyle(lv_style_t* style) {
    if (_overlay && style) {
        lv_obj_add_style(_overlay, style, 0);
    }
}

// Invert the dimming (0% = dark, 100% = bright)
void AMOLEDBrightness::setInverted(bool inverted) {
    _inverted = inverted;
    updateOverlay(_currentBrightness);
}

// Enable auto-dim after timeout
void AMOLEDBrightness::setAutoDim(uint16_t timeoutMs, uint8_t dimLevel) {
    _autoDimEnabled = true;
    _autoDimTimeout = timeoutMs;
    _autoDimLevel = (dimLevel > 100) ? 100 : dimLevel;
    _lastActivityTime = millis();
    
    Serial.printf("[AMOLEDBrightness] Auto-dim enabled: %dms -> %d%%\n", timeoutMs, dimLevel);
}

// Disable auto-dim
void AMOLEDBrightness::stopAutoDim() {
    _autoDimEnabled = false;
    Serial.println("[AMOLEDBrightness] Auto-dim disabled");
}

// Convert brightness (0-100) to opacity (0-255)
uint8_t AMOLEDBrightness::convertToOpacity(uint8_t brightness) {
    if (brightness > 100) brightness = 100;
    return (100 - brightness) * 255 / 100;
}

// Convert opacity (0-255) to brightness (0-100)
uint8_t AMOLEDBrightness::convertToBrightness(uint8_t opacity) {
    if (opacity > 255) opacity = 255;
    return 100 - (opacity * 100 / 255);
}

// Set callback for brightness changes
void AMOLEDBrightness::setBrightnessChangeCallback(void (*callback)(uint8_t)) {
    _brightnessChangeCallback = callback;
}

// Update the overlay with new brightness
void AMOLEDBrightness::updateOverlay(uint8_t brightness) {
    if (!_overlay) return;
    
    uint8_t opacity = convertToOpacity(brightness);
    if (_inverted) {
        opacity = 255 - opacity;
    }
    
    lv_obj_set_style_bg_opa(_overlay, opacity, 0);
    lv_obj_move_foreground(_overlay);
}

// Start animation
void AMOLEDBrightness::startAnimation(uint8_t startLevel, uint8_t targetLevel, uint16_t duration) {
    _animStartLevel = startLevel;
    _animTargetLevel = targetLevel;
    _animStartTime = millis();
    _animDuration = duration;
    _isAnimating = true;
}

// Update animation progress
void AMOLEDBrightness::updateAnimation() {
    if (!_isAnimating) return;
    
    unsigned long elapsed = millis() - _animStartTime;
    
    if (elapsed >= _animDuration) {
        // Animation complete
        _currentBrightness = _animTargetLevel;
        _isOn = (_currentBrightness > 0);
        _isAnimating = false;
        updateOverlay(_currentBrightness);
        
        if (_brightnessChangeCallback) {
            _brightnessChangeCallback(_currentBrightness);
        }
        return;
    }
    
    // Calculate progress with easing
    float progress = (float)elapsed / (float)_animDuration;
    float easedProgress = easeInOutCubic(progress);
    
    // Calculate current brightness
    int16_t diff = _animTargetLevel - _animStartLevel;
    // diff is negative on a fade down. Do the sum in a signed type and clamp
    // before narrowing, because casting a negative float to uint8_t is undefined.
    int level = (int)_animStartLevel + (int)(diff * easedProgress);
    if (level < 0) level = 0;
    if (level > 100) level = 100;
    uint8_t currentLevel = (uint8_t)level;
    
    // Keep the level on screen current, so getBrightness(), retargeting and toggle() see it
    _currentBrightness = currentLevel;

    // Update display
    updateOverlay(currentLevel);
}

// Easing function for smooth animations
float AMOLEDBrightness::easeInOutCubic(float t) {
    if (t < 0.5f) {
        return 4.0f * t * t * t;
    } else {
        float f = ((2.0f * t) - 2.0f);
        return 0.5f * f * f * f + 1.0f;
    }
}

// LVGL timer callback (static wrapper)
void AMOLEDBrightness::animTimerCallback(lv_timer_t* timer) {
    AMOLEDBrightness* instance = (AMOLEDBrightness*)lv_timer_get_user_data(timer);
    if (instance) {
        instance->updateAnimation();
        
        // Handle auto-dim
        if (instance->_autoDimEnabled && instance->_isOn) {
            unsigned long currentTime = millis();
            if (currentTime - instance->_lastActivityTime > instance->_autoDimTimeout) {
                instance->fadeTo(instance->_autoDimLevel, 1000);
                instance->_autoDimEnabled = false; // Only dim once
            }
        }
    }
}

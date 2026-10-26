#pragma once
#include <Arduino.h>
#include <Wire.h>
#include "Arduino_IIC.h"
#include "Arduino_IIC_Chip.h"
#include "pin_config.h"
#include <lvgl.h>
#include <memory>

class TouchClass {
public:
    TouchClass() {
        instance = this;
    }

    void on() {
        Serial.println("=== TOUCH INIT START ===");
        delay(100);

        // Step 1: power-up and reset the touch IC
        pinMode(TP_RESET, OUTPUT);
        digitalWrite(TP_RESET, LOW);
        delay(10);
        digitalWrite(TP_RESET, HIGH);
        delay(100);

        pinMode(TP_INT, INPUT_PULLUP);
        delay(50);

        // Step 2: start I2C
        Wire.begin(IIC_SDA, IIC_SCL, 100000); // Reduced speed for stability
        Serial.printf("I2C begin on SDA=%d SCL=%d\n", IIC_SDA, IIC_SCL);
        Wire.setTimeOut(1000); // Set I2C timeout

        // Step 3: scan the I2C bus
        Serial.println("Scanning I2C bus for touch controller...");
        bool found = false;
        uint8_t detectedAddr = 0x00;
        for (uint8_t addr = 1; addr < 127; addr++) {
            Wire.beginTransmission(addr);
            if (Wire.endTransmission() == 0) {
                Serial.printf("I2C device found at 0x%02X\n", addr);
                if (addr == FT3168_DEVICE_ADDRESS || addr == 0x38) {
                    found = true;
                    detectedAddr = addr;
                    break; // Stop at first found device
                }
            }
        }

        if (!found) {
            Serial.println("⚠️ No touch controller found on I2C!");
            // Continue anyway, we'll use fallback
            detectedAddr = FT3168_DEVICE_ADDRESS;
        }

        // Step 4: create bus + device objects
        IIC_Bus = std::make_shared<Arduino_HWIIC>(IIC_SDA, IIC_SCL, &Wire);
        FT3168 = std::make_unique<Arduino_FT3x68>(
            IIC_Bus,
            detectedAddr,
            DRIVEBUS_DEFAULT_VALUE,
            TP_INT,
            touchInterruptStatic
        );

        // Step 5: try to initialize (but don't block if it fails)
        bool init_success = false;
        for (int i = 0; i < 3; i++) {
            if (FT3168->begin()) {
                init_success = true;
                Serial.println("✅ Touch controller initialized!");
                break;
            }
            Serial.printf("Touch init attempt %d failed\n", i + 1);
            delay(100);
        }

        if (!init_success) {
            Serial.println("❌ Touch controller init failed, using fallback mode");
            touch_available = false;
        } else {
            touch_available = true;
        }

        // Step 6: register LVGL input device (always do this)
        indev = lv_indev_create();
        lv_indev_set_type(indev, LV_INDEV_TYPE_POINTER);
        lv_indev_set_read_cb(indev, read_cb);
        lv_indev_set_display(indev, lv_display_get_default());

        Serial.printf("LVGL display=%p indev=%p\n", lv_display_get_default(), indev);
        Serial.println("=== TOUCH INIT COMPLETE ===\n");
    }

    lv_indev_t* getIndev() { return indev; }

    bool isTouched() {
        if (!touch_available || !FT3168) return false;
        
        // Quick check without extensive I2C communication
        return digitalRead(TP_INT) == LOW;
    }

    void getTouchPoint(int32_t &x, int32_t &y) {
        if (!touch_available || !FT3168) { 
            x = y = 0; 
            return; 
        }
        x = readRawX();
        y = readRawY();
    }

private:
    std::shared_ptr<Arduino_HWIIC> IIC_Bus;
    std::unique_ptr<Arduino_FT3x68> FT3168;
    lv_indev_t* indev = nullptr;
    static TouchClass* instance;
    bool touch_available = false;
    uint32_t last_touch_time = 0;
    int32_t last_x = 0, last_y = 0;

    static void touchInterruptStatic() {
        // Just record that we had an interrupt
        if (instance) {
            instance->last_touch_time = millis();
        }
    }

    static void read_cb(lv_indev_t* indev, lv_indev_data_t* data) {
        if (!instance) {
            data->state = LV_INDEV_STATE_RELEASED;
            return;
        }

        // If touch is not available, always return released
        if (!instance->touch_available) {
            data->state = LV_INDEV_STATE_RELEASED;
            return;
        }

        // Check if we recently had a touch interrupt
        bool recent_touch = (millis() - instance->last_touch_time < 100);
        
        // Also check the interrupt pin directly (fast)
        bool pin_touch = (digitalRead(TP_INT) == LOW);

        if (recent_touch || pin_touch) {
            // Try to read coordinates with error handling
            int32_t x = instance->readRawXSafe();
            int32_t y = instance->readRawYSafe();
            
            if (x > 10 && y > 10 && x < 4000 && y < 4000) {
                data->state = LV_INDEV_STATE_PRESSED;
                data->point.x = x;
                data->point.y = y;
                instance->last_x = x;
                instance->last_y = y;
                return;
            }
        }

        // No valid touch detected
        data->state = LV_INDEV_STATE_RELEASED;
        data->point.x = instance->last_x;
        data->point.y = instance->last_y;
    }

    int32_t readRawXSafe() {
        if (!FT3168) return 0;
        try {
            return FT3168->IIC_Read_Device_Value(
                FT3168->Arduino_IIC_Touch::Value_Information::TOUCH_COORDINATE_X);
        } catch (...) {
            return 0;
        }
    }

    int32_t readRawYSafe() {
        if (!FT3168) return 0;
        try {
            return FT3168->IIC_Read_Device_Value(
                FT3168->Arduino_IIC_Touch::Value_Information::TOUCH_COORDINATE_Y);
        } catch (...) {
            return 0;
        }
    }

    // Original functions (keep for compatibility)
    int32_t readRawX() {
        return readRawXSafe();
    }

    int32_t readRawY() {
        return readRawYSafe();
    }
};

// Static member definitions
TouchClass* TouchClass::instance = nullptr;

// Global object
extern TouchClass Touch;

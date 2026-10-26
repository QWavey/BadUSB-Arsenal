# UNTESTED CODE, ONLY WORKS AFTER REPLUGGING THE ESP

#ifndef RANDOMIZEPV_H
#define RANDOMIZEPV_H

#include <Arduino.h>
#include "USB.h"

#if CONFIG_TINYUSB_ENABLED

#include <SPIFFS.h>
#include <ArduinoJson.h>

class RandomizePV {
private:
    struct Config {
        String vid = "0x303a";
        String pid = "0x0002";
        String mfr = "Espressif";
        String prod = "ESP32-S3";
        String url = "https://espressif.github.io/arduino-esp32/webusb.html";
        bool randomize = false;
    } cfg;

    String genRandomHex() {
        char buf[7];
        sprintf(buf, "0x%04x", (uint16_t)(esp_random() & 0xFFFF));
        return String(buf);
    }

    void loadConfig() {
        if (!SPIFFS.begin(true)) {
            Serial.println("SPIFFS Mount Failed");
            return;
        }
        
        if (!SPIFFS.exists("/usb_config.json")) {
            Serial.println("No saved config found, using defaults");
            return;
        }
        
        File file = SPIFFS.open("/usb_config.json", "r");
        if (!file) {
            Serial.println("Failed to open config file");
            return;
        }
        
        StaticJsonDocument<512> doc;
        DeserializationError error = deserializeJson(doc, file);
        file.close();
        
        if (error) {
            Serial.println("Failed to parse config file");
            return;
        }
        
        cfg.vid = doc["vid"] | cfg.vid;
        cfg.pid = doc["pid"] | cfg.pid;
        cfg.mfr = doc["mfr"] | cfg.mfr;
        cfg.prod = doc["prod"] | cfg.prod;
        cfg.url = doc["url"] | cfg.url;
        cfg.randomize = doc["randomize"] | false;
        
        Serial.println("Loaded USB configuration from file");
    }

    void saveConfig() {
        if (!SPIFFS.begin(true)) {
            Serial.println("SPIFFS Mount Failed");
            return;
        }
        
        File file = SPIFFS.open("/usb_config.json", "w");
        if (!file) {
            Serial.println("Failed to create config file");
            return;
        }
        
        StaticJsonDocument<512> doc;
        doc["vid"] = cfg.vid;
        doc["pid"] = cfg.pid;
        doc["mfr"] = cfg.mfr;
        doc["prod"] = cfg.prod;
        doc["url"] = cfg.url;
        doc["randomize"] = cfg.randomize;
        
        if (serializeJson(doc, file) == 0) {
            Serial.println("Failed to write config file");
        } else {
            Serial.println("USB configuration saved");
        }
        
        file.close();
    }

public:
    RandomizePV() {
        // Constructor
    }
    
    // Configuration methods
    void setVID(const String& vid) {
        if (vid.startsWith("0x")) {
            cfg.vid = vid;
        }
    }
    
    void setPID(const String& pid) {
        if (pid.startsWith("0x")) {
            cfg.pid = pid;
        }
    }
    
    void setManufacturer(const String& manufacturer) {
        cfg.mfr = manufacturer;
    }
    
    void setProduct(const String& product) {
        cfg.prod = product;
    }
    
    void setWebUSBURL(const String& url) {
        cfg.url = url;
    }
    
    void setRandomize(bool randomize) {
        cfg.randomize = randomize;
    }
    
    // Get current configuration
    String getVID() { return cfg.vid; }
    String getPID() { return cfg.pid; }
    String getManufacturer() { return cfg.mfr; }
    String getProduct() { return cfg.prod; }
    String getWebUSBURL() { return cfg.url; }
    bool getRandomize() { return cfg.randomize; }
    
    // Generate random values
    void generateRandomVID() {
        cfg.vid = genRandomHex();
        Serial.printf("Generated random VID: %s\n", cfg.vid.c_str());
    }
    
    void generateRandomPID() {
        cfg.pid = genRandomHex();
        Serial.printf("Generated random PID: %s\n", cfg.pid.c_str());
    }
    
    void generateRandomBoth() {
        generateRandomVID();
        generateRandomPID();
    }
    
    // Apply configuration to USB
    void apply() {
        // Apply randomization if enabled
        if (cfg.randomize) {
            generateRandomBoth();
            saveConfig(); // Save the randomized values
        }
        
        // Convert hex strings to integers
        uint16_t vid = (uint16_t)strtol(cfg.vid.c_str(), NULL, 16);
        uint16_t pid = (uint16_t)strtol(cfg.pid.c_str(), NULL, 16);
        
        // Apply configuration to USB
        if (!::USB) {
            ::USB.VID(vid);
            ::USB.PID(pid);
            ::USB.manufacturerName(cfg.mfr.c_str());
            ::USB.productName(cfg.prod.c_str());
            ::USB.webUSB(true);
            ::USB.webUSBURL(cfg.url.c_str());
            ::USB.begin();
            
            Serial.println("USB configuration applied:");
            Serial.printf("  VID: %s (0x%04X)\n", cfg.vid.c_str(), vid);
            Serial.printf("  PID: %s (0x%04X)\n", cfg.pid.c_str(), pid);
            Serial.printf("  Manufacturer: %s\n", cfg.mfr.c_str());
            Serial.printf("  Product: %s\n", cfg.prod.c_str());
            Serial.printf("  WebUSB URL: %s\n", cfg.url.c_str());
            Serial.printf("  Randomize on boot: %s\n", cfg.randomize ? "Yes" : "No");
        }
    }
    
    // Quick setup methods
    void begin() {
        loadConfig();
        apply();
    }
    
    void begin(bool randomize) {
        cfg.randomize = randomize;
        begin();
    }
    
    void begin(const String& vid, const String& pid, const String& mfr = "Espressif", 
               const String& prod = "ESP32-S3", const String& url = "https://espressif.github.io/arduino-esp32/webusb.html") {
        setVID(vid);
        setPID(pid);
        setManufacturer(mfr);
        setProduct(prod);
        setWebUSBURL(url);
        saveConfig();
        apply();
    }
    
    // Utility methods
    void printConfig() {
        Serial.println("\n=== Current USB Configuration ===");
        Serial.printf("VID: %s\n", cfg.vid.c_str());
        Serial.printf("PID: %s\n", cfg.pid.c_str());
        Serial.printf("Manufacturer: %s\n", cfg.mfr.c_str());
        Serial.printf("Product: %s\n", cfg.prod.c_str());
        Serial.printf("WebUSB URL: %s\n", cfg.url.c_str());
        Serial.printf("Randomize on boot: %s\n", cfg.randomize ? "Yes" : "No");
        Serial.println("=================================\n");
    }
    
    void resetToDefaults() {
        cfg.vid = "0x303a";
        cfg.pid = "0x0002";
        cfg.mfr = "Espressif";
        cfg.prod = "ESP32-S3";
        cfg.url = "https://espressif.github.io/arduino-esp32/webusb.html";
        cfg.randomize = false;
        saveConfig();
        Serial.println("Reset to default configuration");
    }
};

// Create global instance
RandomizePV USBConfig;

#endif // CONFIG_TINYUSB_ENABLED
#endif // RANDOMIZEPV_H

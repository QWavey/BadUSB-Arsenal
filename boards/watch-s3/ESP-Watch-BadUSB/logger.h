#ifndef LOGGER_H
#define LOGGER_H

#include <Arduino.h>
#include <vector>
#include <freertos/FreeRTOS.h>
#include <freertos/semphr.h>

class Logger {
public:
    static std::vector<String> logs;

    static void log(const String& msg, const String& level = "info") {
        String entry = "[" + level + "] " + msg;
        Serial.println(entry);

        // Thread-safe: use a mutex if called from tasks
        if (_mutex == nullptr) {
            _mutex = xSemaphoreCreateMutex();
        }
        if (xSemaphoreTake(_mutex, pdMS_TO_TICKS(50)) == pdTRUE) {
            logs.push_back(entry);
            // Keep last 80 entries only
            while (logs.size() > 80) {
                logs.erase(logs.begin());
            }
            xSemaphoreGive(_mutex);
        }
    }

private:
    static SemaphoreHandle_t _mutex;
};

inline std::vector<String>  Logger::logs;
inline SemaphoreHandle_t    Logger::_mutex = nullptr;

#endif // LOGGER_H

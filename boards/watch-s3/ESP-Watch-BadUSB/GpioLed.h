#ifndef GPIO_LED_H
#define GPIO_LED_H

#include <Arduino.h>
#include "WatchUI.h"

// Watch-port shim. The Key firmware drives a single GPIO LED via a small
// subset of the Adafruit_NeoPixel API. The watch has no addressable LED
// (or any status LED at all) — we forward every colour push to WatchUI
// which paints a coloured dot on the AMOLED. Same call sites, same
// semantics; blink modes still work because LEDManager toggles the
// pixel colour between the blink colour and black, and each show() lands
// as a fresh watchUiSetLed().
class GpioLed {
public:
  GpioLed(uint16_t /*numPixels*/, int16_t /*pin*/, uint16_t /*type*/ = 0)
      : _brightness(255), _pending(0) {}

  void begin() {}   // no GPIO to configure

  void setBrightness(uint8_t b) { _brightness = b; }

  static uint32_t Color(uint8_t r, uint8_t g, uint8_t b) {
    return ((uint32_t)r << 16) | ((uint32_t)g << 8) | (uint32_t)b;
  }

  void setPixelColor(uint16_t /*index*/, uint32_t color) { _pending = color; }

  void show() {
    uint8_t r = (_pending >> 16) & 0xFF;
    uint8_t g = (_pending >> 8) & 0xFF;
    uint8_t b =  _pending        & 0xFF;
    r = (uint16_t)r * _brightness / 255;
    g = (uint16_t)g * _brightness / 255;
    b = (uint16_t)b * _brightness / 255;
    uint32_t scaled = ((uint32_t)r << 16) | ((uint32_t)g << 8) | (uint32_t)b;
    // LEDManager rewrites setPixelColor/show alternately for blink; we
    // always render solid here, so the on-screen dot reflects whatever
    // the last colour was.
    watchUiSetLed(scaled, 0);
  }

private:
  uint8_t  _brightness;
  uint32_t _pending;
};

#endif // GPIO_LED_H

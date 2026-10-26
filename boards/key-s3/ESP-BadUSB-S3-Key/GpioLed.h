#ifndef GPIO_LED_H
#define GPIO_LED_H

#include <Arduino.h>

// Drop-in replacement for the small subset of the Adafruit_NeoPixel API
// that this project uses, for boards with a single plain (non-addressable)
// LED wired to a GPIO through a resistor (e.g. ESP32-S3-Dongle/Key: GPIO1,
// active-HIGH blue LED). Any non-black color turns the LED on; brightness
// and color intensity are honored via PWM (max RGB channel = intensity).
class GpioLed {
public:
  // Signature mirrors Adafruit_NeoPixel(numPixels, pin, type) so existing
  // construction/calls compile unchanged. numPixels/type are ignored.
  GpioLed(uint16_t /*numPixels*/, int16_t pin, uint16_t /*type*/ = 0)
      : _pin(pin), _brightness(255), _pending(0), _attached(false) {}

  void begin() {
    // 5 kHz, 8-bit PWM. ledcAttach auto-allocates a channel (core 3.x API).
    _attached = ledcAttach(_pin, 5000, 8);
    if (!_attached) {                 // fallback for older cores / failure
      pinMode(_pin, OUTPUT);
    }
    _apply(0);
  }

  void setBrightness(uint8_t b) { _brightness = b; }

  // Packs r,g,b into a 32-bit value, same layout as NeoPixel::Color().
  static uint32_t Color(uint8_t r, uint8_t g, uint8_t b) {
    return ((uint32_t)r << 16) | ((uint32_t)g << 8) | (uint32_t)b;
  }

  void setPixelColor(uint16_t /*index*/, uint32_t color) { _pending = color; }

  void show() {
    uint8_t r = (_pending >> 16) & 0xFF;
    uint8_t g = (_pending >> 8) & 0xFF;
    uint8_t b = _pending & 0xFF;
    uint8_t level = r; if (g > level) level = g; if (b > level) level = b;
    // scale by global brightness
    uint8_t duty = (uint16_t)level * _brightness / 255;
    _apply(duty);
  }

private:
  void _apply(uint8_t duty) {
    if (_attached) {
      ledcWrite(_pin, duty);          // active-HIGH: duty 0=off, 255=full
    } else {
      digitalWrite(_pin, duty ? HIGH : LOW);
    }
  }

  int16_t  _pin;
  uint8_t  _brightness;
  uint32_t _pending;
  bool     _attached;
};

#endif // GPIO_LED_H

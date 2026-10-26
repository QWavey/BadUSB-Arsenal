#include "LEDManager.h"

// v4.16 perf: track the last color we actually pushed to the LED so we can
// skip the ~30 us pixels.show() bit-bang when nothing changed. Prior to
// this the else-branch below re-pushed the same color every loop() tick,
// adding jitter to USB HID timing.
static uint32_t g_lastShownColor = 0xFFFFFFFF;  // sentinel that mismatches everything
static void __ledShowIfChanged(uint32_t c) {
  if (c == g_lastShownColor) return;
  g_lastShownColor = c;
  pixels.setPixelColor(0, c);
  pixels.show();
}

void handleLED() {
  if (!ledEnabled) {
    __ledShowIfChanged(pixels.Color(0, 0, 0));
    return;
  }

  unsigned long currentTime = millis();

  if (ledMode == 3) { // Completion blink (orange)
    if (currentTime - lastCompletionBlinkTime >= 200) {
      lastCompletionBlinkTime = currentTime;
      completionBlinkState = !completionBlinkState;
      __ledShowIfChanged(completionBlinkState ? pixels.Color(255, 165, 0) : pixels.Color(0, 0, 0));

      if (!completionBlinkState) {
        completionBlinkCount++;
        if (completionBlinkCount >= 6) {
          ledMode = 0;
          completionBlinkCount = 0;
          setLED(0, 255, 0);
        }
      }
    }
    return;
  }

  if (ledMode == 4) { // Warning mode (orange blinking)
    if (currentTime - lastBlinkTime >= 300) {
      lastBlinkTime = currentTime;
      blinkState = !blinkState;
      __ledShowIfChanged(blinkState ? pixels.Color(255, 165, 0) : pixels.Color(0, 0, 0));
    }
    return;
  }

  if (blinkingEnabled || ledMode == 1 || ledMode == 2) {
    if (currentTime - lastBlinkTime >= blinkInterval) {
      lastBlinkTime = currentTime;
      blinkState = !blinkState;
      __ledShowIfChanged(blinkState ? pixels.Color(currentR, currentG, currentB) : pixels.Color(0, 0, 0));
    }
  } else {
    // v4.16: only push when color changed - was bit-banging every loop tick.
    __ledShowIfChanged(pixels.Color(currentR, currentG, currentB));
  }
}

void setLED(int r, int g, int b) {
  if (!ledEnabled) return;
  currentR = r;
  currentG = g;
  currentB = b;
  pixels.setPixelColor(0, pixels.Color(r, g, b));
  pixels.show();
  ledMode = 0;
  blinkingEnabled = false;
  completionBlinkCount = 0;
}

void setLEDMode(int mode) {
  ledMode = mode;
  lastBlinkTime = millis();
  blinkState = false;
  completionBlinkCount = 0;

  if (mode == 0) {
    currentR = 0;
    currentG = 255;
    currentB = 0;
    blinkInterval = 500;
    blinkingEnabled = false;
  } else if (mode == 1) {
    currentR = 0;
    currentG = 255;
    currentB = 0;
    blinkInterval = 50;
    blinkingEnabled = true;
  } else if (mode == 2) {
    currentR = 255;
    currentG = 0;
    currentB = 0;
    blinkInterval = 500;
    blinkingEnabled = true;
  } else if (mode == 3) {
    lastCompletionBlinkTime = millis();
    completionBlinkState = true;
    completionBlinkCount = 0;
  } else if (mode == 4) {
    currentR = 255;
    currentG = 165;
    currentB = 0;
    blinkInterval = 300;
    blinkingEnabled = true;
  } else if (mode == 5) { // Amber static
    currentR = 255;
    currentG = 127;
    currentB = 0;
    blinkInterval = 500;
    blinkingEnabled = false;
  } else if (mode == 6) { // Blue fast-blink — .espkg update in progress
    currentR = 0;
    currentG = 0;
    currentB = 255;
    blinkInterval = 120;
    blinkingEnabled = true;
  } else if (mode == 7) { // v4.14: Blue medium-blink — button armed (3-10s hold)
    currentR = 0;
    currentG = 0;
    currentB = 255;
    blinkInterval = 200;
    blinkingEnabled = true;
  }
}

void showCompletionBlink() {
  setLEDMode(3);
}

void showWarningBlink() {
  setLEDMode(4);
}

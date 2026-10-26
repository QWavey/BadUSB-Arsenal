#include <stdint.h>

#include <avr32/io.h>

/*
 * diagnose.c - LED diagnostic blink patterns
 * Provides visual feedback for fatal errors.
 */

/* Simple delay loop */
static void delay_cycles(uint32_t cycles) {
  while (cycles--) {
    __asm__ __volatile__("nop");
  }
}

/*
 * Blinks the red LED `code` times, pauses, then repeats.
 *
 * The red LED is on PA08 and is ACTIVE-LOW (drive the pin low = LED on), the
 * same wiring main.c uses via LED_PIN_RED.  A previous version drove PA13 with
 * active-high logic — but PA13 is the BUTTON pin (BTN_PIN), not an LED, so it
 * blinked nothing and, worse, reconfigured the button line as a driven output.
 */
#define DIAG_LED_RED_PIN 8   /* PA08, active-low (matches main.c LED_PIN_RED) */

void error_blink(int code) {
  // Configure PA08 as output.
  AVR32_GPIO.port[0].gpers = (1 << DIAG_LED_RED_PIN);
  AVR32_GPIO.port[0].oders = (1 << DIAG_LED_RED_PIN);

  while (1) {
    for (int i = 0; i < code; i++) {
      AVR32_GPIO.port[0].ovrc = (1 << DIAG_LED_RED_PIN); // active-low: LED ON
      delay_cycles(1000000);
      AVR32_GPIO.port[0].ovrs = (1 << DIAG_LED_RED_PIN); // active-low: LED OFF
      delay_cycles(1000000);
    }
    delay_cycles(4000000); // Long pause
  }
}
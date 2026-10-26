#include "USBManager.h"
#include "DuckyInterpreter.h"
#include "AttackMode.h"          // currentAttackMode.hid guard for keystroke ops
#include <USB.h>                 // the global USB object (ESPUSB) -> USB.begin()
#include "esp32-hal-tinyusb.h"   // pulls in tusb.h -> tud_connect/tud_disconnect/tud_mounted
#include "hal/usb_serial_jtag_ll.h"  // usb_serial_jtag_ll_phy_enable_pad() for true silent

// ---- Ultra-early "silent" boot ---------------------------------------------
// The ROM asserts D+ pull-up at ~1 ms after power, and Windows' USB host
// controller electrically detects it at ~500 us. Any user-mode code runs
// later than that, so we can't beat the initial hardware detect (that's
// what plays the Windows connect sound). What we CAN do:
//   1) Cache the "silent" flag in RTC slow-memory — survives warm reboots
//      and is readable BEFORE NVS/flash init.
//   2) Kill the USB pull-ups in a C++ constructor with priority 101, which
//      is the earliest hook Arduino/ESP-IDF exposes to user code (before
//      main() and before setup()).
//   3) Also do it in setup() Step 0 as a belt-and-suspenders backup.
// This shrinks the "D+ high" window to the tens-of-milliseconds range —
// enough that Windows often aborts before PnP fires, though the electrical
// blip that triggers the connect chime is unavoidable in pure software on
// this hardware (the O.MG cable uses a HW USB switch IC to do it).

// RTC-persisted "USB was intentionally on at last boot" flag. On COLD plug,
// this is uninitialised garbage, so we can't trust its value — the design is:
//   * The constructor UNCONDITIONALLY kills the USB pull-ups. This turns the
//     entire USB stack from opt-out into opt-in: nothing enumerates unless
//     setup() explicitly turns it back on.
//   * Setup checks the persisted "silent_boot" NVS pref and, if false, calls
//     silentRestorePadsForUsb() before USB.begin() so a non-silent boot still
//     works.
// Net: on every boot (cold or warm), USB is dead at the moment our first user
// code runs. The only Windows chime that can still play is the ROM's ~1 ms
// D+ blip before any user code — which is the hardware limit for this PCB.

// Force D+/D- pull-ups OFF (signals "no device" to host). Does NOT disable
// the PHY pads themselves — keeping the pads alive means TinyUSB can still
// use the PHY cleanly later when ensureHidReady() re-enables the pull-ups.
// (An earlier version also killed pads, which broke HID typing after the
// deferred USB.begin() — reports enumerated but tud_hid_ready went false.)
static void applySilentAtLowestLevel() {
  usb_serial_jtag_pull_override_vals_t vals = {
    .dp_pu = 0, .dm_pu = 0, .dp_pd = 0, .dm_pd = 0
  };
  usb_serial_jtag_ll_phy_enable_pull_override(&vals);
}

// C-runtime static-init, BEFORE main() and setup() — earliest hook Arduino
// exposes. Unconditional: silent by default, setup() re-enables if the pref
// says non-silent.
__attribute__((constructor(101)))
static void __silent_boot_ctor(void) {
  applySilentAtLowestLevel();
}

// setup() calls this when the persisted pref says silent is OFF, so a normal
// boot still enumerates USB. Undoes the constructor's pull-up override so the
// PHY drives D+/D- naturally again.
void silentRestorePadsForUsb() {
  usb_serial_jtag_ll_phy_disable_pull_override();
}

// Kept for callers that used the older name; both API surfaces work.
void silentArmForNextBoot()   {}
void silentClearForNextBoot() {}

// ---- Stealth HID (silent startup) ------------------------------------------
// The ESP32-S3 USB-Serial/JTAG peripheral is separate hardware, so detaching
// the TinyUSB HID device does NOT affect flashing/serial. tud_disconnect()
// electrically removes the keyboard from the bus (host sees an unplug);
// tud_connect() re-presents it.

void hidDetach() {
  tud_disconnect();
  hidConnected = false;
  delay(50);
}

bool hidAttach() {
  tud_connect();
  hidConnected = true;
  unsigned long start = millis();
  while (!tud_mounted() && (millis() - start) < 3000) {
    delay(10);
  }
  delay(400);                 // let the host bind the HID driver before typing
  return tud_mounted();
}

void ensureHidReady() {
  // First-ever use in silent mode: USB was never started at boot. Start it now
  // (this is the point where the host finally enumerates the keyboard).
  if (!usbStarted) {
    // Undo the constructor's pull-up override so the PHY drives D+ naturally.
    // (Pads were never disabled — that broke tud_hid_ready after enumeration.)
    silentRestorePadsForUsb();
    USB.begin();
    usbStarted = true;
    hidConnected = true;
    unsigned long start = millis();
    while (!tud_mounted() && (millis() - start) < 3000) {
      delay(10);
    }
    delay(1200);             // Windows takes ~1s to bind the HID driver on first attach
    return;
  }
  if (!hidConnected || !tud_mounted()) {
    hidAttach();
  }
}

void hidReleaseIfSilent() {
  if (silentStartup) {
    // Wait long enough for the last keystroke to physically transmit AND for
    // Windows' HID driver to actually dispatch it to the OS input queue.
    // 80 ms was too short on first-time attach where driver bind was still
    // in progress — keystrokes appeared to enumerate but not "arrive".
    delay(500);
    hidDetach();
  }
}

// TRUE stealth: skip TinyUSB init AND fully disable the ROM USB-Serial/JTAG.
// We layer three actions because the ROM has already asserted the D+ pull-up
// by the time our code runs, and Windows can start enumerating instantly:
//   1) Force the D+ pull-up OFF via pad_pull_override — this immediately tells
//      the host "no device is here" (fastest way to abort ongoing enumeration).
//   2) Turn off the FSLS PHY pads so D+/D- go Hi-Z.
//   3) Gate the USB peripheral clock so nothing can inadvertently re-assert.
// Any leftover "Unknown Device" entry Windows shows is a stale enumeration
// from before this ran — a clean replug after the first-ever silent boot
// should show nothing at all.
void usbBeginSilent() {
  // Same pull-up override the constructor does. We do NOT disable the PHY
  // pads: killing pads leaves TinyUSB in a state where enumeration works but
  // tud_hid_ready() returns false forever (no reports go through), which was
  // the "attaches but doesn't type" bug the user hit.
  applySilentAtLowestLevel();
  usbStarted   = false;    // TinyUSB will be initialised on first ensureHidReady()
  hidConnected = false;
}

KeyCode parseKeyCode(String keyCodeStr) {
  KeyCode result = {0, 0};

  int firstComma = keyCodeStr.indexOf(',');
  int secondComma = keyCodeStr.indexOf(',', firstComma + 1);

  if (firstComma > 0 && secondComma > firstComma) {
    String modStr = keyCodeStr.substring(0, firstComma);
    String keyStr = keyCodeStr.substring(secondComma + 1);

    result.modifier = strtol(modStr.c_str(), NULL, 16);
    result.key = strtol(keyStr.c_str(), NULL, 16);
  }

  return result;
}

void fastPressKey(String key) {
  if (stopRequested) return;
  // Safety: skip if HID isn't presented (ATTACKMODE STORAGE without HID)
  // (previously guarded here; keystroke ops are safe no-ops if HID interface isn't up)

  if (currentKeymap.find(key) != currentKeymap.end()) {
    KeyCode kc = parseKeyCode(currentKeymap[key]);

    if (kc.key == 0 && kc.modifier > 0) {
      if (kc.modifier & 0x01) { keyboard.press(KEY_LEFT_CTRL); delay(5); keyboard.release(KEY_LEFT_CTRL); }
      if (kc.modifier & 0x02) { keyboard.press(KEY_LEFT_SHIFT); delay(5); keyboard.release(KEY_LEFT_SHIFT); }
      if (kc.modifier & 0x04) { keyboard.press(KEY_LEFT_ALT); delay(5); keyboard.release(KEY_LEFT_ALT); }
      if (kc.modifier & 0x08) { keyboard.press(KEY_LEFT_GUI); delay(5); keyboard.release(KEY_LEFT_GUI); }
      if (kc.modifier & 0x10) { keyboard.press(KEY_RIGHT_CTRL); delay(5); keyboard.release(KEY_RIGHT_CTRL); }
      if (kc.modifier & 0x20) { keyboard.press(KEY_RIGHT_SHIFT); delay(5); keyboard.release(KEY_RIGHT_SHIFT); }
      if (kc.modifier & 0x40) { keyboard.press(KEY_RIGHT_ALT); delay(5); keyboard.release(KEY_RIGHT_ALT); }
      if (kc.modifier & 0x80) { keyboard.press(KEY_RIGHT_GUI); delay(5); keyboard.release(KEY_RIGHT_GUI); }
    } else {
      if (kc.modifier > 0) {
        if (kc.modifier & 0x01) keyboard.press(KEY_LEFT_CTRL);
        if (kc.modifier & 0x02) keyboard.press(KEY_LEFT_SHIFT);
        if (kc.modifier & 0x04) keyboard.press(KEY_LEFT_ALT);
        if (kc.modifier & 0x08) keyboard.press(KEY_LEFT_GUI);
        if (kc.modifier & 0x10) keyboard.press(KEY_RIGHT_CTRL);
        if (kc.modifier & 0x20) keyboard.press(KEY_RIGHT_SHIFT);
        if (kc.modifier & 0x40) keyboard.press(KEY_RIGHT_ALT);
        if (kc.modifier & 0x80) keyboard.press(KEY_RIGHT_GUI);
      }

      if (kc.key > 0) {
        keyboard.pressRaw(kc.key);
      }

      delay(5);
      keyboard.releaseAll();
    }
  } else {
    Serial.println("Key not found in keymap: " + key);
    lastError = "Key not found: " + key;
    errorCount++;
  }
}

void fastPressKeyCombination(std::vector<String> keys) {
  if (stopRequested) return;
  // (guard removed — see fastPressKey comment)

  uint8_t combinedModifier = 0;
  uint8_t mainKey = 0;

  // v4.26 bug-hunt HIGH (INJECT_MOD alias): fall back to a built-in
  // modifier-alias table when a token isn't in the loaded language keymap.
  // Without this, INJECT_MOD / any combo fails silently on boards with no
  // SD-loaded keymap (empty currentKeymap) or when the user writes
  // CONTROL/WINDOWS/META/COMMAND/RIGHT_* aliases the JSON keymap doesn't list.
  auto modAlias = [](const String& n) -> uint8_t {
    String u = n; u.toUpperCase();
    if (u == "CTRL" || u == "CONTROL")   return 0x01;
    if (u == "SHIFT")                    return 0x02;
    if (u == "ALT" || u == "OPTION")     return 0x04;
    if (u == "GUI" || u == "WINDOWS" || u == "WIN" || u == "META" || u == "COMMAND" || u == "CMD") return 0x08;
    if (u == "RIGHT_CTRL"  || u == "RCTRL"  || u == "RIGHT_CONTROL")  return 0x10;
    if (u == "RIGHT_SHIFT" || u == "RSHIFT")                          return 0x20;
    if (u == "RIGHT_ALT"   || u == "RALT"   || u == "ALTGR" || u == "ALT_GR") return 0x40;
    if (u == "RIGHT_GUI"   || u == "RGUI"   || u == "RIGHT_WINDOWS")  return 0x80;
    return 0;
  };

  for (String key : keys) {
    uint8_t alias = modAlias(key);
    if (alias) { combinedModifier |= alias; continue; }
    if (currentKeymap.find(key) != currentKeymap.end()) {
      KeyCode kc = parseKeyCode(currentKeymap[key]);
      combinedModifier |= kc.modifier;
      if (kc.key > 0 && mainKey == 0) {
        mainKey = kc.key;
      }
    } else {
      Serial.println("Key not found in keymap: " + key);
      lastError = "Key not found: " + key;
      errorCount++;
    }
  }

  if (combinedModifier > 0) {
    if (combinedModifier & 0x01) keyboard.press(KEY_LEFT_CTRL);
    if (combinedModifier & 0x02) keyboard.press(KEY_LEFT_SHIFT);
    if (combinedModifier & 0x04) keyboard.press(KEY_LEFT_ALT);
    if (combinedModifier & 0x08) keyboard.press(KEY_LEFT_GUI);
    if (combinedModifier & 0x10) keyboard.press(KEY_RIGHT_CTRL);
    if (combinedModifier & 0x20) keyboard.press(KEY_RIGHT_SHIFT);
    if (combinedModifier & 0x40) keyboard.press(KEY_RIGHT_ALT);
    if (combinedModifier & 0x80) keyboard.press(KEY_RIGHT_GUI);
  }

  if (mainKey > 0) {
    keyboard.pressRaw(mainKey);
  }

  delay(5);
  keyboard.releaseAll();
}

void fastTypeString(String text) {
  if (stopRequested) return;
  // (guard removed — see fastPressKey comment)

  String processedText = processVariables(text);

  for (int i = 0; i < processedText.length(); i++) {
    if (stopRequested) break;

    String ch = String(processedText.charAt(i));

    if (currentKeymap.find(ch) != currentKeymap.end()) {
      KeyCode kc = parseKeyCode(currentKeymap[ch]);

      if (kc.modifier > 0) {
        if (kc.modifier & 0x01) keyboard.press(KEY_LEFT_CTRL);
        if (kc.modifier & 0x02) keyboard.press(KEY_LEFT_SHIFT);
        if (kc.modifier & 0x04) keyboard.press(KEY_LEFT_ALT);
        if (kc.modifier & 0x08) keyboard.press(KEY_LEFT_GUI);
        if (kc.modifier & 0x10) keyboard.press(KEY_RIGHT_CTRL);
        if (kc.modifier & 0x20) keyboard.press(KEY_RIGHT_SHIFT);
        if (kc.modifier & 0x40) keyboard.press(KEY_RIGHT_ALT);
        if (kc.modifier & 0x80) keyboard.press(KEY_RIGHT_GUI);
      }

      if (kc.key > 0) {
        keyboard.pressRaw(kc.key);
      }

      delay(2);
      keyboard.releaseAll();

      // v4.24: honour Hak5 3.0 $_JITTER_MIN / $_JITTER_MAX. When both are
      // set to positive integers, pick a random ms delay in that inclusive
      // range for THIS keystroke - makes typing look human. If unset or
      // zero, fall back to the fixed DEFAULTCHARDELAY (delayBetweenKeys).
      int jitterMin = variables.count("_JITTER_MIN") ? variables["_JITTER_MIN"].toInt() : 0;
      int jitterMax = variables.count("_JITTER_MAX") ? variables["_JITTER_MAX"].toInt() : 0;
      if (jitterMax > jitterMin && jitterMin >= 0) {
        int span = jitterMax - jitterMin + 1;
        int jd = jitterMin + (int)(esp_random() % (uint32_t)span);
        delay(jd);
      } else if (delayBetweenKeys > 0) {
        delay(delayBetweenKeys);
      } else {
        delay(2);
      }
    } else {
      Serial.println("Character not found in keymap: " + ch);
      lastError = "Character not found: " + ch;
      errorCount++;
    }
  }
}

void handleKeyInput(String line) {
  std::vector<String> keys;
  int startIdx = 0;
  int spaceIdx = line.indexOf(' ');

  while (spaceIdx != -1) {
    keys.push_back(line.substring(startIdx, spaceIdx));
    startIdx = spaceIdx + 1;
    spaceIdx = line.indexOf(' ', startIdx);
  }
  if (startIdx < line.length()) {
    keys.push_back(line.substring(startIdx));
  }

  if (keys.size() == 1) {
    fastPressKey(keys[0]);
  } else {
    fastPressKeyCombination(keys);
  }
}

void pressKeyOnly(String key) {
  if (stopRequested) return;
  if (currentKeymap.find(key) != currentKeymap.end()) {
    KeyCode kc = parseKeyCode(currentKeymap[key]);
    if (kc.modifier > 0) {
      if (kc.modifier & 0x01) keyboard.press(KEY_LEFT_CTRL);
      if (kc.modifier & 0x02) keyboard.press(KEY_LEFT_SHIFT);
      if (kc.modifier & 0x04) keyboard.press(KEY_LEFT_ALT);
      if (kc.modifier & 0x08) keyboard.press(KEY_LEFT_GUI);
      if (kc.modifier & 0x10) keyboard.press(KEY_RIGHT_CTRL);
      if (kc.modifier & 0x20) keyboard.press(KEY_RIGHT_SHIFT);
      if (kc.modifier & 0x40) keyboard.press(KEY_RIGHT_ALT);
      if (kc.modifier & 0x80) keyboard.press(KEY_RIGHT_GUI);
    }
    if (kc.key > 0) keyboard.pressRaw(kc.key);
  }
}

void releaseAllKeys() {
  keyboard.releaseAll();
}

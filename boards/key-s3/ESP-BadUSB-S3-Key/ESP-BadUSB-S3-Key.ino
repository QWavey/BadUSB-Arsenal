#include <Arduino.h>
#include <WiFi.h>
#include <WebServer.h>
#include <SD.h>
#include <SPI.h>
#include <ArduinoJson.h>
#include <USB.h>
#include <USBHIDKeyboard.h>
#include "esp32-hal-tinyusb.h"   // tud_mounted() for USB.begin() poll (v4.4)
#include "esp_timer.h"           // esp_timer_get_time() for live CPU busy% (v4.10)
#include <vector>
#include <map>
#include <algorithm>
#include <Preferences.h>

#include "Config.h"
#include "GlobalState.h"
#include "LEDManager.h"
#include "FSManager.h"
#include "LogManager.h"
#include "USBManager.h"
#include "WiFiManager.h"
#include "DuckyInterpreter.h"
#include "WebServerManager.h"
#include "BTManager.h"
#include "AttackMode.h"
#include "MSCManager.h"
#include "UpdateManager.h"
#include "MSCManager.h"     // mscBegin() for BEHAVE_BROKEN mode
#include "ComShell.h"       // v4.5: "Allow COM connections" — USB CDC shell for PuTTY over COM port

// ---- Smart CPU throttling (thermal / power) --------------------------------
// The chip runs at 240 MHz by default and gets noticeably warm because the
// dashboard's constant /api/stats polling keeps the WiFi radio + web server
// busy. We can drop the core to 80 MHz whenever no one's actively driving
// the device and bump it back on demand — same responsiveness, ~40% less
// heat and ~30% less current draw.
//
// Boost triggers:  script starts running, HID/MSC activity, live-type,
//                   firmware update apply, boot script boot-up window.
// Idle drop:       nothing above true, no HTTP client for 5 s.
static uint32_t lastActivityMs = 0;
static uint32_t currentCpuMhz  = 240;
static bool     cpuMgmtEnabled = true;

void cpuNoteActivity() {
  lastActivityMs = millis();
  if (currentCpuMhz != 240 && cpuMgmtEnabled) {
    setCpuFrequencyMhz(240);
    currentCpuMhz = 240;
  }
}

// ---- Live CPU-busy tracker (v4.10) -----------------------------------------
// Every loop() iteration we measure the wall-clock time spent inside the
// loop's real work vs the yield delay(1) at the bottom. Sum of "work" us in
// a 1-second window / 10000 = busy%. This is what the dashboard's Stats grid
// polls for the "CPU %" live number.
static volatile uint64_t g_busyUs = 0;
static uint32_t g_cpuBusyPct = 0;
static unsigned long g_busyWindowStartMs = 0;
uint32_t cpuBusyPercent() { return g_cpuBusyPct; }

// v4.23 (bug-hunt CRITICAL #1): decouple the USB LED-report task from the
// `variables` map. The event handler just parks the byte here; the main
// loop's hostLedTick() copies it into `variables` on the interpreter's own
// thread. Byte-sized reads/writes are atomic on ESP32-S3 so no lock needed.
volatile uint8_t g_hostLedByte  = 0;
volatile bool    g_hostLedDirty = false;
static uint32_t  g_hostLedRequestCount = 0;
// v4.23: non-static so DuckyInterpreter.cpp can call it via extern in the
// DELAY / WAIT_FOR_* cooperative loops.
void hostLedTick() {
  if (!g_hostLedDirty) return;
  // v4.25 bug-hunt HIGH #5: clear dirty FIRST, then sample the byte. If the
  // USB events task fires between the two writes, dirty becomes true again
  // and next tick reprocesses (idempotent). The old order (read then clear)
  // dropped fast A->B->A lock-key transitions - WAIT_FOR_CAPS_CHANGE hung.
  g_hostLedDirty = false;
  uint8_t leds = g_hostLedByte;
  variables["_NUMLOCK_ON"]    = (leds & 0x01) ? "TRUE" : "FALSE";
  variables["_CAPSLOCK_ON"]   = (leds & 0x02) ? "TRUE" : "FALSE";
  variables["_SCROLLLOCK_ON"] = (leds & 0x04) ? "TRUE" : "FALSE";
  variables["_RECEIVED_HOST_LOCK_LED_REPLY"] = "TRUE";
  variables["_HOST_CONFIGURATION_REQUEST_COUNT"] = String(++g_hostLedRequestCount);
}

// ---- v4.24 shared button-event bus ----------------------------------------
// Prior versions polled GPIO0 in two disjoint places:
//   1) loop()   - short-press = stopRequested, 10s hold = factory reset
//   2) DuckyInterpreter's WAIT_FOR_BUTTON_PRESS - spun on digitalRead()
// (2) starved (1) so a factory-reset hold during a WAIT would never fire and
// a WAIT couldn't ride the same debounced state machine. pumpButton() is now
// the single owner; loop() and WAIT_FOR_BUTTON_PRESS both call it. It:
//   * publishes press-release edges into g_buttonShortPressed (drained by
//     WAIT_FOR_BUTTON_PRESS)
//   * fires the BUTTON_DEF-bound script on short-press when one is bound
//   * still triggers the 10s factory-reset even during a WAIT
//   * only sets stopRequested when the caller wants that (loop does, WAIT
//     doesn't - g_buttonSuppressStop)
volatile bool g_buttonShortPressed  = false;   // release-edge one-shot (consumer clears)
bool          g_buttonSuppressStop  = false;   // WAIT_FOR_BUTTON_PRESS sets this
String        g_buttonHandlerScript = "";      // BUTTON_DEF ... END_BUTTON body
bool          g_buttonHandlerRunning = false;  // reentrancy guard

extern void executeScript(const String& script);

// v4.26 bug-hunt HIGH #2: press that triggered a stopRequested must NOT
// double-fire as a BUTTON_DEF handler when its release-edge arrives after
// the script cleans up. Track it per-press.
static bool          g_pressWasStop     = false;

void pumpButton() {
  static unsigned long lastButtonPress = 0;
  static unsigned long buttonDownAt    = 0;
  static bool          buttonArmed     = false;
  static int           lastRaw         = HIGH;

  int st = digitalRead(RESET_BUTTON_PIN);
  if (st == LOW) {
    if (buttonDownAt == 0) buttonDownAt = millis();
    unsigned long held = millis() - buttonDownAt;

    // v4.26 bug-hunt MEDIUM #8: only fire the stop ONCE per press-edge, not
    // every 500 ms for the whole hold (otherwise `stopRequested=true` +
    // `setLEDMode(4)` + a log line repeat ~20x during a 10-s factory-reset
    // hold). Gate on both the debounce AND `!stopRequested`/`!g_pressWasStop`.
    if (!g_buttonSuppressStop && !g_pressWasStop &&
        millis() - lastButtonPress > 500) {
      lastButtonPress = millis();
      if (scriptRunning) {
        stopRequested = true;
        g_pressWasStop = true;      // remember: don't fire handler on this press-release
        Serial.println("Stop requested via reset button");
        setLEDMode(4);
        logCommand("SCRIPT_STOPPED", "User requested stop via reset button");
      }
    }

    if (held >= 3000 && !buttonArmed) {
      buttonArmed = true;
      Serial.println("[BUTTON] Factory-reset ARMED - keep holding to 10 s to trigger");
      setLEDMode(7);
    }
    if (held >= 10000) {
      Serial.println("[BUTTON] 10s HOLD detected -> factory reset");
      pixels.setPixelColor(0, pixels.Color(0, 0, 255)); pixels.show();
      auto wipeDir = [](const char* dir) {
        if (!sdCardPresent) return;
        File root = SD.open(dir);
        if (!root) return;
        std::vector<String> victims;
        File f = root.openNextFile();
        while (f) {
          if (!f.isDirectory()) {
            String leaf = String(f.name());
            int slash = leaf.lastIndexOf('/');
            if (slash >= 0) leaf = leaf.substring(slash + 1);
            victims.push_back(String(dir) + "/" + leaf);
          }
          f = root.openNextFile();
        }
        root.close();
        for (auto& p : victims) SD.remove(p);
      };
      wipeDir(DIR_SCRIPTS);
      wipeDir(DIR_UPLOADS);
      wipeDir(DIR_LOGS);
      const char* transient[] = {
        "/reboot_script.txt", "/temp_resume.txt", "/wifi_creds.txt",
        "/temp_creds.txt", "/update.espkg", "/.sdtest", "/history.txt"
      };
      for (const char* p : transient) if (SD.exists(p)) SD.remove(p);
      preferences.clear();
      delay(1500);
      ESP.restart();
    }
  } else {
    // LOW -> HIGH release edge: publish for WAIT_FOR_BUTTON_PRESS consumers.
    if (lastRaw == LOW && buttonDownAt != 0) {
      unsigned long held = millis() - buttonDownAt;
      if (held >= 40 && held < 3000) {
        // Genuine short press-release. Publish to the wait-bus and, if a
        // BUTTON_DEF is bound AND we're not currently mid-script AND this
        // wasn't the very press that stopped a script (v4.26 HIGH #2),
        // fire the handler as its own script.
        g_buttonShortPressed = true;
        if (!scriptRunning && !g_pressWasStop && !g_buttonHandlerRunning &&
            g_buttonHandlerScript.length()) {
          g_buttonHandlerRunning = true;
          String body = g_buttonHandlerScript;   // copy: script may rebind
          executeScript(body);
          g_buttonHandlerRunning = false;
        }
      }
    }
    if (buttonDownAt != 0 && buttonArmed) {
      Serial.println("[BUTTON] Released before 10 s - factory reset CANCELLED");
      setLED(0, 0, 255);
    }
    buttonDownAt = 0;
    buttonArmed  = false;
    g_pressWasStop = false;   // clear per-press latch on release
  }
  lastRaw = st;
}

// ---- Thermal management (v4.11) --------------------------------------------
// The ESP32-S3 has an on-die temperature sensor exposed via temperatureRead()
// in the Arduino core. On a USB-key form factor the chip sits ~2 mm from the
// enclosure so felt heat is real. We sample every 2 s and step through four
// thermal states with graceful degradation:
//
//   State 0 NORMAL  (<65 C) : cpuMgmtTick() governs CPU freq as usual.
//   State 1 WARM    (65-74) : force 160 MHz cap.
//   State 2 HOT     (75-84) : force 80 MHz cap; auto-disable BLE if on.
//   State 3 CRITICAL(>=85)  : shutdown mode - tud_disconnect, WiFi off, web
//                             server stops answering, loop() just delays and
//                             waits for the temp to drop below the HOT
//                             threshold before letting the user reboot to
//                             recover.
//
// Hysteresis: we drop back one state only when temp falls 4 C below the
// entering threshold, so the metric doesn't oscillate around a boundary.
float    g_cpuTempC     = 0.0f;
uint8_t  g_thermalState = 0;     // 0=normal, 1=warm, 2=hot, 3=critical
static unsigned long g_lastTempSampleMs = 0;
static bool g_thermalShutdown = false;   // latched at CRITICAL until reboot

float    cpuTemperatureC() { return g_cpuTempC; }
uint8_t  thermalState()    { return g_thermalState; }
bool     isThermalShutdown() { return g_thermalShutdown; }

static void thermalApply(uint8_t newState) {
  if (newState == g_thermalState) return;
  g_thermalState = newState;
  Serial.printf("[THERMAL] state -> %u (%.1f C)\n", (unsigned)newState, g_cpuTempC);
  switch (newState) {
    case 0:  // NORMAL
      cpuMgmtEnabled = true;
      break;
    case 1:  // WARM - cap at 160 MHz
      cpuMgmtEnabled = false;
      if (currentCpuMhz != 160) { setCpuFrequencyMhz(160); currentCpuMhz = 160; }
      break;
    case 2:  // HOT - cap at 80 MHz + kill BLE if running
      cpuMgmtEnabled = false;
      if (currentCpuMhz != 80) { setCpuFrequencyMhz(80); currentCpuMhz = 80; }
      if (bluetoothToggleEnabled) {
        Serial.println("[THERMAL] Disabling BLE to shed heat");
        stopBT();
        bluetoothToggleEnabled = false;
      }
      break;
    case 3:  // CRITICAL - shutdown mode (latched)
      g_thermalShutdown = true;
      Serial.println("[THERMAL] *** CRITICAL - entering shutdown mode ***");
      // v4.16 FIX: signal media-not-ready BEFORE tud_disconnect so an
      // in-flight WRITE10 to a FAT metadata sector is stalled cleanly
      // rather than truncated mid-transfer (would need chkdsk otherwise).
      sdCardPresent = false;
      delay(20);   // let tud_msc_test_unit_ready_cb see the change once
      // Kill USB entirely so the host stops driving MSC/HID transfers.
      tud_disconnect();
      // Bring WiFi radio down completely - AP + STA both off.
      WiFi.disconnect(true);
      WiFi.mode(WIFI_OFF);
      // Slowest safe clock.
      if (currentCpuMhz != 80) { setCpuFrequencyMhz(80); currentCpuMhz = 80; }
      break;
  }
}

static void thermalTick() {
  // v4.16: sample every 500 ms (was 2000). The temperature sensor is cheap
  // to read, and the user wants the decimal-place digits to change in
  // near-real-time on the dashboard. Combined with the /api/temp 1 Hz poll
  // on the frontend, the tile now updates once per second minimum.
  if (millis() - g_lastTempSampleMs < 500) return;
  g_lastTempSampleMs = millis();
  g_cpuTempC = temperatureRead();
  // ESP32 core returns 53.33 as a sentinel when the sensor isn't ready; ignore
  // clearly-out-of-range readings.
  if (g_cpuTempC < -20.0f || g_cpuTempC > 150.0f) return;

  // Hysteresis: entering threshold to step UP, exit threshold to step DOWN.
  const float T_WARM_UP = 65.0f, T_WARM_DN = 61.0f;
  const float T_HOT_UP  = 75.0f, T_HOT_DN  = 71.0f;
  const float T_CRIT_UP = 85.0f, T_CRIT_DN = 81.0f;

  uint8_t desired = g_thermalState;
  switch (g_thermalState) {
    case 0: if (g_cpuTempC >= T_WARM_UP) desired = 1; break;
    case 1: if (g_cpuTempC >= T_HOT_UP)  desired = 2;
            else if (g_cpuTempC < T_WARM_DN) desired = 0; break;
    case 2: if (g_cpuTempC >= T_CRIT_UP) desired = 3;
            else if (g_cpuTempC < T_HOT_DN)  desired = 1; break;
    case 3: /* latched - only reboot recovers */                    break;
  }
  thermalApply(desired);
}

static void cpuMgmtTick() {
  if (!cpuMgmtEnabled) return;
  // Never idle down while a script is running / update is applying / delay
  // is timing out — those need consistent tick rates.
  if (scriptRunning || updateApplying || currentDelayTotal > 0) {
    if (currentCpuMhz != 240) { setCpuFrequencyMhz(240); currentCpuMhz = 240; }
    lastActivityMs = millis();
    return;
  }
  // Keep boosted while any WiFi client is connected — the browser polls
  // /api/stats every 3 s, so demoting mid-request stutters the UI.
  if (WiFi.softAPgetStationNum() > 0) {
    if (currentCpuMhz != 240) { setCpuFrequencyMhz(240); currentCpuMhz = 240; }
    lastActivityMs = millis();
    return;
  }
  if (millis() - lastActivityMs > 5000 && currentCpuMhz != 80) {
    setCpuFrequencyMhz(80);
    currentCpuMhz = 80;
    Serial.println("[CPU] Idling to 80 MHz to reduce heat");
  }
}

void setup() {
  // STEP 0 — TRUE STEALTH: before anything else, if the last boot had Silent
  // Startup on, immediately drop the USB FSLS PHY pads so Windows never
  // completes enumeration of the ROM USB-Serial/JTAG. Any code that runs
  // before this (Serial.begin, delay(1000), preferences init) gives the host
  // enough time to attach the generic "USB JTAG/serial debug unit" — moving
  // this up is what makes silent actually silent.
  {
    Preferences bootPrefs;
    bootPrefs.begin("badusb", true);   // read-only
    bool silent = bootPrefs.getBool("silent_boot", true);
    bootPrefs.end();
    if (silent) {
      usbBeginSilent();      // disables USB PHY pads before any enumeration
      silentArmForNextBoot();// persist so the C++ constructor kills USB earlier next boot
    } else {
      silentClearForNextBoot();
    }
  }

  Serial.begin(115200);
  // v4.4: the old 1000 ms hard delay was a copy-paste from AVR days — Serial
  // is available instantly on ESP32-S3 UART. Shave it off the boot path.

  pixels.begin();
  pixels.setBrightness(50);
  setLED(0, 0, 255);   // v4.14: blue-only board — use B channel

  preferences.begin("badusb", false);

  // ---- Firmware version stamp (v4.8) -------------------------------------
  // NVS survives a normal `esptool write_flash` of the app slot, so any
  // "dangerous" flag set in a previous session (behave_broken, com_on) would
  // silently re-arm itself after a fresh flash. That's how a user ended up
  // stuck in SD_READER mode on what they thought was a clean install.
  //
  // Fix: stamp the currently-running firmware version into NVS. If the stored
  // stamp doesn't match, we're on a fresh flash or an upgrade — clear the
  // flags that could brick the user's normal boot flow. Everything else
  // (WiFi creds, AP password, ATTACKMODE config, silent-startup, USB VID/PID)
  // is preserved because those aren't "dangerous" the same way.
  //
  // To force a reset on any future release, bump FIRMWARE_STAMP in Config.h.
  {
    const uint32_t FIRMWARE_STAMP = 436;   // v4.36 - CRITICAL: /temp_resume.txt & /reboot_script.txt were read via loadScript() which prepends /scripts/, so every ATTACKMODE reboot silently discarded the resume file. Now read from SD root directly. Also: evalCondition strips wrapping parens (`IF ($_OS == LINUX)`), case-insensitive TRUE/FALSE (Hak5 built-ins use uppercase), string equality path uses toUpperCase compare, `==`/`!=` fall back to string when either side isn't numeric. ATTACKMODE cfg seeded from currentAttackMode (tokens only override fields they mention). ensureHidReady+3s wait on resume path so first-plug driver-bind window doesn't drop early keystrokes.
    uint32_t storedStamp = preferences.getUInt("fw_stamp", 0);
    if (storedStamp != FIRMWARE_STAMP) {
      Serial.printf("[BOOT] Firmware stamp changed (%u -> %u). Clearing "
                    "dangerous flags (behave_broken, com_on, fake USB ID).\n",
                    (unsigned)storedStamp, (unsigned)FIRMWARE_STAMP);
      preferences.putBool  ("behave_broken",   false);
      preferences.putBool  ("com_on",          false);
      // Restore the default USB identity so a fake "SD_READER"/"Generic"
      // string doesn't survive into normal mode.
      preferences.remove   ("usb_prod");
      preferences.remove   ("usb_mfr");
      // Restore default ATTACKMODE (HID on, MSC off) if it was left in the
      // "SD_READER" state (hid=false, msc=true) by a prior session.
      if (!preferences.getBool("am_hid", true) &&
           preferences.getBool("am_msc", false) &&
           preferences.getBool("am_no_hid_intent", false)) {
        preferences.putBool("am_hid",           true);
        preferences.putBool("am_msc",           false);
        preferences.putBool("am_no_hid_intent", false);
      }
      // v4.8: also clear any stale MSC sub-region so the full SD is exposed
      // by default on a fresh flash.
      preferences.remove("msc_base");
      preferences.remove("msc_sect");
      preferences.putUInt("fw_stamp", FIRMWARE_STAMP);
    }
  }
  // ------------------------------------------------------------------------

  // ---- BEHAVE_BROKEN (v4.6 rewrite) --------------------------------------
  // Recovery is a DOUBLE-press of GPIO0 within 3 s of boot (was: 5s hold).
  // We also DON'T touch any files on the SD anymore - the previous version's
  // f_chmod(HIDDEN|SYS) made Explorer treat them as "gone" which was
  // confusing. Real partition-split "only expose sub-region via MSC while
  // hiding the main partition" is a much bigger project (parse+rewrite MBR,
  // format fresh FAT in free space, MSC LBA translation) - flagged as a
  // follow-up firmware, not v4.6.
  //
  // The GPIO0 pin (RESET_BUTTON_PIN in Config.h) is correct for the ESP32-S3
  // Dongle/Key hardware: GPIO0 is the BOOT button with internal pull-up.
  {
    bool behaveBroken = preferences.getBool("behave_broken", false);
    if (behaveBroken) {
      pinMode(RESET_BUTTON_PIN, INPUT_PULLUP);
      Serial.println("[BOOT] behave_broken set. Double-press GPIO0 within 3 s to recover...");
      unsigned long start = millis();
      int presses = 0;
      int lastState = HIGH;
      unsigned long lastEdge = 0;
      const unsigned long DEBOUNCE_MS = 40;
      while (millis() - start < 3000) {
        int state = digitalRead(RESET_BUTTON_PIN);
        if (state != lastState && (millis() - lastEdge) > DEBOUNCE_MS) {
          lastEdge = millis();
          if (lastState == LOW && state == HIGH) { presses++; if (presses >= 2) break; }
          lastState = state;
        }
        pixels.setPixelColor(0, ((millis() / 200) & 1) ? pixels.Color(255,127,0) : 0);
        pixels.show();
        delay(5);
      }
      if (presses >= 2) {
        Serial.println("[BOOT] Double-press detected! Clearing behave_broken flag.");
        preferences.putBool("behave_broken", false);
        preferences.remove("usb_prod");
        preferences.remove("usb_mfr");
        preferences.putBool("am_hid", true);
        preferences.putBool("am_msc", false);
        preferences.putBool("am_no_hid_intent", false);
        // v4.8: also clear the MSC sub-region so the full SD is exposed again
        // on the normal boot after recovery.
        preferences.remove("msc_base");
        preferences.remove("msc_sect");
        preferences.end();
        pixels.setPixelColor(0, pixels.Color(0, 255, 0)); pixels.show();
        delay(500);
        ESP.restart();
      } else {
        Serial.println("[BOOT] Entering SD_READER-only mode.");
        USB.VID(0x0781);       // SanDisk-ish generic
        USB.PID(0x5567);
        USB.manufacturerName("Generic");
        USB.productName("SD_READER");
        // v4.6 fix: FORCE storage=true so mscBegin() actually registers the
        // MSC interface. In v4.5 mscBegin() was called with storage=false
        // (default) and silently early-returned - hence "didn't show up".
        loadAttackModePrefs();
        currentAttackMode.storage = true;
        currentAttackMode.hid     = false;
        if (!initSDCard()) sdCardPresent = false;
        // v4.6: NO hideWebFilesOnSD() - files stay 100% intact and visible
        // when the SD is later read in a real card reader.
        // v4.9: load the MSC sub-region window (LBA + size persisted by
        // performBehaveBroken) BEFORE mscBegin so the SD_READER exposes only
        // the free-space slice, hiding the real files from the host.
        mscLoadSubRegion();
        mscBegin();
        silentRestorePadsForUsb();
        USB.begin();
        // Live loop: watch GPIO0 for a runtime double-press too so recovery
        // works even if the boot-time window was missed.
        // v4.11 FIX: initialize firstAt = millis() (was 0), and gate the
        // 2s-reset with liveCount > 0. In v4.10 the very first press edge
        // caused `if (millis() - 0 > 2000)` to reset liveCount to 1 unconditionally,
        // so a SINGLE press already got treated as two -> device fell out of
        // SD_READER on an accidental tap. Also guard against unsigned wrap.
        int st = HIGH; unsigned long edgeAt = 0; int liveCount = 0; unsigned long firstAt = millis();
        while (true) {
          int s = digitalRead(RESET_BUTTON_PIN);
          if (s != st && (millis() - edgeAt) > DEBOUNCE_MS) {
            edgeAt = millis();
            if (st == LOW && s == HIGH) {
              if (liveCount == 0) firstAt = millis();
              liveCount++;
              if (liveCount >= 2 && (millis() - firstAt) < 2000) {
                Serial.println("[BEHAVE_BROKEN] Live double-press - clearing and rebooting");
                preferences.begin("badusb", false);
                preferences.putBool("behave_broken", false);
                // v4.8: clear sub-region + fake identity so we come back
                // as a normal device.
                preferences.remove("msc_base");
                preferences.remove("msc_sect");
                preferences.remove("usb_prod");
                preferences.remove("usb_mfr");
                preferences.putBool("am_hid",           true);
                preferences.putBool("am_msc",           false);
                preferences.putBool("am_no_hid_intent", false);
                preferences.end();
                ESP.restart();
              }
              // v4.11: only reset when we've actually seen at least one
              // valid press cycle. Prevents the "first release counts as two"
              // bug where firstAt started at 0 and millis()-0 was always > 2000.
              if (liveCount > 0 && (millis() - firstAt) > 2000) {
                liveCount = 1; firstAt = millis();
              }
            }
            st = s;
          }
          delay(20);
        }
      }
    }
  }
  // ------------------------------------------------------------------------

  ap_ssid = preferences.getString("ap_ssid", DEFAULT_AP_SSID);
  ap_password = preferences.getString("ap_password", DEFAULT_AP_PASSWORD);
  currentLanguage = preferences.getString("language", "us");
  wifiScanTime = preferences.getInt("wifi_scan_time", WIFI_SCAN_TIMEOUT);
  ledEnabled = preferences.getBool("led_enabled", true);
  loggingEnabled = preferences.getBool("logging_enabled", false);
  autoConnectEnabled = preferences.getBool("autoconnect", false);
  saveOnConnectEnabled = preferences.getBool("save_creds", false);
  btDiscoveryEnabled = preferences.getBool("bt_discovery", false);
  silentStartup = preferences.getBool("silent_boot", true);
  loadAttackModePrefs();     // ATTACKMODE + SIZE persisted from last boot


  pinMode(RESET_BUTTON_PIN, INPUT_PULLUP);

  if (!initSDCard()) {
    Serial.println("SD Card initialization failed!");
    setLEDMode(2);
    sdCardPresent = false;
    while (1) {
      handleLED();
      delay(100);
    }
  }

  // v4.8: clear stale HIDDEN/SYS bits on the website files (leftover from
  // v4.4-v4.7 hideWebFilesOnSD) so Explorer stops treating them as gone.
  unhideWebFilesOnSD();
  // v4.17: ensure /extensions/ exists so the Extensions tab always finds a
  // valid directory even on a fresh SD card.
  // v4.22: also create the /extensions/hak5/ and /extensions/custom/ folders.
  //   hak5/   -> .txt files from the Hak5 usbrubberducky-payloads repo,
  //              executed with Hak5-strict semantics (bare EXTENSION frame
  //              triggers the strict ATTACKMODE STORAGE = storage-only path)
  //   custom/ -> your own .ext files, executed with our default semantics
  //              (ATTACKMODE STORAGE keeps HID for UX safety)
  if (sdCardPresent) {
    if (!SD.exists("/extensions"))        SD.mkdir("/extensions");
    if (!SD.exists("/extensions/hak5"))   SD.mkdir("/extensions/hak5");
    if (!SD.exists("/extensions/custom")) SD.mkdir("/extensions/custom");
  }

  // v4.8: load the MSC sub-region window (LBA base + sector count) from NVS.
  // When set, MSC only exposes those sectors of the real SD to the host.
  mscLoadSubRegion();

  logDebug("=== BOOT START ===");
  logDebug("AP SSID: " + ap_ssid);
  logDebug("Language pref: " + currentLanguage);

  loadAvailableLanguages();
  logDebug("Languages loaded: " + String(availableLanguages.size()));
  for (auto& l : availableLanguages) logDebug("  lang: " + l);

  loadAvailableScripts();
  logDebug("Scripts loaded: " + String(availableScripts.size()));

  if (!loadLanguage(currentLanguage)) {
    Serial.println("Failed to load default language, trying 'us'");
    logDebug("Failed to load language: " + currentLanguage + ", trying 'us'");
    if (!loadLanguage("us")) {
      Serial.println("Failed to load 'us' language");
      logDebug("CRITICAL: Failed to load 'us' language!");
      setLEDMode(2);
    }
  }
  logDebug("Active language: " + currentLanguage);
  logDebug("Keymap entries: " + String(currentKeymap.size()));

  String bootPref = preferences.getString("boot_script", "");
  currentBootScriptFiles.clear();
  bootScript = "";
  
  if (bootPref.length() > 0) {
    int start = 0;
    int end = bootPref.indexOf(',');
    while (end != -1) {
      String file = bootPref.substring(start, end);
      if (SD.exists(String(DIR_SCRIPTS) + "/" + file)) {
        currentBootScriptFiles.push_back(file);
        bootScript += loadScript(file) + "\n";
      }
      start = end + 1;
      end = bootPref.indexOf(',', start);
    }
    String lastFile = bootPref.substring(start);
    if (SD.exists(String(DIR_SCRIPTS) + "/" + lastFile)) {
      currentBootScriptFiles.push_back(lastFile);
      bootScript += loadScript(lastFile) + "\n";
    }

    if (currentBootScriptFiles.size() > 0) {
      bootModeEnabled = true;
      Serial.println("Boot scripts loaded: " + bootPref);
    }
  } else if (SD.exists(String(DIR_SCRIPTS) + "/boot.txt")) {
    bootScript = loadScript("boot.txt");
    bootModeEnabled = true;
    currentBootScriptFiles.push_back("boot.txt");
    Serial.println("Default boot.txt found");
  }

  currentUSBConfig.vid = preferences.getString("usb_vid", "0x303a");
  currentUSBConfig.pid = preferences.getString("usb_pid", "0x0002");
  currentUSBConfig.rndVid = preferences.getBool("usb_rndVid", false);
  currentUSBConfig.rndPid = preferences.getBool("usb_rndPid", false);
  currentUSBConfig.mfr = preferences.getString("usb_mfr", "Espressif");
  currentUSBConfig.prod = preferences.getString("usb_prod", "ESP32-S3");

  if (currentUSBConfig.rndVid) {
    char buf[7]; sprintf(buf, "0x%04x", (uint16_t)(esp_random() & 0xFFFF));
    currentUSBConfig.vid = String(buf);
  }
  if (currentUSBConfig.rndPid) {
    char buf[7]; sprintf(buf, "0x%04x", (uint16_t)(esp_random() & 0xFFFF));
    currentUSBConfig.pid = String(buf);
  }

  {
    uint16_t vidToUse = (uint16_t)strtol(currentUSBConfig.vid.c_str(), NULL, 16);
    uint16_t pidToUse = (uint16_t)strtol(currentUSBConfig.pid.c_str(), NULL, 16);
    // ATTACKMODE overrides USB Identity settings when it set custom VID/PID.
    if (currentAttackMode.vid != 0x303A || currentAttackMode.pid != 0x0002) {
      vidToUse = currentAttackMode.vid;
      pidToUse = currentAttackMode.pid;
    }
    USB.VID(vidToUse);
    USB.PID(pidToUse);
  }
  USB.manufacturerName(currentUSBConfig.mfr.c_str());
  USB.productName(currentUSBConfig.prod.c_str());

  // v4.5: "Allow COM connections" (USB CDC serial for PuTTY) takes precedence
  // over HID + MSC — when enabled the composite is CDC-only. Toggling
  // requires a reboot to rebuild descriptors, same pattern as ATTACKMODE.
  bool comOn = preferences.getBool("com_on", false);
  if (comOn) {
    // Force HID + MSC off for this boot so the composite is clean CDC.
    currentAttackMode.hid = false;
    currentAttackMode.storage = false;
    Serial.println("[COM] Allow COM connections is ON — CDC-only mode this boot");
    // Also override the product string so the OS sees a friendly name.
    USB.productName("ESP32-S3 COM Shell");
    comShellBegin();   // register the CDC interface BEFORE USB.begin()
  } else {
    // Always call keyboard.begin(): its only job is creating the TX semaphore/
    // mutex — USBHID::SendReport requires them or every hid report is dropped
    // silently. The HID interface descriptor was already registered by the
    // USBHIDKeyboard global-object constructor at C++ static-init time.
    keyboard.begin();

    // v4.18: hook the LED-output event from USBHIDKeyboard so we can update
    // the Hak5 special vars ($_CAPSLOCK_ON, $_NUMLOCK_ON, $_SCROLLLOCK_ON,
    // $_RECEIVED_HOST_LOCK_LED_REPLY, $_HOST_CONFIGURATION_REQUEST_COUNT)
    // authoritatively based on what the host actually thinks. The event is
    // posted by USBHIDKeyboard::_onOutput when the host sends a keyboard
    // LED report; buffer[0] carries: bit0=NumLock, bit1=CapsLock, bit2=Scroll.
    // v4.23 (bug-hunt CRITICAL #1 fix): the event handler runs on the
    // arduino_usb_events task, NOT the main Arduino task. Mutating the
    // std::map `variables` from a different task while the Ducky interpreter
    // is reading/writing it is UB - the red-black tree can rebalance mid-
    // iteration and crash. Instead, park the LED byte in a plain volatile
    // slot; the main loop pumps it into `variables` on the interpreter's
    // own thread (see hostLedTick()).
    keyboard.onEvent(ARDUINO_USB_HID_KEYBOARD_LED_EVENT,
      [](void*, esp_event_base_t, int32_t, void* event_data) {
        arduino_usb_hid_keyboard_event_data_t* d = (arduino_usb_hid_keyboard_event_data_t*)event_data;
        if (!d) return;
        // Byte-sized writes are atomic on ESP32-S3 - no lock needed.
        g_hostLedByte  = d->leds;
        g_hostLedDirty = true;
      });
    // Register the USB Mass Storage interface BEFORE USB.begin() so the
    // composite descriptor includes MSC when STORAGE is enabled.
    mscBegin();
  }
  // In silent mode, usbBeginSilent() already ran at STEP 0 (very top of setup)
  // — do NOT call USB.begin() here or the PHY pads pop back on.
  //
  // Decision matrix for whether to bring USB up at boot:
  //   * ATTACKMODE BLANK (no HID, no STORAGE) → always deferred. There's
  //     nothing to enumerate; keep the pads dead so the host sees nothing.
  //     Recovery is via "ATTACKMODE HID" over WiFi.
  //   * non-silent boot → bring up now (normal behaviour).
  //   * silent + STORAGE → bring up now so the drive is mounted persistently.
  //   * silent + HID only → defer; HID attaches on first script/live-type.
  bool isBlank = (!currentAttackMode.hid) && (!currentAttackMode.storage);
  // v4.5: COM mode always brings USB up (CDC IS the USB device) and never
  // counts as "blank" even though HID+MSC are off.
  bool bringUpUsbNow = comOn || (!isBlank && ((!silentStartup) || currentAttackMode.storage));

  // v4.6: removed the hideWebFilesOnSD() call - it set FAT HIDDEN+SYS bits
  // and made the files appear "gone" in the host's Explorer, which is not
  // what the user wanted (they want files preserved intact and only invisible
  // via ESP's MSC exposure, which requires a partition-split approach that
  // is a separate follow-up).
  if (bringUpUsbNow) {
    silentRestorePadsForUsb();
    USB.begin();
    usbStarted = true;
    hidConnected = true;
    // v4.4: was `delay(1000)` — a hard sleep whether the host mounts fast or
    // slow. Poll tud_mounted() with a 1 s ceiling; most hosts (Windows/Linux)
    // mount within 100-300 ms, saving ~700 ms of boot time on average.
    unsigned long t0 = millis();
    while (!tud_mounted() && (millis() - t0) < 1000) delay(5);
    if (silentStartup && currentAttackMode.storage) {
      Serial.println("Silent+STORAGE: USB up so drive mounts; HID stays quiet until used");
    }
  } else if (isBlank) {
    Serial.println("ATTACKMODE BLANK: USB stays fully unmounted (recover via web UI: ATTACKMODE HID)");
  } else {
    Serial.println("Silent startup: USB deferred; HID attaches on demand");
  }

  setupAP();
  bluetoothName = preferences.getString("bt_name", "ESP32-S3");
  setupBT();
  setupWebServer();
  // COM shell is now started BEFORE USB.begin() further up (search for
  // "Allow COM connections"). Nothing to do here.

  setLED(0, 0, 255);   // v4.14: blue-only board — use B channel

  // v4.36 CRITICAL fix: read the resume files DIRECTLY from the SD root.
  // Prior code used loadScript(name) which internally prepends "/scripts/",
  // so the reads went to /scripts//temp_resume.txt and /scripts//reboot_
  // script.txt (both nonexistent) and returned "". Symptom: every ATTACKMODE
  // reboot silently discarded the persisted script - the ESP came back with
  // the new USB identity, deleted the resume file without reading it, and
  // never called executeScript. This is the "USB attaches/detaches then
  // nothing types" bug.
  //
  // Also: only remove the file AFTER a successful non-empty read, so a
  // transient read failure doesn't nuke the payload permanently.
  auto readSDRoot = [](const char* path) -> String {
    if (!SD.exists(path)) return "";
    File f = SD.open(path);
    if (!f) return "";
    String s = f.readString();
    f.close();
    return s;
  };
  {
    String content = readSDRoot("/reboot_script.txt");
    if (content.length() > 0) {
      Serial.printf("Reboot script found (%u B) - executing once\n", (unsigned)content.length());
      SD.remove("/reboot_script.txt");
      // v4.36 HIGH #4/#5: when a resume file is present, force USB up NOW
      // and wait long enough for a fresh HID identity's driver-bind window
      // (Windows first-plug of an unfamiliar VID/PID can take 3-5s).
      ensureHidReady();
      delay(3000);
      executeScript(content);
    } else if (SD.exists("/reboot_script.txt")) {
      // File exists but read returned empty - keep it for a next-boot retry
      // instead of silently deleting.
      Serial.println("[RESUME] /reboot_script.txt read returned empty - keeping for retry");
    }
  }
  {
    String content = readSDRoot("/temp_resume.txt");
    if (content.length() > 0) {
      Serial.printf("Resume script found (%u B) after USB identity change - executing\n", (unsigned)content.length());
      SD.remove("/temp_resume.txt");
      ensureHidReady();
      delay(3000);
      executeScript(content);
    } else if (SD.exists("/temp_resume.txt")) {
      Serial.println("[RESUME] /temp_resume.txt read returned empty - keeping for retry");
    }
  }

  Serial.println("ESP32-S3 BadUSB Ready!");
  Serial.print("Connect to WiFi: ");
  Serial.println(ap_ssid);
  Serial.print("Password: ");
  Serial.println(ap_password);
  Serial.println("Open browser and go to: 192.168.4.1");

  loadCommandHistory();
  // v4.4: the duplicate "check for reboot payload" block that used to live
  // here was dead code — the block near the top of setup() already deletes
  // /reboot_script.txt on find, so this second check never fired. Removed.
}

void loop() {
  // v4.10: busy timer for live CPU %. Wall-clock spent between here and the
  // delay(1) below is counted as "busy"; the yield time is idle.
  uint64_t __loop_t0 = esp_timer_get_time();

  // v4.11: thermal governor. Samples the die temp every 2 s and either
  // throttles CPU / disables BLE / cuts USB + WiFi entirely if things run
  // away. Do this FIRST so a hot loop can't dodge the check.
  thermalTick();
  if (g_thermalShutdown) {
    // Latched shutdown - nothing but the temperature loop runs. LED blinks
    // red slow so the user can see the state.
    static unsigned long __lastBlink = 0;
    if (millis() - __lastBlink > 500) {
      __lastBlink = millis();
      static bool on = false; on = !on;
      pixels.setPixelColor(0, on ? pixels.Color(255, 0, 0) : 0);
      pixels.show();
    }
    delay(200);
    return;
  }

  server.handleClient();
  loopCaptivePortal();   // service captive-portal DNS
  comShellLoop();        // v4.5: COM-port shell over USB CDC (if enabled)
  hostLedTick();         // v4.23: copy LED byte from USB task -> variables safely on main task
  handleLED();
  cpuMgmtTick();         // drop to 80 MHz after 5 s idle (thermal management)

  // Apply a bundled .espkg update if one was uploaded (writes SD files, then OTAs)
  processPendingUpdate();

  // v4.4: consume the deferred /execute payload one tick after the response
  // was queued, giving LWIP time to flush the HTTP 200 to the client before
  // the script (potentially) reboots the chip.
  if (pendingScriptReady) {
    pendingScriptReady = false;
    String s = pendingScript;
    pendingScript = "";
    delay(120);            // let the TCP FIN go out
    executeScript(s);
  }

  if (millis() - lastSDCheck >= SD_CHECK_INTERVAL) {
    lastSDCheck = millis();
    checkSDCard();
  }

  // v4.24: button polling moved to pumpButton() so WAIT_FOR_BUTTON_PRESS
  // shares the same debounced state machine (10s factory-reset still fires
  // during a WAIT; BUTTON_DEF handler auto-runs on short-press when idle).
  pumpButton();

  // Status updates
  if (millis() - lastStatusUpdate >= STATUS_UPDATE_INTERVAL) {
    lastStatusUpdate = millis();
    if (scriptRunning) {
      unsigned long elapsed = (millis() - scriptStartTime) / 1000;
      Serial.println("Script running for " + String(elapsed) + " seconds");
    }
  }

  // Run the boot script ONCE per connection session, not on every loop.
  // Without this guard it re-fires continuously while any client stays
  // connected (scriptRunning clears the moment the script ends).
  static bool bootScriptHasRun = false;
  int bootStations = WiFi.softAPgetStationNum();
  if (bootStations == 0) bootScriptHasRun = false;   // rearm when everyone leaves
  if (bootModeEnabled && bootStations > 0 && !scriptRunning && !bootScriptHasRun) {
    bootScriptHasRun = true;
    Serial.println("Client connected - executing boot script");
    logCommand("BOOT_SCRIPT", "Executing boot script on client connection");
    executeScript(bootScript);
  }

  // Background processing for Rower and Automation
  processRower();
  processAutomation();
  processAutoConnect();
  processBackgroundTasks();
  
  if (bluetoothToggleEnabled) {
    loopBT();
  }
  
  // v4.16 FIX (perf-critical): the previous `btDiscoveryEnabled || millis() - lastBTScan > 60000`
  // gate fired the fallback branch whenever discovery wasn't on for at least
  // 60 s, and `scanBT()` blocks the ENTIRE loop for 5 seconds (synchronous
  // pBLEScan->start(5, false)). That froze /api/stats, DNS, HID, MSC every
  // minute. Now: only scan when discovery is actually enabled, and only
  // every 15 s. When off, the BT stack stays idle - zero unwanted freezes.
  static unsigned long lastBTScan = 0;
  if (btDiscoveryEnabled && !scriptRunning && (millis() - lastBTScan > 15000)) {
    lastBTScan = millis();
    scanBT();
  }


  // Poll async WiFi scan without blocking
  pollWiFiScan();

  // v4.10: fold this iteration's busy time into the 1s window and refresh
  // the CPU busy% metric that /api/stats exposes.
  {
    uint64_t __loop_t1 = esp_timer_get_time();
    g_busyUs += (__loop_t1 - __loop_t0);
    if (millis() - g_busyWindowStartMs >= 1000) {
      g_busyWindowStartMs = millis();
      uint32_t pct = (uint32_t)(g_busyUs / 10000ULL);
      if (pct > 100) pct = 100;
      g_cpuBusyPct = pct;
      g_busyUs = 0;
    }
  }
  delay(1);
}

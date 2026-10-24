#include <Arduino.h>
#include <WiFi.h>
#include <WebServer.h>
#include <SD.h>
#include <SPI.h>
#include <ArduinoJson.h>
#include <USB.h>
#include <USBHIDKeyboard.h>
#include "esp32-hal-tinyusb.h"   // tud_mounted() for USB.begin() poll (v4.4)
#include "hal/usb_serial_jtag_ll.h"  // usb_serial_jtag_ll_phy_enable_pad() for post-boot silent
#include "esp_timer.h"           // esp_timer_get_time() for live CPU busy% (v4.10)
#include <vector>
#include <map>
#include <algorithm>
#include <Preferences.h>
#include <esp_partition.h>     // Watch port: flash-time clock blob at boot

#include "Config.h"           // brings in SD_MMC + `#define SD SD_MMC` shim
#include "GlobalState.h"
#include "LEDManager.h"
#include "FSManager.h"
#include "LogManager.h"
#include "USBManager.h"
#include "WiFiManager.h"
#include "DuckyInterpreter.h"
#include "WebServerManager.h"
#include "BTManager.h"
#include "deadnet.h"        // DeadNet ARP/RA/DEAUTH/DNS attack engine port
#include "AttackMode.h"
#include "MSCManager.h"
#include "UpdateManager.h"
#include "MSCManager.h"     // mscBegin() for BEHAVE_BROKEN mode
#include "ComShell.h"       // v4.5: "Allow COM connections" — USB CDC shell for PuTTY over COM port
#include "WatchUI.h"        // watch port: AMOLED status panel + on-screen STOP button
// Force arduino-cli library discovery for the transitive deps WatchUI.cpp uses:
#include <Arduino_GFX_Library.h>
#include <Arduino_DriveBus_Library.h>
#include <lvgl.h>
#include <XPowersLib.h>

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
// Watch port perf: file-scope cache for WiFi.softAPgetStationNum(). Was
// called 3x per ~1 kHz loop; every call goes through the WiFi driver.
// Refreshed at 4 Hz in loop() below.
static int      g_apStations   = 0;
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
String        selectedScriptName    = "";      // script tapped in Files → Home Play
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
  if (g_apStations > 0) {   // cached at 4 Hz in loop(); see g_apStations decl.
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
    bootPrefs.begin("badusb", false);
    // First-boot enforced-silent: on a fresh flash (fb_done not yet set)
    // force silent_boot=true unconditionally so the watch never enumerates
    // as JTAG on first power-up. Subsequent boots honour whatever the user
    // set in the AMOLED / web toggle. Set the "done" flag AFTER we arm
    // silent — a mid-write reset falls back through the same first-boot
    // path on the next attempt, which is exactly what we want.
    bool firstBootDone = bootPrefs.getBool("fb_done", false);
    bool silent = bootPrefs.getBool("silent_boot", false);
    if (!firstBootDone) {
      silent = true;
      bootPrefs.putBool("silent_boot", true);
      bootPrefs.putBool("fb_done",     true);
    }
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

  // Watch port: bring the AMOLED + touch up. Must run AFTER STEP 0's silent-
  // boot USB PHY kill so the display init can't stall the ~1 ms window
  // Windows uses for USB enumeration.
  watchUiBegin();
  watchUiSetStatus("Booting...");
  watchUiSetLed(GpioLed::Color(0, 0, 255), 0);   // solid blue during setup

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
    const uint32_t FIRMWARE_STAMP = 437;   // v4.37 - AttackMode bug-hunt #6 fix: substring "OFF"/"BLANK"/"NONE" hunter false-positived on numeric tokens like VID_0FF0, silently coercing ATTACKMODE into BLANK (hid=false, storage=false) and leaving units enumerating only as JTAG. Fix uses the parsed token flags. Bump forces the FIRMWARE_STAMP != stored clause below to reset the stale (hid=false, msc=true, am_no_hid_intent=true) trio on units that already got wedged by the old bug.
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

  // ---- Flash-time clock blob ------------------------------------------
  // The WebFlasher writes a tiny binary at the head of the coredump
  // partition (offset 0x3F0000) containing:
  //   magic  : uint32  0xC10CBA5E ("clockbase")
  //   epoch  : uint64  UTC seconds at the moment of flash
  //   tz     : int32   local timezone offset in seconds
  //   pad    : uint32
  // If the magic matches we treat the epoch as the synchronised time,
  // then erase the blob so subsequent boots don't keep resetting the
  // clock to flash-day. If the wearer later opens the dashboard, that
  // path (/api/set-time) overwrites the pref with a fresher browser
  // clock — flash-time is just the bootstrap so HH:MM is right OUT of
  // the box.
  {
    const esp_partition_t* p = esp_partition_find_first(
        ESP_PARTITION_TYPE_DATA, ESP_PARTITION_SUBTYPE_DATA_COREDUMP, NULL);
    if (p) {
      struct __attribute__((packed)) { uint32_t magic; uint64_t epoch;
                                       int32_t tz; uint32_t pad; } blob = {};
      if (esp_partition_read(p, 0, &blob, sizeof(blob)) == ESP_OK
          && blob.magic == 0xC10CBA5EU
          && blob.epoch >= 1700000000ULL) {
        Serial.printf("[BOOT] flash-time blob: epoch=%llu tz=%d\n",
                      (unsigned long long)blob.epoch, (int)blob.tz);
        preferences.putULong64("clock_epoch",   blob.epoch);
        preferences.putLong   ("clock_tz_secs", blob.tz);
        preferences.putULong  ("clock_sync_ms", millis());
        esp_partition_erase_range(p, 0, 4096);   // one-shot; clear now.
      }
    }
  }

  // ---- First-boot "all user toggles OFF" migration --------------------
  // Deliberately DOES NOT touch silent_boot or am_hid — those affect USB
  // PHY / HID composite at boot; a previous attempt migrated silent_boot
  // and caused a USB re-enum loop. This migration only clears user-facing
  // feature toggles + the boot script. Guarded by prefs_off_v2 so it runs
  // once per NVS.
  if (!preferences.getBool("prefs_off_v2", false)) {
    Serial.println("[BOOT] prefs_off_v2 migration — clearing user toggles");
    // Bug-hunt round C-2: write the migration-done flag LAST. Previously it
    // was put FIRST — a power loss between that commit and the toggle
    // writes below marked the migration "done" on flash while leaving stale
    // toggle values, so the next boot would skip the migration and inherit
    // whatever the prior session persisted (e.g. wifi_toggle=true / com_on=
    // true from a factory image). Now: reset every toggle first, THEN set
    // the flag. A power loss anywhere in the middle just leaves the flag
    // clear so the migration re-runs on the next boot.
    preferences.putBool("led_enabled",     false);
    preferences.putBool("logging_enabled", false);
    preferences.putBool("autoconnect",     false);
    preferences.putBool("save_creds",      false);
    preferences.putBool("bt_discovery",    false);
    preferences.putBool("wifi_toggle",     false);
    preferences.putBool("bt_toggle",       false);
    preferences.putBool("com_on",          false);
    preferences.putBool("autostart_on",    false);
    preferences.remove ("boot_script");
    // silent_boot deliberately NOT touched.
    preferences.putBool("prefs_off_v2",    true);   // flag LAST — see comment above
  }

  // ---- Bricked-mode gate ---------------------------------------------
  // If the wearer picked "Brick Firmware" in Settings, freeze here in a
  // clock-only loop. Placed AFTER watchUiBegin() so the clock is live,
  // and BEFORE any WiFi/BT/USB setup so a bricked unit doesn't beacon.
  if (preferences.getBool("bricked", false)) {
    Serial.println("[BOOT] bricked=true — blackscreen until reflash");
    watchUiEnterBrickBlackscreen();   // wipes UI, paints pure black
    for (;;) {
      watchUiTick();
      delay(50);
    }
  }

  ap_ssid = preferences.getString("ap_ssid", DEFAULT_AP_SSID);
  ap_password = preferences.getString("ap_password", DEFAULT_AP_PASSWORD);
  currentLanguage = preferences.getString("language", "us");
  wifiScanTime = preferences.getInt("wifi_scan_time", WIFI_SCAN_TIMEOUT);
  ledEnabled = preferences.getBool("led_enabled", false);
  loggingEnabled = preferences.getBool("logging_enabled", false);
  autoConnectEnabled = preferences.getBool("autoconnect", false);
  saveOnConnectEnabled = preferences.getBool("save_creds", false);
  btDiscoveryEnabled = preferences.getBool("bt_discovery", false);
  // silent_boot default kept at TRUE for backwards-compat with the STEP 0
  // gate that reads it — do NOT change this line without also auditing
  // STEP 0's silent_boot logic. Prior sessions may have persisted true;
  // that's expected and STEP 0 handles it.
  silentStartup = preferences.getBool("silent_boot", true);
  // New user-facing toggles — default OFF so a fresh unit is quiet.
  wifiToggleEnabled      = preferences.getBool("wifi_toggle", false);
  bluetoothToggleEnabled = preferences.getBool("bt_toggle",   false);
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
  // Feed the initial list into the watch UI's Files tab.
  watchUiSetFileList(availableScripts);

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

  // Bootscript gate — load the script text if EITHER trigger is armed:
  //   * "Autorun at boot"        (autostart_on)  — fires on ESP boot
  //   * "Autostart on USB attach"(autoattach_on) — fires on USB-C mount
  // The script FILE is the same for both; only the trigger differs.
  // bootModeEnabled gates the boot-fires-once branch below. The USB-
  // attach branch reads bootScript directly, so we must load it too
  // when only autoattach is armed.
  bool autostartEnabled = preferences.getBool("autostart_on",  false);
  bool autoattachEnabled= preferences.getBool("autoattach_on", false);
  bool anyTriggerArmed  = autostartEnabled || autoattachEnabled;
  String bootPref = preferences.getString("boot_script", "");
  currentBootScriptFiles.clear();
  bootScript = "";

  if (!anyTriggerArmed) {
    if (bootPref.length() > 0 || SD.exists(String(DIR_SCRIPTS) + "/boot.txt")) {
      Serial.println("[BOOT] Autorun + Autostart both disabled - boot script not loaded");
    }
  } else if (bootPref.length() > 0) {
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
      // bootModeEnabled arms the "fires on first WiFi client connect"
      // path in loop(). Only enable it when Autorun is on — Autostart
      // (USB-attach) uses a different guard and mustn't double-fire.
      bootModeEnabled = autostartEnabled;
      Serial.println("Boot scripts loaded: " + bootPref);
    }
  } else if (SD.exists(String(DIR_SCRIPTS) + "/boot.txt")) {
    bootScript = loadScript("boot.txt");
    bootModeEnabled = autostartEnabled;
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

  // Watch port: setupAP() always runs so the WiFi / LWIP / TCP-IP stack
  // gets initialized (WebServer.begin() below assumes it has). Downstream
  // code takes a semaphore that only exists once the WiFi driver has
  // brought its queues up; a full skip crashes with "xQueueSemaphoreTake
  // queue.c:1709 (( pxQueue ))" the moment the server binds. If the
  // wearer wants WiFi off, we bring softAP down immediately after — the
  // stack stays alive, only the radio + AP go silent.
  setupAP();
  {
    IPAddress ip = WiFi.softAPIP();
    watchUiSetAP(ap_ssid.c_str(), ap_password.c_str(), ip.toString().c_str());
  }
  if (!wifiToggleEnabled) {
    Serial.println("[BOOT] WiFi disabled by pref — bringing softAP down after init");
    stopAP();
    watchUiSetAP("WiFi OFF", "-", "enable in Settings");
  }
  // Sync the on-screen Settings switches to the real state now that prefs
  // are loaded.
  {
    bool comOn     = preferences.getBool("com_on",       false);
    bool autostart = preferences.getBool("autostart_on", false);
    watchUiRefreshSettings(
        /*wifi*/    wifiToggleEnabled,
        /*bt*/      bluetoothToggleEnabled,
        /*btdisc*/  btDiscoveryEnabled,
        /*led*/     ledEnabled,
        /*silent*/  silentStartup,
        /*logging*/ loggingEnabled,
        /*com*/     comOn);
    watchUiSetAutostartToggle(autostart);
    watchUiSetAutoattachToggle(preferences.getBool("autoattach_on", false));
    // Personalization page state — brightness, screen-sleep, PIN, duress.
    watchUiRefreshExtras(
        preferences.getBool  ("screen_sleep_on", false),
        preferences.getInt   ("brightness",     100),
        preferences.getString("pin_value",       "").c_str(),
        preferences.getString("duress_value",    "").c_str());
    watchUiApplyBrightness(preferences.getInt("brightness", 100));
  }
  // Refresh the Files tab now the autostart target is known — light up the
  // star on the matching row if a boot_script is set.
  watchUiSetFileList(availableScripts,
                     preferences.getString("boot_script", ""));
  bluetoothName = preferences.getString("bt_name", "ESP32-S3");
  // Watch port: only init BLE if the user has turned it on. The BLE stack
  // consumes ~40 KB of internal heap and this watch's build lands the
  // WiFi driver in a coex state where BLE `hci inits failed` reliably —
  // the subsequent createServer returns NULL and setCallbacks() panics
  // with StoreProhibited (confirmed via serial). The user toggle in
  // Settings does a lazy setupBT() when they actually want BT.
  if (bluetoothToggleEnabled) {
    setupBT();
  } else {
    Serial.println("[BT] Skipping BLE init at boot (toggle off)");
  }
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
  // The passphrase is not printed to serial; only its length.
  Serial.print("Password: (hidden, ");
  Serial.print(ap_password.length());
  Serial.println(" chars)");
  Serial.println("Open browser and go to: 192.168.4.1");

  // User report: silent USB "didn't work on first boot, but worked after
  // retoggling". Root cause: Serial.begin() at line 371 re-enables the
  // USB-Serial/JTAG PHY that STEP 0's usbBeginSilent() had disabled, so
  // the host still saw the JTAG device throughout the whole boot. Once
  // boot logging is done and no more Serial.println is expected, re-kill
  // the JTAG PHY. USB.begin() (if any) already ran above and uses the
  // FSLS PHY pads, not the JTAG PHY, so this doesn't affect HID/MSC.
  if (silentStartup && !usbStarted) {
    // Only when we haven't brought up TinyUSB — that means the wearer is
    // in true stealth (no HID, no MSC, no CDC). Kill the JTAG PHY so
    // the host sees literally nothing.
    usb_serial_jtag_ll_phy_enable_pad(false);
  }
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
  // Watch port perf: rate-limit the auxiliary loops. loop() runs at ~1 kHz;
  // captive DNS is fine at 100 Hz, CDC shell at 100 Hz.
  {
    static unsigned long s_lastNet = 0;
    if (millis() - s_lastNet >= 10) {
      s_lastNet = millis();
      loopCaptivePortal();   // service captive-portal DNS
      comShellLoop();        // v4.5: COM-port shell over USB CDC (if enabled)
    }
  }
  hostLedTick();         // v4.23: copy LED byte from USB task -> variables safely on main task
  handleLED();
  cpuMgmtTick();         // drop to 80 MHz after 5 s idle (thermal management)

  // Watch port: pump LVGL, redraw the LED dot / status line / clients count,
  // and consume on-screen STOP-button presses. Non-blocking (~2 ms typical).
  watchUiTick();

  // Clock face — update HH:MM every second while the overlay is visible.
  // Uses uptime, not NTP, because this device runs offline.
  // Clock — feed real epoch time when we have one (POSTed by the web
  // dashboard's script.js via /api/set-time, persisted in NVS as
  // clock_epoch + clock_tz_secs + clock_sync_ms). Otherwise fall back
  // to uptime.
  {
    static unsigned long lastClock = 0;
    if (watchUiClockVisible() && millis() - lastClock >= 1000) {
      lastClock = millis();
      uint64_t syncEpoch = preferences.getULong64("clock_epoch",  0);
      int32_t  tz        = preferences.getLong   ("clock_tz_secs", 0);
      uint32_t syncMs    = preferences.getULong  ("clock_sync_ms", 0);
      uint64_t nowSecs;
      if (syncEpoch >= 1700000000ULL) {
        // Add the milliseconds that have passed on the local clock since
        // the sync. millis() wraps ~49.7 d — for a watch that gets an
        // occasional resync via the dashboard this is fine.
        uint32_t elapsedMs = millis() - syncMs;   // unsigned wrap OK
        nowSecs = syncEpoch + (elapsedMs / 1000) + tz;
      } else {
        nowSecs = millis() / 1000;                // uptime fallback
      }
      watchUiSetClockSeconds((uint32_t)(nowSecs & 0xFFFFFFFFULL));
    }
  }

  // Inactivity screen-off — GATED behind the "Auto screen sleep" toggle
  // in Settings. Default OFF per user request; the wearer opts in via
  // the Personalization page.
  if (preferences.getBool("screen_sleep_on", false)) {
    static unsigned long lastCheck = 0;
    if (millis() - lastCheck >= 500) {
      lastCheck = millis();
      uint32_t idle = lv_display_get_inactive_time(NULL);
      const uint32_t SLEEP_MS = 60000;
      if (!watchUiScreenAsleep() && idle >= SLEEP_MS) {
        watchUiScreenSleep();
      } else if (watchUiScreenAsleep() && idle < SLEEP_MS) {
        watchUiScreenWake();
      }
    }
  } else if (watchUiScreenAsleep()) {
    // Turned off mid-sleep — wake it up so the user isn't stuck black.
    watchUiScreenWake();
  }
  if (watchUiConsumeStopPressed()) {
    if (scriptRunning) {
      stopRequested = true;
      setLEDMode(4);
      Serial.println("Stop requested via on-screen button");
      logCommand("SCRIPT_STOPPED", "User requested stop via on-screen STOP");
      watchUiFlash("STOP requested");
    } else {
      watchUiFlash("No script is running");
    }
  }

  // Watch port: consume any settings the user toggled on the AMOLED
  // settings screen. LVGL callbacks only stashed the intent — we apply
  // it here on the main task so WiFi/BT/NVS never get touched from the
  // LVGL event thread. Rate-limited to 50 Hz — human tap events are on
  // human-time scales, no point polling this at 1 kHz.
  static unsigned long s_lastSet = 0;
  if (millis() - s_lastSet >= 20) {
    s_lastSet = millis();
    WatchUiPendingSettings p = watchUiConsumePendingSettings();
    auto putIfChanged = [](const char* key, bool v){
      // Bug-hunt #19: was `getBool(key, !v)` — that reads the opposite as
      // the missing-key default, so a MISSING key + v==true returned false
      // and always wrote (harmless once), but a MISSING key + v==false
      // returned true and always wrote too. Every first-boot toggle
      // triggered an NVS write even when its default was already the
      // desired value — quiet NVS wear. Correct guard: read with `v` as
      // the default, so a missing key returns v and the write is skipped.
      if (preferences.getBool(key, v) != v) preferences.putBool(key, v);
    };
    if (p.has_led) {
      ledEnabled = p.led_on;
      putIfChanged("led_enabled", ledEnabled);
      if (!ledEnabled) setLED(0, 0, 0);
      watchUiFlash(ledEnabled ? "LED ON" : "LED OFF");
    }
    if (p.has_logging) {
      loggingEnabled = p.logging_on;
      putIfChanged("logging_enabled", loggingEnabled);
      watchUiFlash(loggingEnabled ? "Logging ON" : "Logging OFF");
    }
    if (p.has_silent) {
      // User report: web toggle for silent USB "works" in the sense
      // that the pref writes, but the ACTUAL USB behaviour requires a
      // reboot to take effect — because previously we only wrote NVS
      // and toasted "next reboot". Now apply LIVE:
      //   silent ON  → hidReleaseIfSilent()-style detach + kill both
      //                PHYs (TinyUSB + ROM USB-Serial/JTAG) so the
      //                host sees an unplug in seconds.
      //   silent OFF → silentRestorePadsForUsb() re-enables the PHY
      //                pads; ensureHidReady() brings USB.begin() +
      //                HID attach up on-demand next time a script or
      //                live-type fires. (Full re-enum still needs the
      //                host to bind the driver — that takes a few
      //                seconds; toast reflects this.)
      silentStartup = p.silent_on;
      putIfChanged("silent_boot", silentStartup);
      if (silentStartup) {
        // Bug-hunt round C-4: preserve usbStarted across the live silent-on
        // path. usbBeginSilent() unconditionally sets usbStarted=false (its
        // BOOT-TIME job is to promise "USB.begin() has NOT been called
        // yet"); but on a LIVE toggle USB.begin() has already been called
        // this session. If we let usbStarted flip to false, the very next
        // silent-off toggle takes the `if (!usbStarted) USB.begin()` branch
        // below and calls USB.begin() a SECOND time on an already-init
        // TinyUSB stack — reproducible descriptor rebuild crash after
        // silent-on → silent-off → silent-on. Save + restore around it.
        bool wasStarted = usbStarted;
        hidDetach();          // presents unplug to host
        usbBeginSilent();     // pull-ups off + FSLS PHY down + JTAG PHY down
        usbStarted = wasStarted;   // restore — see comment above
        watchUiFlash("Silent USB: on now");
      } else {
        silentRestorePadsForUsb();  // pull-ups + PHY pads back
        // If USB.begin() has never been called yet (we booted silent),
        // start it now so CDC + HID come up right away.
        if (!usbStarted) {
          USB.begin();
          usbStarted = true;
          hidConnected = true;
        } else {
          hidAttach();        // present replug on the existing stack
        }
        watchUiFlash("Silent USB: off - HID re-attaching");
      }
    }
    if (p.has_bt) {
      bluetoothToggleEnabled = p.bt_on;
      putIfChanged("bt_toggle", bluetoothToggleEnabled);
      if (bluetoothToggleEnabled) { setupBT(); watchUiFlash("Bluetooth ON"); }
      else                        { stopBT();  watchUiFlash("Bluetooth OFF"); }
    }
    if (p.has_btdisc) {
      btDiscoveryEnabled = p.btdisc_on;
      putIfChanged("bt_discovery", btDiscoveryEnabled);
      watchUiFlash(btDiscoveryEnabled ? "BT discovery ON" : "BT discovery OFF");
    }
    if (p.has_wifi) {
      // Live: bring softAP up or down without a reboot so the home tab
      // reflects the change immediately.
      wifiToggleEnabled = p.wifi_on;
      putIfChanged("wifi_toggle", wifiToggleEnabled);
      if (wifiToggleEnabled) {
        setupAP();
        IPAddress ip = WiFi.softAPIP();
        watchUiSetAP(ap_ssid.c_str(), ap_password.c_str(), ip.toString().c_str());
        watchUiFlash("WiFi ON");
      } else {
        stopAP();
        watchUiSetAP("WiFi OFF", "-", "enable in Settings");
        watchUiFlash("WiFi OFF");
      }
    }
    if (p.has_com) {
      preferences.putBool("com_on", p.com_on);
      watchUiFlash(p.com_on ? "COM shell: next reboot"
                            : "COM shell off: next reboot");
    }
    if (p.has_screen_sleep) {
      preferences.putBool("screen_sleep_on", p.screen_sleep_on);
      watchUiFlash(p.screen_sleep_on ? "Auto sleep ON" : "Auto sleep OFF");
    }
    if (p.has_brightness) {
      preferences.putInt("brightness", p.brightness_pct);
      // Apply live via MIPI DCS 0x51 (CO5300 supports it), 0..255 scaled
      // from percent. 10% ~= 25/255 = lowest legible on this AMOLED.
      watchUiApplyBrightness(p.brightness_pct);
    }
    if (p.has_pin) {
      preferences.putString("pin_value", String(p.pin_value));
      watchUiRefreshExtras(
        preferences.getBool  ("screen_sleep_on", false),
        preferences.getInt   ("brightness",     100),
        preferences.getString("pin_value",       "").c_str(),
        preferences.getString("duress_value",    "").c_str());
    }
    if (p.has_duress) {
      preferences.putString("duress_value", String(p.duress_value));
      watchUiRefreshExtras(
        preferences.getBool  ("screen_sleep_on", false),
        preferences.getInt   ("brightness",     100),
        preferences.getString("pin_value",       "").c_str(),
        preferences.getString("duress_value",    "").c_str());
    }
    if (p.has_selected) {
      // Selected script — just persist for later; the Home Play button
      // will consume it via s_pending.want_play (below).
      selectedScriptName = String(p.selected_name);
    }
    if (p.want_play) {
      // Same effect as tapping ▶ in Files but on the currently-selected
      // script. If none is selected, just toast.
      if (selectedScriptName.length() == 0) {
        watchUiFlash("No script selected - tap one in Files");
      } else if (scriptRunning) {
        watchUiFlash("A script is already running");
      } else if (!sdCardPresent) {
        watchUiFlash("No SD card");
      } else {
        String script = loadScript(selectedScriptName);
        if (script.length() == 0) {
          watchUiFlash("Script empty or missing");
        } else {
          watchUiFlash((String("Running ") + selectedScriptName).c_str());
          pendingScript = script;
          pendingScriptReady = true;
        }
      }
    }
    if (p.want_reboot) {
      // Bug-hunt (blocker #1 for autorun): LVGL fires the toggle-Autorun
      // callback AND the Reboot-button callback in the same lv_task_handler
      // pass, so BOTH s_pendingExtras.has_autostart AND s_pending.want_reboot
      // are set when we get here. The s_pendingExtras block (below) is
      // consumed AFTER s_pending, so without this pre-flush the reboot path
      // calls preferences.end() + restart BEFORE `autostart_on` ever hits
      // NVS, and the wearer's just-flipped Autorun switch is silently lost
      // on the reboot they triggered right after it. Drain the extras that
      // affect boot NVS here first so they always land.
      {
        WatchUiPendingExtras drain = watchUiConsumePendingExtras();
        if (drain.has_autostart) {
          preferences.putBool("autostart_on", drain.autostart_on);
          if (drain.autostart_on) preferences.putBool("autoattach_on", false);
        }
        if (drain.has_autoattach) {
          preferences.putBool("autoattach_on", drain.autoattach_on);
          if (drain.autoattach_on) preferences.putBool("autostart_on", false);
        }
      }
      watchUiFlash("Rebooting...");
      for (int i = 0; i < 20; ++i) { watchUiTick(); delay(20); }
      // Bug-hunt finding #8: flush NVS before restarting so a putBool from
      // the same tick isn't lost when TinyUSB shutdown cuts power to the
      // NVS write task. Then usb_persist_restart(RESTART_NO_PERSIST) does
      // the clean shutdown before reset — plain ESP.restart() on the S3
      // can leave the ROM stub in a half-init state and boot to download.
      preferences.end();
      usb_persist_restart(RESTART_NO_PERSIST);
    }
    if (p.want_factory_reset) {
      watchUiFlash("Factory reset - hold WIPE 5 s to confirm");
      // (Full factory-reset arm/confirm handled elsewhere in loop.)
    }
  }

  // Files-tab action bridge: run/delete requests queued from the watch UI
  // execute on the main task so they don't tie up LVGL.
  {
    WatchUiPendingScriptAction pa = watchUiConsumePendingScriptAction();
    if (pa.has_run) {
      if (scriptRunning) {
        watchUiFlash("A script is already running");
      } else if (!sdCardPresent) {
        watchUiFlash("No SD card");
      } else {
        String script = loadScript(String(pa.run_name));
        if (script.length() == 0) {
          watchUiFlash("Script empty or missing");
        } else {
          watchUiFlash((String("Running ") + pa.run_name).c_str());
          // Defer execution one tick so the toast paints first.
          pendingScript = script;
          pendingScriptReady = true;
        }
      }
    }
    if (pa.has_delete) {
      if (scriptRunning) {
        watchUiFlash("Can't delete while running");
      } else if (deleteScript(String(pa.delete_name))) {
        watchUiFlash((String("Deleted ") + pa.delete_name).c_str());
        loadAvailableScripts();
        watchUiSetFileList(availableScripts,
                           preferences.getString("boot_script", ""));
      } else {
        watchUiFlash("Delete failed");
      }
    }
    if (pa.has_toggle_autostart) {
      String newTarget = String(pa.autostart_name);
      if (newTarget.length() == 0) {
        preferences.remove("boot_script");
        watchUiFlash("Autostart cleared");
      } else {
        preferences.putString("boot_script", newTarget);
        watchUiFlash((String("Autostart: ") + newTarget).c_str());
      }
      watchUiSetFileList(availableScripts, newTarget);
    }
  }

  // Autostart toggle + Reset-to-standard + Brick pending actions
  {
    WatchUiPendingExtras pe = watchUiConsumePendingExtras();
    if (pe.has_autostart) {
      preferences.putBool("autostart_on", pe.autostart_on);
      // Mutual exclusion enforced on the ESP side too — flipping one on
      // forces the other off in NVS so the two triggers can never both
      // fire on the same script.
      if (pe.autostart_on) preferences.putBool("autoattach_on", false);
      watchUiFlash(pe.autostart_on ? "Autorun ON (fires on next boot)"
                                   : "Autorun OFF");
    }
    if (pe.has_autoattach) {
      preferences.putBool("autoattach_on", pe.autoattach_on);
      if (pe.autoattach_on) preferences.putBool("autostart_on", false);
      watchUiFlash(pe.autoattach_on ? "Autostart ON (fires on USB attach)"
                                    : "Autostart OFF");
    }
    if (pe.want_reset_std) {
      // Match the prefs_off_v2 migration set exactly — silent_boot + am_hid
      // deliberately NOT touched so USB behaviour on next boot is unchanged.
      preferences.putBool("led_enabled",     false);
      preferences.putBool("logging_enabled", false);
      preferences.putBool("autoconnect",     false);
      preferences.putBool("save_creds",      false);
      preferences.putBool("bt_discovery",    false);
      preferences.putBool("wifi_toggle",     false);
      preferences.putBool("bt_toggle",       false);
      preferences.putBool("com_on",          false);
      preferences.putBool("autostart_on",    false);
      preferences.remove("boot_script");
      watchUiFlash("Reset to standard - rebooting");
      for (int i = 0; i < 30; ++i) { watchUiTick(); delay(20); }
      preferences.end();                          // #8: flush the 10 puts above
      usb_persist_restart(RESTART_NO_PERSIST);
    }
    if (pe.want_brick) {
      preferences.putBool("bricked", true);
      watchUiFlash("Bricking - reflash to recover");
      for (int i = 0; i < 40; ++i) { watchUiTick(); delay(20); }
      preferences.end();                          // #8: persist bricked=true
      usb_persist_restart(RESTART_NO_PERSIST);
    }
    if (pe.want_duress) {
      // Duress fired from the clock's PIN pad. Wipe every user script
      // from SD (keep the web UI's HTML/CSS/JS intact), clear autostart,
      // then brick the firmware. Reflash required to recover.
      if (sdCardPresent) {
        loadAvailableScripts();
        for (const String& n : availableScripts) {
          deleteScript(n);
        }
      }
      preferences.remove("boot_script");
      preferences.putBool("bricked", true);
      preferences.end();
      usb_persist_restart(RESTART_NO_PERSIST);
    }
    if (pe.has_deadnet) {
      // Watch-side DeadNet toggle. Gate on WiFi STA connected (the LAN
      // is the network we joined via WiFi). AMOLED start is always ARP
      // only — the risky modes (DEAUTH, SNIFF) need explicit opt-in via
      // the web dashboard which can show the warning first.
      if (pe.deadnet_on) {
        if (WiFi.status() != WL_CONNECTED) {
          watchUiFlash("DeadNet: not joined to any WiFi");
          watchUiSetDeadnetToggle(false);
        } else if (g_deadnet.startAttack(ATTACK_MODE_ARP)) {
          // "Everything off except WiFi" - detach USB HID/MSC + stop BT.
          hidDetach();
          if (bluetoothToggleEnabled) { stopBT(); bluetoothToggleEnabled = false; }
          if (preferences.getBool("bt_toggle", false) != false)
            preferences.putBool("bt_toggle", false);
          watchUiFlash("DeadNet armed - HID + BT off");
        } else {
          watchUiFlash("DeadNet: start failed");
          watchUiSetDeadnetToggle(false);
        }
      } else {
        g_deadnet.stopAttack();
        hidAttach();
        watchUiFlash("DeadNet stopped - HID re-attached");
      }
    }
  }

  // Watch-side LAN + DeadNet status: refresh the badge in Settings and
  // keep the AMOLED switch in sync with the actual running state (a web
  // toggle should reflect on-screen and vice versa). Cheap 1 Hz poll.
  {
    static unsigned long lastLanPoll = 0;
    if (millis() - lastLanPoll >= 1000) {
      lastLanPoll = millis();
      bool staUp = (WiFi.status() == WL_CONNECTED);
      watchUiSetLanConnected(staUp, staUp ? WiFi.SSID().c_str() : "not joined");
      watchUiSetDeadnetToggle(g_deadnet.isRunning());
    }
  }

  // Cheap status/clients pull — 4 Hz is fine for a human-facing readout.
  {
    static unsigned long lastUi = 0;
    if (millis() - lastUi >= 250) {
      lastUi = millis();
      watchUiSetClients((int)WiFi.softAPgetStationNum());
      const char* st = "Idle";
      if (g_thermalShutdown)     st = "Thermal shutdown";
      else if (updateApplying)   st = "Update applying";
      else if (scriptRunning)    st = "Script running";
      watchUiSetStatus(st);
      watchUiSetScript(scriptRunning ? "web script" : "");
      // Refresh the Files tab when the SD script list changed (web upload
      // /delete, boot script edit). Cheap size-compare guard so we don't
      // rebuild every 250 ms.
      static size_t lastScriptCount = (size_t)-1;
      if (availableScripts.size() != lastScriptCount) {
        lastScriptCount = availableScripts.size();
        watchUiSetFileList(availableScripts,
                           preferences.getString("boot_script", ""));
      }
    }
  }

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
    // Watch port: detect SD hot-plug edges so the UI can react. checkSDCard()
    // updates sdCardPresent; compare against the previous value and show a
    // banner + refresh the Files tab whenever it changes.
    static bool lastSdPresent = sdCardPresent;
    checkSDCard();
    if (sdCardPresent != lastSdPresent) {
      lastSdPresent = sdCardPresent;
      if (sdCardPresent) {
        watchUiSetBanner(LV_SYMBOL_SD_CARD "  SD card inserted", 0x2A5A2A);
        loadAvailableScripts();
        watchUiSetFileList(availableScripts,
                           preferences.getString("boot_script", ""));
      } else {
        watchUiSetBanner(LV_SYMBOL_WARNING "  SD card removed", 0x5A2A2A);
        availableScripts.clear();
        watchUiSetFileList(availableScripts, String());
      }
    }
  }

  // v4.24: button polling moved to pumpButton() so WAIT_FOR_BUTTON_PRESS
  // shares the same debounced state machine (10s factory-reset still fires
  // during a WAIT; BUTTON_DEF handler auto-runs on short-press when idle).
  // Watch port perf: gate to 200 Hz — debounce is 40 ms, no need for 1 kHz.
  {
    static unsigned long s_lastBtn = 0;
    if (millis() - s_lastBtn >= 5) { s_lastBtn = millis(); pumpButton(); }
  }

  // Status updates
  if (millis() - lastStatusUpdate >= STATUS_UPDATE_INTERVAL) {
    lastStatusUpdate = millis();
    if (scriptRunning) {
      unsigned long elapsed = (millis() - scriptStartTime) / 1000;
      Serial.println("Script running for " + String(elapsed) + " seconds");
    }
  }

  // Watch port perf: cache softAPgetStationNum() at 4 Hz. Was being called
  // 3x per loop iteration (~1 kHz), each hitting the WiFi driver. g_apStations
  // is file-scope so cpuMgmtTick() reads it too.
  static unsigned long s_lastApSta = 0;
  if (millis() - s_lastApSta >= 250) {
    s_lastApSta = millis();
    g_apStations = (int)WiFi.softAPgetStationNum();
  }

  // "Autorun at boot" — fires the bootscript ONCE per boot as soon as
  // USB HID is ready. User semantic per most-recent spec: "at boot"
  // means "when the ESP starts up", not "when a WiFi client connects".
  // With silent USB on, ensureHidReady() brings the PHY up first;
  // then waits (bounded) for tud_mounted so keystrokes reach the host.
  if (bootModeEnabled) {
    static bool bootScriptHasRun = false;
    if (!bootScriptHasRun && !scriptRunning && bootScript.length() > 0) {
      bootScriptHasRun = true;
      Serial.println("[BOOT] Autorun armed - executing boot script");
      logCommand("BOOT_SCRIPT", "Autorun: firing at boot");
      ensureHidReady();
      unsigned long t0 = millis();
      while (!tud_mounted() && (millis() - t0) < 1500) delay(10);
      executeScript(bootScript);
    }
  }

  // "Autostart on USB attach" — poll VBUS at ~4 Hz for hot-plug edges.
  //
  // Bug-hunt blocker #1: initial value of s_wasVbus is FALSE, not true.
  // A wearer's typical test flow is: enable autoattach on the AMOLED, then
  // reboot with USB still plugged in. Static-local initialisers fire ONCE
  // per program execution, so an initial value of `true` meant the FIRST
  // poll after that reboot saw (nowVbus=true, s_wasVbus=true) and neither
  // branch fired — the wearer never saw autoattach trigger unless they
  // physically unplugged and re-plugged. `false` treats the first VBUS-
  // present observation as an attach edge, so booting plugged-in AND
  // hot-plugging both fire the payload as the wearer expects.
  {
    static unsigned long s_lastVbusPoll = 0;
    static bool s_wasVbus     = false;   // first-observation counts as attach
    static bool s_attachFired = false;
    if (millis() - s_lastVbusPoll >= 250) {
      s_lastVbusPoll = millis();
      bool attachAutostartOn = preferences.getBool("autoattach_on", false);
      bool nowVbus = watchUiVbusPresent();
      if (!nowVbus) {
        if (s_wasVbus) Serial.println("[VBUS] detach");
        s_wasVbus     = false;
        s_attachFired = false;
      } else if (!s_wasVbus) {
        s_wasVbus = true;
        Serial.printf("[VBUS] attach detected (autoattach_on=%d, bootScript.len=%d, scriptRunning=%d)\n",
                      (int)attachAutostartOn, (int)bootScript.length(), (int)scriptRunning);
        if (attachAutostartOn && !s_attachFired && !scriptRunning &&
            bootScript.length() > 0) {
          s_attachFired = true;
          ensureHidReady();
          // Pump LVGL while waiting for host enumeration so the AMOLED
          // doesn't freeze the whole driver-bind window.
          unsigned long t0 = millis();
          while (!tud_mounted() && (millis() - t0) < 1500) { watchUiTick(); delay(10); }
          Serial.println("[VBUS] running bootscript on attach");
          logCommand("BOOT_SCRIPT", "USB attach - running bootscript");
          executeScript(bootScript);
        }
      }
    }
  }

  // Background processing for Rower and Automation
  {
    static unsigned long s_lastRower = 0;
    if (millis() - s_lastRower >= 50) { s_lastRower = millis(); processRower(); }
  }
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

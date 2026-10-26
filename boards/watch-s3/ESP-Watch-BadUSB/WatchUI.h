#ifndef WATCH_UI_H
#define WATCH_UI_H

// WatchUI — the AMOLED front panel for the Watch-BadUSB port. Replaces the
// hardware LED status of the Key with an on-screen LED dot + status + AP
// creds + STOP button + settings modal.

#include <Arduino.h>
#include <stdint.h>
#include <vector>

void watchUiBegin();
void watchUiTick();

void watchUiSetLed(uint32_t rgb, uint16_t blinkMs = 0);
void watchUiFlash(const char* text);
void watchUiSetStatus(const char* text);
void watchUiSetScript(const char* name);
void watchUiSetClients(int n);
void watchUiSetAP(const char* ssid, const char* password, const char* ip);
void watchUiSetProgress(int percent);

bool watchUiConsumeStopPressed();

// ---- Files-tab bridge ----
// The Files tab lists /scripts on the SD card. Tapping the green ▶ button on
// a row queues a run action; tapping the red 🗑 queues a delete. The main
// loop consumes these via watchUiConsumePendingScriptAction() so LVGL never
// blocks on SD I/O.
struct WatchUiPendingScriptAction {
    bool has_run;              char run_name[64];
    bool has_delete;           char delete_name[64];
    bool has_toggle_autostart; char autostart_name[64];  // "" clears
};
WatchUiPendingScriptAction watchUiConsumePendingScriptAction();

// Feed the list to the UI. Pass an ordered vector of script basenames and
// the current autostart target (empty = none); the matching row's star
// paints filled yellow.
void watchUiSetFileList(const std::vector<String>& names,
                        const String& autostartName = String());

// Show a transient banner across the whole home tab: SD inserted / removed,
// power-off countdown, etc. Pass nullptr to hide.
void watchUiSetBanner(const char* text, uint32_t rgb = 0x2A2A30);

// ---- Clock face + swipe reveal -----------------------------------------
// A minimal full-screen clock (HH:MM, uptime-derived) sits on top of the
// tabview at boot. Swipe DOWN dismisses it → tabs visible. Swipe UP anywhere
// on the tabview brings it back. Set the clock's display time (seconds).
void watchUiShowClock();
void watchUiHideClock();
bool watchUiClockVisible();
void watchUiSetClockSeconds(uint32_t seconds);

// ---- Power-off long-press countdown -----------------------------------
// The main loop polls AXP2101 PEK IRQs. On press-edge it calls
// watchUiBeginPowerHold(); each tick refreshes with remaining seconds.
// On release-edge it calls watchUiEndPowerHold(). >0 remaining shows the
// countdown banner in danger red.
void watchUiBeginPowerHold();
void watchUiSetPowerHoldRemaining(int seconds);
void watchUiEndPowerHold();

// ---- Screen sleep ------------------------------------------------------
// The main loop tracks LVGL inactivity and calls these on idle > threshold
// / on touch wake so the AMOLED can go dark to save battery.
void watchUiScreenSleep();
void watchUiScreenWake();
bool watchUiScreenAsleep();

// Real power-off via AXP2101 (bug-hunt #11 — SHUTDOWN was ESP.deepSleep(0),
// which on the watch is CPU-only sleep; the PMU keeps rails alive so the
// chip wakes on any touch/USB event). Requests the PMU to cut power for
// good; if the call fails (PMU not up) falls back to deep sleep.
void watchUiPowerOff();

// ---- settings-tab bridges ----
struct WatchUiPendingSettings {
    bool has_wifi;          bool wifi_on;
    bool has_bt;            bool bt_on;
    bool has_btdisc;        bool btdisc_on;
    bool has_led;           bool led_on;
    bool has_silent;        bool silent_on;
    bool has_logging;       bool logging_on;
    bool has_com;           bool com_on;
    bool has_screen_sleep;  bool screen_sleep_on;   // new: user-optional
    bool has_brightness;    int  brightness_pct;    // 10..100, +/- steps
    bool has_pin;           char pin_value[8];      // "" clears; 4 digits
    bool has_duress;        char duress_value[8];   // "" clears; 4 digits
    bool has_selected;      char selected_name[64]; // script from Files→Home
    bool want_play;                                   // Home ▶ pressed
    bool want_reboot;
    bool want_factory_reset;
};

WatchUiPendingSettings watchUiConsumePendingSettings();
void watchUiRefreshSettings(bool wifi, bool bt, bool btdisc,
                            bool led,  bool silent, bool logging, bool com);
void watchUiRefreshExtras(bool screen_sleep_on, int brightness_pct,
                          const char* pin_value, const char* duress_value);
// Apply display brightness (0..100 percent) via MIPI DCS 0x51. Wraps
// the ScreenClass instance so callers outside WatchUI.cpp don't need to
// include the AMOLED driver header.
void watchUiApplyBrightness(int pct);
// Returns true if VBUS (USB-C power) is currently detected by the
// AXP2101 PMU. Works EVEN when Silent USB is on — VBUS presence is a
// hardware signal, independent of whether we've called USB.begin().
// Used by the "Autostart on USB attach" trigger.
bool watchUiVbusPresent();
void watchUiSetAutostartToggle(bool on);
// Enter a full-black failsafe screen — used when firmware is "bricked"
// via Settings. Deletes all UI (except the black overlay), disables
// touches, blanks the AMOLED to pure black.
void watchUiEnterBrickBlackscreen();
// Mark a script running/paused/idle — Home's Stop button dims when idle,
// Play swaps its icon to Pause/Continue when running/paused.
void watchUiSetScriptState(int state);   // 0=idle, 1=running, 2=paused
void watchUiSetSelectedScript(const char* name);

// ---- Autostart / Reset-to-standard / Brick firmware --------------------
struct WatchUiPendingExtras {
    bool has_autostart;   bool autostart_on;   // fires on ESP BOOT
    bool has_autoattach;  bool autoattach_on;  // fires on USB-C ATTACH
    bool want_reset_std;   // Reset every user toggle to OFF + wipe boot_script
    bool want_brick;       // Set bricked=true; on next boot only the clock runs
    bool has_deadnet;     bool deadnet_on;   // DeadNet start/stop from AMOLED
    bool want_duress;      // Duress code was entered — wipe scripts + brick
};
void watchUiSetAutoattachToggle(bool on);
void watchUiSetDeadnetToggle(bool on);
void watchUiSetLanConnected(bool connected, const char* ssid);
WatchUiPendingExtras watchUiConsumePendingExtras();

typedef void (*WatchUiWalkthroughDone)();
void watchUiShowWalkthrough(const char* ssid, const char* psk, const char* ip,
                            WatchUiWalkthroughDone onDoneCb);
bool watchUiWalkthroughActive();

#endif // WATCH_UI_H

#include "WatchUI.h"
#include "pin_config.h"

// Screen + touch classes come from the QWavey/ESP32-S3-Waveshare-OLED-Watch
// library. They must be installed alongside their transitive deps
// (Arduino_GFX, Arduino_DriveBus, LVGL 9, XPowersLib). See the README.
#include "ESP32-S3-Screen-AMOLED-2.06.h"
#include "ESP32-S3-Touch-AMOLED-2.06.h"
#include <lvgl.h>
#include <XPowersLib.h>
#include <string>
#include <Preferences.h>              // task #2: read clock_epoch/tz/sync_ms
                                       // to render real time on show()
extern Preferences preferences;
// scriptPaused lives in GlobalState — the Home Play button flips it to
// pause/resume a running Ducky script mid-execution.
extern volatile bool scriptPaused;

// Global instances the extern declarations in those headers point at.
ScreenClass  Screen;
TouchClass   Touch;
static XPowersPMU s_pmu;
static bool       s_pmuReady = false;

// ---- Home-tab LVGL objects the setters mutate ------------------------------
static lv_obj_t* s_tabHome    = nullptr;
static lv_obj_t* s_tabFiles   = nullptr;
static lv_obj_t* s_tabSet     = nullptr;
static lv_obj_t* s_statusLbl  = nullptr;
static lv_obj_t* s_scriptLbl  = nullptr;
static lv_obj_t* s_progress   = nullptr;
static lv_obj_t* s_apSsidLbl  = nullptr;
static lv_obj_t* s_apPassLbl  = nullptr;
static lv_obj_t* s_ipLbl      = nullptr;
static lv_obj_t* s_clientsLbl = nullptr;
static lv_obj_t* s_battLbl    = nullptr;
static lv_obj_t* s_flashLbl   = nullptr;
static lv_obj_t* s_bannerLbl  = nullptr;
static lv_obj_t* s_runBanner  = nullptr;   // persistent "RUNNING <name>" strip
static lv_obj_t* s_runBannerLbl = nullptr;
static lv_obj_t* s_stopBtn    = nullptr;   // small red STOP on Home right
static lv_obj_t* s_playBtn    = nullptr;   // small green PLAY on Home left
static lv_obj_t* s_playLbl    = nullptr;   // icon label inside play btn
static lv_obj_t* s_selectedLbl = nullptr;  // "Selected: xyz.txt" on Home
static char      s_selectedName[64] = {0}; // basename of tapped script
static int       s_scriptState = 0;        // 0=idle, 1=running, 2=paused
static lv_obj_t* s_filesList  = nullptr;   // scroll container inside the Files tab
static lv_obj_t* s_filesEmpty = nullptr;   // "no scripts yet" placeholder
static lv_obj_t* s_lanStatusLbl = nullptr; // Settings-tab LAN badge under DeadNet
static lv_obj_t* s_brightVal  = nullptr;   // brightness "%" label between +/-
static int       s_brightnessPct = 100;    // current brightness
static bool      s_screenSleepOn = false;  // user setting: auto-sleep enabled?
static char      s_pinValue[8]   = {0};    // 4-digit PIN or ""
static char      s_duressValue[8]= {0};    // 4-digit duress or ""

// ---- Settings-tab switch handles (indexed by SETTING_*) -------------------
enum SettingIdx {
    SET_WIFI = 0, SET_BT, SET_BTDISC, SET_LED,
    SET_SILENT, SET_LOGGING, SET_COM,
    SET_AUTOSTART,   // "Autorun at boot" — script fires on ESP boot
    SET_DEADNET,
    SET_SCREEN_SLEEP,
    SET_AUTOATTACH,  // "Autostart on USB attach" — script fires on USB-C hot-plug
    SET_COUNT
};
static lv_obj_t* s_settingSw[SET_COUNT] = { nullptr };

// ---- state -----------------------------------------------------------------
static uint32_t      s_ledColor       = 0x00FF00;   // green idle default
static uint16_t      s_blinkMs        = 0;
static unsigned long s_lastBlinkTick  = 0;
static bool          s_blinkOn        = true;
static bool          s_stopEdge       = false;
static unsigned long s_flashUntil     = 0;
static unsigned long s_lastLvglTick   = 0;
static unsigned long s_lastBattPoll   = 0;
static unsigned long s_lastPMUProbe   = 0;
static WatchUiPendingSettings s_pending = {};
static WatchUiPendingScriptAction s_pendingAction = {};
static WatchUiPendingExtras s_pendingExtras = {};
static String s_currentAutostart;   // "" = none; used for star highlight
static uint32_t s_bannerUntil = 0;

// Clock overlay + power-hold + screen-sleep state
static lv_obj_t* s_clockRoot   = nullptr;
static lv_obj_t* s_clockTime   = nullptr;   // large HH:MM
static lv_obj_t* s_clockHint   = nullptr;   // "swipe down to reveal"
// lv_tabview replaced with 3 manual containers (see watchUiBegin). The
// class-handler race that made Settings gestures unreachable is gone
// because there's no tabview class handler to compete with our own event
// callbacks. s_activeTab is the source of truth for which of the 3
// containers is currently visible.
static int       s_activeTab   = 0;         // 0=Home, 1=Files, 2=Settings
static lv_obj_t* s_tabView     = nullptr;   // legacy — kept as non-null flag
// Manual tab-strip children — file-scope so the click handler AND the
// gesture handler can both call manualStripRepaint() to re-tint the
// active label and re-show the active-tab underline.
static lv_obj_t* s_manualLbls  [3] = { nullptr };
static lv_obj_t* s_manualUnder [3] = { nullptr };
static bool      s_powerHold   = false;
static bool      s_screenAsleep = false;
// Set true once watchUiEnterBrickBlackscreen() runs. Every UI entry point
// that could re-materialize widgets on the black failsafe (showTab,
// watchUiShowClock, watchUiSetFileList, PIN pad, etc.) must skip when this
// flag is set — otherwise a stray swipe-down on the brick screen redraws
// the clock overlay via the screen-level GESTURE handler, defeating the
// "black until reflash" intent. Bug-hunt R1.
static bool      s_bricked      = false;
extern class ScreenClass Screen;            // forward-declared instance in header

// ---- design tokens ---------------------------------------------------------
// Restrained palette — one neutral ground, one accent, one danger. Nothing
// glows. Nothing gradients. Same colour discipline the hub SVGs use.
static const uint32_t C_BG        = 0x0A0A0B;
static const uint32_t C_SURFACE   = 0x141416;
static const uint32_t C_LINE      = 0x2A2A30;
static const uint32_t C_TEXT      = 0xE8E8EA;
static const uint32_t C_MUTED     = 0x8A8A90;
static const uint32_t C_ACCENT    = 0x3AA7F5;   // subdued blue, not neon
static const uint32_t C_OK        = 0x30D158;
static const uint32_t C_DANGER    = 0xC8442D;
static const uint32_t C_LED_OFF   = 0x101010;

static const int LCD_W = LCD_WIDTH;
static const int LCD_H = LCD_HEIGHT;
// Curvy AMOLED safe-area — the Waveshare 2.06" panel has aggressive
// rounded corners (~60 px radius at the top-left/right where the tab bar
// draws). Text drawn flush to any edge gets sliced. Every full-width row
// now uses (LCD_W - 2*SAFE_X). SAFE_TOP + SAFE_BOT keep the first/last
// row of a paged Settings screen off the curved ends too.
static const int SAFE_X   = 40;
static const int SAFE_TOP = 12;
static const int SAFE_BOT = 20;
// Manual tab-strip height. File-scope so makeSettingsPage() can subtract it
// from the settings-page height instead of a stale hard-coded `56` that
// dated to the old built-in tab-bar (now tab_bar_size = 0). Bug-hunt R2.
// Tab strip: bumped again to 72×14 top footprint for comfy fat-finger
// tap zones on the AMOLED. Curve of the top bezel is roughly 60 px, so
// STRIP_PAD_TOP=14 pushes labels safely below it.
static const int STRIP_H       = 72;
static const int STRIP_PAD_TOP = 14;
static const int DOT_STRIP_H = 30;

static lv_color_t lvhex(uint32_t rgb) { return lv_color_hex(rgb & 0xFFFFFF); }

// Kill every LVGL animation on a single object across the states/parts
// the default theme touches. Called by killAllAnimsRec below on the whole
// screen tree — user asked "REMOVE ANY KIND OF ANIMATIONS", including
// button-press fades, switch-slide, tabview-scroll, focus glow, etc.
// anim_duration = 0 handles widgets that self-animate (switch, slider);
// transition = nullptr handles the theme's LV_STATE_PRESSED fade.
static void killAnimsOn(lv_obj_t* obj) {
    static const lv_style_selector_t combos[] = {
        LV_PART_MAIN      | LV_STATE_DEFAULT,
        LV_PART_MAIN      | LV_STATE_PRESSED,
        LV_PART_MAIN      | LV_STATE_CHECKED,
        LV_PART_MAIN      | LV_STATE_FOCUSED,
        LV_PART_MAIN      | LV_STATE_FOCUS_KEY,
        LV_PART_MAIN      | LV_STATE_EDITED,
        LV_PART_MAIN      | LV_STATE_HOVERED,
        LV_PART_MAIN      | LV_STATE_SCROLLED,
        LV_PART_MAIN      | LV_STATE_DISABLED,
        LV_PART_INDICATOR | LV_STATE_DEFAULT,
        LV_PART_INDICATOR | LV_STATE_CHECKED,
        LV_PART_INDICATOR | LV_STATE_PRESSED,
        LV_PART_KNOB      | LV_STATE_DEFAULT,
        LV_PART_KNOB      | LV_STATE_CHECKED,
        LV_PART_KNOB      | LV_STATE_PRESSED,
        LV_PART_ITEMS     | LV_STATE_DEFAULT,
        LV_PART_ITEMS     | LV_STATE_CHECKED,
        LV_PART_ITEMS     | LV_STATE_PRESSED,
    };
    for (size_t i = 0; i < sizeof(combos)/sizeof(combos[0]); i++) {
        lv_obj_set_style_anim_duration(obj, 0, combos[i]);
        lv_obj_set_style_transition(obj, nullptr, combos[i]);
    }
}

// Recursively kill animations on every child under `obj`. Call once after
// the full UI tree is built so no theme-injected transitions or self-
// animating widget parts remain.
static void killAllAnimsRec(lv_obj_t* obj) {
    if (!obj) return;
    killAnimsOn(obj);
    uint32_t n = lv_obj_get_child_count(obj);
    for (uint32_t i = 0; i < n; i++) {
        killAllAnimsRec(lv_obj_get_child(obj, i));
    }
}

// Zero-time transition descriptor used to squash the default theme's
// LV_STATE_PRESSED fade (~80 ms color-morph on button press). Never pass
// nullptr to lv_obj_set_style_transition — that crashes the LVGL 9
// transition subsystem on the next state change (verified). A VALID
// dsc with time=0 and no properties skips animation without touching
// any freeable pointer.
static lv_style_transition_dsc_t s_zeroTx;
static bool                      s_zeroTxInited = false;
// Props that the LVGL 9 default theme animates between DEFAULT and
// CHECKED/PRESSED — bg color+opa, border color, opacity as a whole. An
// empty props array (INV only) left the theme's own transition intact
// (LVGL falls back to theme dsc if our override has no props to
// interpolate). Listing the props explicitly with time=0 means state
// changes on those props run a zero-duration anim — instant swap.
static const lv_style_prop_t     s_zeroTxProps[] = {
    LV_STYLE_BG_COLOR,
    LV_STYLE_BG_OPA,
    LV_STYLE_BORDER_COLOR,
    LV_STYLE_BORDER_WIDTH,
    LV_STYLE_OPA,
    LV_STYLE_TRANSFORM_SCALE_X,
    LV_STYLE_TRANSFORM_SCALE_Y,
    LV_STYLE_TRANSLATE_X,
    LV_STYLE_TRANSLATE_Y,
    LV_STYLE_PROP_INV,
};

static void ensureZeroTx() {
    if (s_zeroTxInited) return;
    lv_style_transition_dsc_init(&s_zeroTx, s_zeroTxProps,
                                 lv_anim_path_linear, 0, 0, nullptr);
    s_zeroTxInited = true;
}

// Kill press-fade + hover-fade + checked-fade transitions on ONE widget
// across the standard part/state combos the LVGL 9 default theme touches.
// Selectors that don't apply to a given widget are silently ignored.
static void killWidgetAnims(lv_obj_t* obj) {
    if (!obj) return;
    ensureZeroTx();
    static const lv_style_selector_t combos[] = {
        LV_PART_MAIN      | LV_STATE_DEFAULT,
        LV_PART_MAIN      | LV_STATE_PRESSED,
        LV_PART_MAIN      | LV_STATE_CHECKED,
        LV_PART_MAIN      | LV_STATE_FOCUSED,
        LV_PART_INDICATOR | LV_STATE_DEFAULT,
        LV_PART_INDICATOR | LV_STATE_CHECKED,
        LV_PART_INDICATOR | LV_STATE_PRESSED,
        LV_PART_KNOB      | LV_STATE_DEFAULT,
        LV_PART_KNOB      | LV_STATE_CHECKED,
        LV_PART_KNOB      | LV_STATE_PRESSED,
    };
    for (size_t i = 0; i < sizeof(combos)/sizeof(combos[0]); i++) {
        lv_obj_set_style_transition   (obj, &s_zeroTx, combos[i]);
        lv_obj_set_style_anim_duration(obj, 0,         combos[i]);
    }
}

// Repaint the manual tab strip to reflect s_activeTab. Force a full
// screen invalidate at the end — user reported the underline "stuck on
// the wrong tab sometimes", which was individual underlines invalidating
// but the strip's parent button not redrawing over the old underline's
// pixel region on the frame we cared about. `lv_obj_invalidate` on the
// screen is $$$-cheap-per-second (< 1 ms) and belt-and-braces guarantees
// the underline pixels are always current with s_activeTab.
static void manualStripRepaint() {
    for (int i = 0; i < 3; i++) {
        bool on = (i == s_activeTab);
        if (s_manualLbls[i]) {
            lv_obj_set_style_text_color(s_manualLbls[i],
                lvhex(on ? C_TEXT : C_MUTED), 0);
        }
        if (s_manualUnder[i]) {
            if (on) lv_obj_clear_flag(s_manualUnder[i], LV_OBJ_FLAG_HIDDEN);
            else    lv_obj_add_flag  (s_manualUnder[i], LV_OBJ_FLAG_HIDDEN);
        }
    }
    lv_obj_invalidate(lv_scr_act());   // force full redraw
}

// Show one of the 3 manual tab containers, hide the other two, update
// s_activeTab, sync the tab-strip, and reset Settings paging when
// entering that tab. This is the ONLY way to change the active tab —
// callers from click handlers, gesture handlers, and boot init all go
// through here.
static void settingsShowPage(int p);   // forward
static void showTab(int idx) {
    if (s_bricked) return;    // brick failsafe: no tab-swap after wipe
    if (idx < 0 || idx > 2) return;
    lv_obj_t* tabs[3] = { s_tabHome, s_tabFiles, s_tabSet };
    for (int i = 0; i < 3; i++) {
        if (!tabs[i]) continue;
        if (i == idx) lv_obj_clear_flag(tabs[i], LV_OBJ_FLAG_HIDDEN);
        else          lv_obj_add_flag  (tabs[i], LV_OBJ_FLAG_HIDDEN);
    }
    s_activeTab = idx;
    if (idx == 2) settingsShowPage(0);   // always land on page 0 in Settings
    manualStripRepaint();
}

// ---- helpers ---------------------------------------------------------------
static void stopBtnCb(lv_event_t* e) {
    if (lv_event_get_code(e) != LV_EVENT_CLICKED) return;
    s_stopEdge = true;
    // If we were paused, unpause so the interpreter loop can see the
    // stopRequested flag the main .ino sets on its next tick.
    scriptPaused = false;
}

// R1-B: XPowersPMU is a typedef for XPowersAXP2101 under XPOWERS_CHIP_AXP2101.
// Its 4-arg begin() is the only one that compiles; the 2-arg overload does
// not exist. Wire is already up (TouchClass::on ran Wire.begin(IIC_SDA,
// IIC_SCL)) so passing the pins again is idempotent.
// R4-A: use a hasTried flag so the first call actually attempts init instead
// of short-circuiting because millis()-0 < 2000.
static void tryInitPMU() {
    if (s_pmuReady) return;
    static bool hasTried = false;
    if (hasTried && millis() - s_lastPMUProbe < 2000) return;
    hasTried = true;
    s_lastPMUProbe = millis();
    if (s_pmu.begin(Wire, AXP2101_SLAVE_ADDRESS, IIC_SDA, IIC_SCL)) {
        s_pmuReady = true;
        s_pmu.disableIRQ(XPOWERS_AXP2101_ALL_IRQ);
        s_pmu.clearIrqStatus();
        // Watch port: keep the power-key IRQs latched so we can poll the
        // status register in watchUiTick and drive a hold-to-power-off
        // countdown. We NEVER wire the AXP2101's INT pin to a GPIO here —
        // polling the latch is enough at 100 ms cadence.
        s_pmu.enableIRQ(XPOWERS_AXP2101_PKEY_NEGATIVE_IRQ |
                        XPOWERS_AXP2101_PKEY_POSITIVE_IRQ |
                        XPOWERS_AXP2101_PKEY_LONG_IRQ);
        s_pmu.enableBattDetection();
        s_pmu.enableBattVoltageMeasure();
        s_pmu.enableSystemVoltageMeasure();
        s_pmu.enableVbusVoltageMeasure();
        s_pmu.enableTemperatureMeasure();
    }
}

// One consistent row: [icon] [label ..... grow .....] [switch]. Used for
// every switch on the Settings tab so the tab reads as one thing.
static lv_obj_t* makeSwitchRow(lv_obj_t* parent, const char* icon,
                               const char* label, bool initial,
                               lv_event_cb_t cb, int idx) {
    lv_obj_t* row = lv_obj_create(parent);
    lv_obj_remove_style_all(row);
    // Curvy safe-area — see SAFE_X comment. Applies to every settings row
    // so the switch on the right doesn't clip past the rounded corner.
    lv_obj_set_size(row, LCD_W - 2 * SAFE_X, 60);
    lv_obj_set_scroll_dir(row, LV_DIR_NONE);
    lv_obj_set_style_pad_hor(row, 14, 0);
    lv_obj_set_style_pad_ver(row, 8, 0);
    lv_obj_set_style_border_side(row, LV_BORDER_SIDE_BOTTOM, 0);
    lv_obj_set_style_border_width(row, 1, 0);
    lv_obj_set_style_border_color(row, lvhex(C_LINE), 0);
    lv_obj_set_flex_flow(row, LV_FLEX_FLOW_ROW);
    lv_obj_set_flex_align(row, LV_FLEX_ALIGN_START,
                               LV_FLEX_ALIGN_CENTER,
                               LV_FLEX_ALIGN_CENTER);
    lv_obj_clear_flag(row, LV_OBJ_FLAG_SCROLLABLE);

    lv_obj_t* ic = lv_label_create(row);
    lv_label_set_text(ic, icon);
    lv_obj_set_style_text_color(ic, lvhex(C_MUTED), 0);
    lv_obj_set_style_text_font(ic, &lv_font_montserrat_20, 0);
    lv_obj_set_style_pad_right(ic, 14, 0);
    lv_obj_set_width(ic, 30);

    lv_obj_t* lbl = lv_label_create(row);
    lv_label_set_text(lbl, label);
    lv_obj_set_style_text_color(lbl, lvhex(C_TEXT), 0);
    lv_obj_set_style_text_font(lbl, &lv_font_montserrat_18, 0);
    lv_obj_set_flex_grow(lbl, 1);

    lv_obj_t* sw = lv_switch_create(row);
    lv_obj_set_style_bg_color(sw, lvhex(C_LINE), 0);
    lv_obj_set_style_bg_color(sw, lvhex(C_ACCENT), LV_PART_INDICATOR | LV_STATE_CHECKED);
    // Kill the ~300 ms knob slide + the theme's press/check FADE transition.
    killWidgetAnims(sw);
    if (initial) lv_obj_add_state(sw, LV_STATE_CHECKED);
    lv_obj_add_event_cb(sw, cb, LV_EVENT_VALUE_CHANGED, (void*)(intptr_t)idx);
    // Strip the row's press-highlight so tapping anywhere else on the row
    // doesn't queue a full-row redraw for the darken pass.
    lv_obj_set_style_bg_opa(row, LV_OPA_TRANSP, LV_STATE_PRESSED);
    if (idx >= 0 && idx < SET_COUNT) s_settingSw[idx] = sw;
    return row;
}

// Same row shell but the trailing element is a button — used for Reboot /
// Factory Reset. Danger flag turns the button red.
static lv_obj_t* makeActionRow(lv_obj_t* parent, const char* icon,
                               const char* label, const char* btnText,
                               bool danger, lv_event_cb_t cb) {
    lv_obj_t* row = lv_obj_create(parent);
    lv_obj_remove_style_all(row);
    // Curvy safe-area for Reboot/Reset/Wipe/Brick action rows too.
    lv_obj_set_size(row, LCD_W - 2 * SAFE_X, 70);
    lv_obj_set_scroll_dir(row, LV_DIR_NONE);
    lv_obj_set_style_pad_hor(row, 14, 0);
    lv_obj_set_style_pad_ver(row, 10, 0);
    lv_obj_set_style_border_side(row, LV_BORDER_SIDE_BOTTOM, 0);
    lv_obj_set_style_border_width(row, 1, 0);
    lv_obj_set_style_border_color(row, lvhex(C_LINE), 0);
    lv_obj_set_flex_flow(row, LV_FLEX_FLOW_ROW);
    lv_obj_set_flex_align(row, LV_FLEX_ALIGN_START,
                               LV_FLEX_ALIGN_CENTER,
                               LV_FLEX_ALIGN_CENTER);
    lv_obj_clear_flag(row, LV_OBJ_FLAG_SCROLLABLE);

    lv_obj_t* ic = lv_label_create(row);
    lv_label_set_text(ic, icon);
    lv_obj_set_style_text_color(ic, lvhex(danger ? C_DANGER : C_MUTED), 0);
    lv_obj_set_style_text_font(ic, &lv_font_montserrat_20, 0);
    lv_obj_set_style_pad_right(ic, 14, 0);
    lv_obj_set_width(ic, 30);

    lv_obj_t* lbl = lv_label_create(row);
    lv_label_set_text(lbl, label);
    lv_obj_set_style_text_color(lbl, lvhex(C_TEXT), 0);
    lv_obj_set_style_text_font(lbl, &lv_font_montserrat_18, 0);
    lv_obj_set_flex_grow(lbl, 1);

    lv_obj_t* btn = lv_btn_create(row);
    lv_obj_set_size(btn, 130, 46);
    lv_obj_set_style_bg_color(btn, lvhex(danger ? C_DANGER : C_ACCENT), 0);
    lv_obj_set_style_radius(btn, 8, 0);
    lv_obj_set_style_shadow_width(btn, 0, 0);
    killWidgetAnims(btn);
    lv_obj_add_event_cb(btn, cb, LV_EVENT_CLICKED, nullptr);
    lv_obj_t* bt = lv_label_create(btn);
    lv_label_set_text(bt, btnText);
    lv_obj_set_style_text_color(bt, lv_color_white(), 0);
    lv_obj_set_style_text_font(bt, &lv_font_montserrat_18, 0);
    lv_obj_center(bt);
    return row;
}

// ---- settings switch event handler ----------------------------------------
// Runs on the LVGL task. We NEVER touch WiFi/BT/Preferences from here — that
// would race with the main firmware task. Just record the intent; loop()
// picks it up via watchUiConsumePendingSettings().
static void settingSwitchCb(lv_event_t* e) {
    lv_obj_t* sw   = (lv_obj_t*)lv_event_get_target(e);
    int idx        = (int)(intptr_t)lv_event_get_user_data(e);
    bool on        = lv_obj_has_state(sw, LV_STATE_CHECKED);
    switch (idx) {
        case SET_WIFI:      s_pending.has_wifi    = true; s_pending.wifi_on    = on; break;
        case SET_BT:        s_pending.has_bt      = true; s_pending.bt_on      = on; break;
        case SET_BTDISC:    s_pending.has_btdisc  = true; s_pending.btdisc_on  = on; break;
        case SET_LED:       s_pending.has_led     = true; s_pending.led_on     = on; break;
        case SET_SILENT:    s_pending.has_silent  = true; s_pending.silent_on  = on; break;
        case SET_LOGGING:   s_pending.has_logging = true; s_pending.logging_on = on; break;
        case SET_COM:       s_pending.has_com     = true; s_pending.com_on     = on; break;
        case SET_SCREEN_SLEEP:
                            s_pending.has_screen_sleep = true;
                            s_pending.screen_sleep_on  = on;
                            s_screenSleepOn = on;
                            break;
        case SET_AUTOATTACH:
                            // Mutually exclusive with autorun-at-boot.
                            if (on && s_settingSw[SET_AUTOSTART] &&
                                lv_obj_has_state(s_settingSw[SET_AUTOSTART], LV_STATE_CHECKED)) {
                                lv_obj_clear_state(s_settingSw[SET_AUTOSTART], LV_STATE_CHECKED);
                                s_pendingExtras.has_autostart = true;
                                s_pendingExtras.autostart_on  = false;
                            }
                            s_pendingExtras.has_autoattach = true;
                            s_pendingExtras.autoattach_on  = on;
                            break;
        case SET_AUTOSTART: // Fires on ESP BOOT; opposite of SET_AUTOATTACH.
                            if (on && s_settingSw[SET_AUTOATTACH] &&
                                lv_obj_has_state(s_settingSw[SET_AUTOATTACH], LV_STATE_CHECKED)) {
                                lv_obj_clear_state(s_settingSw[SET_AUTOATTACH], LV_STATE_CHECKED);
                                s_pendingExtras.has_autoattach = true;
                                s_pendingExtras.autoattach_on  = false;
                            }
                            s_pendingExtras.has_autostart = true;
                            s_pendingExtras.autostart_on  = on; break;
        case SET_DEADNET:   s_pendingExtras.has_deadnet   = true;
                            s_pendingExtras.deadnet_on    = on; break;
    }
}
static void rebootBtnCb(lv_event_t*)         { s_pending.want_reboot        = true; }
static void factoryResetBtnCb(lv_event_t*)   { s_pending.want_factory_reset = true; }
static void resetStdBtnCb(lv_event_t*)       { s_pendingExtras.want_reset_std = true; }
static void brickBtnCb(lv_event_t*)          { s_pendingExtras.want_brick     = true; }

// ---- tab builders ---------------------------------------------------------
static void buildHomeTab(lv_obj_t* tab) {
    lv_obj_set_style_bg_color(tab, lvhex(C_BG), 0);
    lv_obj_set_style_pad_all(tab, 12, 0);
    lv_obj_clear_flag(tab, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_set_scrollbar_mode(tab, LV_SCROLLBAR_MODE_OFF);

    // Status + battery. The animated LED "circle that turns colors while a
    // script runs" is gone — status label carries the same information as
    // text, without a repainting circle on every LVGL tick.
    s_statusLbl = lv_label_create(tab);
    lv_label_set_text(s_statusLbl, "Booting...");
    lv_obj_set_style_text_color(s_statusLbl, lvhex(C_TEXT), 0);
    lv_obj_set_style_text_font(s_statusLbl, &lv_font_montserrat_22, 0);
    lv_obj_align(s_statusLbl, LV_ALIGN_TOP_LEFT, 0, 6);

    s_battLbl = lv_label_create(tab);
    lv_label_set_text(s_battLbl, LV_SYMBOL_BATTERY_FULL " -- %");
    lv_obj_set_style_text_color(s_battLbl, lvhex(C_MUTED), 0);
    lv_obj_set_style_text_font(s_battLbl, &lv_font_montserrat_16, 0);
    // Fixed width + right-aligned text + no transform scale — user saw
    // stretching on "80% 4.02V" style updates. Same fix as the clock: pin
    // width and align inside, don't let LV_SIZE_CONTENT redo layout per
    // string change.
    lv_obj_set_width(s_battLbl, 160);
    lv_obj_set_style_text_align(s_battLbl, LV_TEXT_ALIGN_RIGHT, 0);
    lv_label_set_long_mode(s_battLbl, LV_LABEL_LONG_CLIP);
    lv_obj_align(s_battLbl, LV_ALIGN_TOP_RIGHT, 0, 8);

    // Script + progress
    s_scriptLbl = lv_label_create(tab);
    lv_label_set_text(s_scriptLbl, LV_SYMBOL_FILE "  no script");
    lv_obj_set_style_text_color(s_scriptLbl, lvhex(C_OK), 0);
    lv_obj_set_style_text_font(s_scriptLbl, &lv_font_montserrat_18, 0);
    lv_obj_set_width(s_scriptLbl, LCD_W - 24);
    lv_label_set_long_mode(s_scriptLbl, LV_LABEL_LONG_DOT);
    lv_obj_align(s_scriptLbl, LV_ALIGN_TOP_LEFT, 0, 82);

    s_progress = lv_bar_create(tab);
    lv_obj_set_size(s_progress, LCD_W - 24, 8);
    lv_obj_align(s_progress, LV_ALIGN_TOP_LEFT, 0, 108);
    lv_bar_set_range(s_progress, 0, 100);
    lv_bar_set_value(s_progress, 0, LV_ANIM_OFF);
    lv_obj_set_style_bg_color(s_progress, lvhex(C_LINE), 0);
    lv_obj_set_style_bg_color(s_progress, lvhex(C_ACCENT), LV_PART_INDICATOR);
    lv_obj_set_style_radius(s_progress, 4, 0);
    lv_obj_set_style_radius(s_progress, 4, LV_PART_INDICATOR);
    lv_obj_add_flag(s_progress, LV_OBJ_FLAG_HIDDEN);

    // AP block — a subtle card so the creds read as a group
    lv_obj_t* apCard = lv_obj_create(tab);
    lv_obj_remove_style_all(apCard);
    lv_obj_set_size(apCard, LCD_W - 24, 118);
    lv_obj_align(apCard, LV_ALIGN_TOP_LEFT, 0, 134);
    lv_obj_set_style_bg_color(apCard, lvhex(C_SURFACE), 0);
    lv_obj_set_style_bg_opa(apCard, LV_OPA_COVER, 0);
    lv_obj_set_style_radius(apCard, 12, 0);
    lv_obj_set_style_pad_all(apCard, 12, 0);
    lv_obj_clear_flag(apCard, LV_OBJ_FLAG_SCROLLABLE);

    s_apSsidLbl = lv_label_create(apCard);
    lv_label_set_text(s_apSsidLbl, LV_SYMBOL_WIFI "  AP: (starting)");
    lv_obj_set_style_text_color(s_apSsidLbl, lvhex(C_TEXT), 0);
    lv_obj_set_style_text_font(s_apSsidLbl, &lv_font_montserrat_16, 0);
    lv_obj_align(s_apSsidLbl, LV_ALIGN_TOP_LEFT, 0, 0);

    s_apPassLbl = lv_label_create(apCard);
    lv_label_set_text(s_apPassLbl, "PSK: ...");
    lv_obj_set_style_text_color(s_apPassLbl, lvhex(C_MUTED), 0);
    lv_obj_set_style_text_font(s_apPassLbl, &lv_font_montserrat_16, 0);
    lv_obj_align(s_apPassLbl, LV_ALIGN_TOP_LEFT, 0, 24);

    s_ipLbl = lv_label_create(apCard);
    lv_label_set_text(s_ipLbl, "http://192.168.4.1");
    lv_obj_set_style_text_color(s_ipLbl, lvhex(C_ACCENT), 0);
    lv_obj_set_style_text_font(s_ipLbl, &lv_font_montserrat_18, 0);
    lv_obj_align(s_ipLbl, LV_ALIGN_TOP_LEFT, 0, 50);

    s_clientsLbl = lv_label_create(apCard);
    lv_label_set_text(s_clientsLbl, LV_SYMBOL_USB "  0 clients");
    lv_obj_set_style_text_color(s_clientsLbl, lvhex(C_MUTED), 0);
    lv_obj_set_style_text_font(s_clientsLbl, &lv_font_montserrat_16, 0);
    lv_obj_align(s_clientsLbl, LV_ALIGN_TOP_LEFT, 0, 80);

    // Banner moved out of buildHomeTab — it now lives on lv_layer_top()
    // (created in watchUiBegin) so the power-off countdown / SD hot-plug
    // toast floats above Home, Files, Settings and the clock overlay.

    // Toast overlay — moved out of buildHomeTab per bug-hunt finding #7.
    // Previously parented to the home tab, so a watchUiFlash("Deleted foo")
    // fired from the Files tab was invisible until the wearer switched
    // tabs. Now created on lv_layer_top() in watchUiBegin() alongside the
    // banner so every tab sees the toast.

    // "Selected:" label sits just above the Play/Stop row. Empty when
    // nothing is queued — filled by watchUiSetSelectedScript() (called
    // when the user taps a script row in Files).
    s_selectedLbl = lv_label_create(tab);
    lv_label_set_text(s_selectedLbl, "");
    lv_obj_set_style_text_color(s_selectedLbl, lvhex(C_MUTED), 0);
    lv_obj_set_style_text_font(s_selectedLbl, &lv_font_montserrat_18, 0);
    lv_obj_set_width(s_selectedLbl, LCD_W - 2 * SAFE_X);
    lv_label_set_long_mode(s_selectedLbl, LV_LABEL_LONG_DOT);
    lv_obj_set_style_text_align(s_selectedLbl, LV_TEXT_ALIGN_CENTER, 0);
    lv_obj_align(s_selectedLbl, LV_ALIGN_BOTTOM_MID, 0, -100);

    // Play (green, left) + Stop (red, right). Small compact pair — the
    // full-width Stop was overkill and dominated the tab. Play arms/runs
    // the SELECTED script (or the last-run one); Stop stops a running
    // script. Both stay inside the AMOLED's rounded safe area.
    const int rowW  = LCD_W - 2 * SAFE_X;
    const int btnH  = 74;
    const int btnW  = 100;
    const int gap   = 12;
    // Two-button pair centered inside the safe area.
    const int pairW = 2 * btnW + gap;
    const int pairX = SAFE_X + (rowW - pairW) / 2;
    const int btnY  = -10;   // negative Y = align to bottom

    s_playBtn = lv_btn_create(tab);
    lv_obj_set_size(s_playBtn, btnW, btnH);
    lv_obj_align(s_playBtn, LV_ALIGN_BOTTOM_LEFT, pairX, btnY);
    lv_obj_set_style_bg_color(s_playBtn, lvhex(C_OK), 0);
    lv_obj_set_style_bg_color(s_playBtn, lvhex(C_OK), LV_STATE_PRESSED);
    lv_obj_set_style_radius(s_playBtn, 12, 0);
    lv_obj_set_style_shadow_width(s_playBtn, 0, 0);
    killWidgetAnims(s_playBtn);
    lv_obj_add_event_cb(s_playBtn, [](lv_event_t* e){
        if (lv_event_get_code(e) != LV_EVENT_CLICKED) return;
        // Tri-state: idle → run, running → pause, paused → continue.
        // Idle path queues to the main loop; pause/resume flip the
        // scriptPaused global directly (main loop sees it every 50 ms).
        if (s_scriptState == 0) {
            s_pending.want_play = true;   // launches selectedScriptName
        } else if (s_scriptState == 1) {
            scriptPaused = true;
            watchUiSetScriptState(2);
            watchUiFlash("Paused");
        } else if (s_scriptState == 2) {
            scriptPaused = false;
            watchUiSetScriptState(1);
            watchUiFlash("Continuing");
        }
    }, LV_EVENT_CLICKED, nullptr);
    s_playLbl = lv_label_create(s_playBtn);
    lv_label_set_text(s_playLbl, LV_SYMBOL_PLAY);
    lv_obj_set_style_text_color(s_playLbl, lv_color_white(), 0);
    lv_obj_set_style_text_font(s_playLbl, &lv_font_montserrat_32, 0);
    lv_obj_center(s_playLbl);

    s_stopBtn = lv_btn_create(tab);
    lv_obj_set_size(s_stopBtn, btnW, btnH);
    lv_obj_align(s_stopBtn, LV_ALIGN_BOTTOM_LEFT, pairX + btnW + gap, btnY);
    lv_obj_set_style_bg_color(s_stopBtn, lvhex(C_DANGER), 0);
    lv_obj_set_style_bg_color(s_stopBtn, lvhex(C_DANGER), LV_STATE_PRESSED);
    lv_obj_set_style_radius(s_stopBtn, 12, 0);
    lv_obj_set_style_shadow_width(s_stopBtn, 0, 0);
    killWidgetAnims(s_stopBtn);
    lv_obj_add_event_cb(s_stopBtn, stopBtnCb, LV_EVENT_CLICKED, nullptr);
    lv_obj_t* stopLbl = lv_label_create(s_stopBtn);
    lv_label_set_text(stopLbl, LV_SYMBOL_STOP);
    lv_obj_set_style_text_color(stopLbl, lv_color_white(), 0);
    lv_obj_set_style_text_font(stopLbl, &lv_font_montserrat_32, 0);
    lv_obj_center(stopLbl);

    // Dim the Stop button by default (no script running). Actual state
    // updates come via watchUiSetScriptState() from the main loop.
    lv_obj_set_style_bg_opa(s_stopBtn, LV_OPA_40, 0);
}

// ---- Files tab -------------------------------------------------------------
// Rows are [ name (grows) | ▶ green | 🗑 red ]. Callbacks record the intent
// on s_pendingAction; the main loop consumes it and runs the actual SD /
// interpreter work off the LVGL task.
static void filesRunCb(lv_event_t* e) {
    const char* name = (const char*)lv_event_get_user_data(e);
    if (!name) return;
    s_pendingAction.has_run = true;
    strncpy(s_pendingAction.run_name, name, sizeof(s_pendingAction.run_name) - 1);
    s_pendingAction.run_name[sizeof(s_pendingAction.run_name) - 1] = '\0';
    // Also register as the selected script so the persistent RUNNING
    // banner + Home Play button both know the name.
    watchUiSetSelectedScript(name);
}
static void filesDelCb(lv_event_t* e) {
    const char* name = (const char*)lv_event_get_user_data(e);
    if (!name) return;
    s_pendingAction.has_delete = true;
    strncpy(s_pendingAction.delete_name, name, sizeof(s_pendingAction.delete_name) - 1);
    s_pendingAction.delete_name[sizeof(s_pendingAction.delete_name) - 1] = '\0';
}
static void filesStarCb(lv_event_t* e) {
    const char* name = (const char*)lv_event_get_user_data(e);
    if (!name) return;
    s_pendingAction.has_toggle_autostart = true;
    // Pressing the star on the currently-armed script clears autostart;
    // pressing on any other row makes THAT the new autostart target.
    if (s_currentAutostart == String(name)) {
        s_pendingAction.autostart_name[0] = '\0';
    } else {
        strncpy(s_pendingAction.autostart_name, name,
                sizeof(s_pendingAction.autostart_name) - 1);
        s_pendingAction.autostart_name[sizeof(s_pendingAction.autostart_name) - 1] = '\0';
    }
}

static void buildFilesTab(lv_obj_t* tab) {
    lv_obj_set_style_bg_color(tab, lvhex(C_BG), 0);
    lv_obj_set_style_pad_all(tab, 0, 0);
    lv_obj_clear_flag(tab, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_set_scrollbar_mode(tab, LV_SCROLLBAR_MODE_OFF);

    s_filesList = lv_obj_create(tab);
    lv_obj_remove_style_all(s_filesList);
    lv_obj_set_size(s_filesList, LCD_W, LV_PCT(100));
    // Task #4: scroll polish. LVGL 9 defaults to elastic snap + a scroll-
    // end animation that on this QSPI AMOLED reads as lag. Kill both.
    lv_obj_set_scroll_snap_x(s_filesList, LV_SCROLL_SNAP_NONE);
    lv_obj_set_scroll_snap_y(s_filesList, LV_SCROLL_SNAP_NONE);
    // Bug-hunt R5: anim_duration=0 in LV_STATE_SCROLLED does NOT reliably
    // kill LVGL's scroll-end throw — that anim reads the style with
    // selector=LV_PART_MAIN and the state at animation start (still
    // DEFAULT for the first frame after release). Use plain selector 0 so
    // every state resolves to 0-duration. Also clear the SCROLL_ELASTIC
    // and SCROLL_MOMENTUM flags — with them still set (LVGL default) each
    // finger flick spawned a ~300 ms overscroll bounce that reads as lag.
    lv_obj_set_style_anim_duration(s_filesList, 0, 0);
    lv_obj_clear_flag(s_filesList, LV_OBJ_FLAG_SCROLL_ELASTIC);
    lv_obj_clear_flag(s_filesList, LV_OBJ_FLAG_SCROLL_MOMENTUM);
    // Curvy safe-area: leave a horizontal gutter so each row (also inset
    // now) sits inside the visible circle.
    lv_obj_set_style_pad_left(s_filesList, SAFE_X, 0);
    lv_obj_set_style_pad_right(s_filesList, SAFE_X, 0);
    lv_obj_set_style_pad_ver(s_filesList, 0, 0);
    lv_obj_set_style_bg_color(s_filesList, lvhex(C_BG), 0);
    lv_obj_set_flex_flow(s_filesList, LV_FLEX_FLOW_COLUMN);
    lv_obj_set_flex_align(s_filesList, LV_FLEX_ALIGN_START,
                                       LV_FLEX_ALIGN_START,
                                       LV_FLEX_ALIGN_START);
    lv_obj_set_scroll_dir(s_filesList, LV_DIR_VER);
    lv_obj_set_style_pad_gap(s_filesList, 0, 0);
    lv_obj_set_scrollbar_mode(s_filesList, LV_SCROLLBAR_MODE_OFF);

    s_filesEmpty = lv_label_create(tab);
    lv_label_set_text(s_filesEmpty,
        LV_SYMBOL_FILE "  No scripts yet\n"
        "Drop .txt files in /scripts on the SD card.");
    lv_obj_set_style_text_color(s_filesEmpty, lvhex(C_MUTED), 0);
    lv_obj_set_style_text_font(s_filesEmpty, &lv_font_montserrat_18, 0);
    lv_obj_set_style_text_align(s_filesEmpty, LV_TEXT_ALIGN_CENTER, 0);
    lv_obj_set_width(s_filesEmpty, LCD_W - 40);
    lv_label_set_long_mode(s_filesEmpty, LV_LABEL_LONG_WRAP);
    lv_obj_align(s_filesEmpty, LV_ALIGN_CENTER, 0, 0);
}

// Store row-name strings so callbacks can point at stable memory across
// rebuilds. LVGL user_data is a raw pointer, so we can't hand it a String
// stack copy — this vector outlives each row until the next rebuild frees it.
static std::vector<std::string> s_fileRowNames;

// ---- settings tab ---------------------------------------------------------
// Paged Settings — 4 items per page, big font + big switches, gesture
// switch (swipe UP = next page, swipe DOWN = previous). No scroll, no
// anim; each page swap is a hide/show of a full-screen page container.
// Three pages: Radios, Behaviour, Actions.
#define SETTINGS_PAGE_COUNT 4
static lv_obj_t* s_settingsPages[SETTINGS_PAGE_COUNT] = { nullptr };
static lv_obj_t* s_settingsDots [SETTINGS_PAGE_COUNT] = { nullptr };
static int       s_settingsPage = 0;
static lv_obj_t* s_settingsPageIndicator = nullptr;

static void settingsShowPage(int p) {
    if (p < 0) p = 0;
    if (p >= SETTINGS_PAGE_COUNT) p = SETTINGS_PAGE_COUNT - 1;
    s_settingsPage = p;
    for (int i = 0; i < SETTINGS_PAGE_COUNT; i++) {
        if (!s_settingsPages[i]) continue;
        if (i == p) lv_obj_clear_flag(s_settingsPages[i], LV_OBJ_FLAG_HIDDEN);
        else        lv_obj_add_flag  (s_settingsPages[i], LV_OBJ_FLAG_HIDDEN);
    }
    for (int i = 0; i < SETTINGS_PAGE_COUNT; i++) {
        if (!s_settingsDots[i]) continue;
        // Set for EVERY state selector combo we know the theme might tint.
        // User reported a lingering blue tint on dot 0 no matter which
        // page was active — traced to LVGL applying a checked-state
        // color from the theme when set_active was called on a flex-row
        // first child during focus init. Belt-and-braces override every
        // state at the same time and force redraw.
        uint32_t col = (i == p) ? C_ACCENT : C_LINE;
        lv_obj_set_style_bg_color(s_settingsDots[i], lvhex(col), 0);
        lv_obj_set_style_bg_color(s_settingsDots[i], lvhex(col), LV_STATE_PRESSED);
        lv_obj_set_style_bg_color(s_settingsDots[i], lvhex(col), LV_STATE_CHECKED);
        lv_obj_set_style_bg_color(s_settingsDots[i], lvhex(col), LV_STATE_FOCUSED);
        lv_obj_invalidate(s_settingsDots[i]);
    }
}

// NOTE: Bug-hunt R3 — the previous settingsGestureCb() installed on the
// Settings tab itself has been REMOVED. An identical handler is registered
// on the tabview in watchUiBegin() (line ~837) with a proper active-tab
// check + lv_event_stop_processing(). Keeping both meant LVGL's bubble
// dispatched the same gesture twice — swipe LEFT once and the page
// advanced by two, skipping the middle page entirely. Only the tabview
// handler remains now.

// Big variant of the switch row for the paged Settings — Montserrat 22
// label, 60x36 switch, generous padding. Reuses the pending-settings
// callback + switch handle registration in s_settingSw[].
static lv_obj_t* makeBigSwitchRow(lv_obj_t* parent, const char* icon,
                                  const char* label, bool initial,
                                  lv_event_cb_t cb, int idx) {
    lv_obj_t* row = lv_obj_create(parent);
    lv_obj_remove_style_all(row);
    lv_obj_set_size(row, LCD_W - 2 * SAFE_X, 80);
    lv_obj_set_scroll_dir(row, LV_DIR_NONE);
    lv_obj_set_style_pad_hor(row, 16, 0);
    lv_obj_set_style_pad_ver(row, 10, 0);
    lv_obj_set_style_border_side(row, LV_BORDER_SIDE_BOTTOM, 0);
    lv_obj_set_style_border_width(row, 1, 0);
    lv_obj_set_style_border_color(row, lvhex(C_LINE), 0);
    lv_obj_set_flex_flow(row, LV_FLEX_FLOW_ROW);
    lv_obj_set_flex_align(row, LV_FLEX_ALIGN_START,
                               LV_FLEX_ALIGN_CENTER,
                               LV_FLEX_ALIGN_CENTER);
    lv_obj_clear_flag(row, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_set_style_bg_opa(row, LV_OPA_TRANSP, LV_STATE_PRESSED);

    lv_obj_t* ic = lv_label_create(row);
    lv_label_set_text(ic, icon);
    lv_obj_set_style_text_color(ic, lvhex(C_MUTED), 0);
    lv_obj_set_style_text_font(ic, &lv_font_montserrat_24, 0);
    lv_obj_set_style_pad_right(ic, 14, 0);
    lv_obj_set_width(ic, 36);

    lv_obj_t* lbl = lv_label_create(row);
    lv_label_set_text(lbl, label);
    lv_obj_set_style_text_color(lbl, lvhex(C_TEXT), 0);
    lv_obj_set_style_text_font(lbl, &lv_font_montserrat_22, 0);
    lv_obj_set_style_text_letter_space(lbl, 0, 0);
    lv_obj_set_style_transform_scale_x(lbl, 256, 0);
    lv_obj_set_style_transform_scale_y(lbl, 256, 0);
    lv_obj_set_flex_grow(lbl, 1);

    lv_obj_t* sw = lv_switch_create(row);
    lv_obj_set_size(sw, 72, 42);
    lv_obj_set_style_bg_color(sw, lvhex(C_LINE), 0);
    lv_obj_set_style_bg_color(sw, lvhex(C_ACCENT), LV_PART_INDICATOR | LV_STATE_CHECKED);
    killWidgetAnims(sw);
    if (initial) lv_obj_add_state(sw, LV_STATE_CHECKED);
    lv_obj_add_event_cb(sw, cb, LV_EVENT_VALUE_CHANGED, (void*)(intptr_t)idx);
    if (idx >= 0 && idx < SET_COUNT) s_settingSw[idx] = sw;
    return row;
}

static lv_obj_t* makeBigActionRow(lv_obj_t* parent, const char* icon,
                                  const char* label, const char* btnText,
                                  bool danger, lv_event_cb_t cb) {
    lv_obj_t* row = lv_obj_create(parent);
    lv_obj_remove_style_all(row);
    lv_obj_set_size(row, LCD_W - 2 * SAFE_X, 88);
    lv_obj_set_scroll_dir(row, LV_DIR_NONE);
    lv_obj_set_style_pad_hor(row, 16, 0);
    lv_obj_set_style_pad_ver(row, 12, 0);
    lv_obj_set_style_border_side(row, LV_BORDER_SIDE_BOTTOM, 0);
    lv_obj_set_style_border_width(row, 1, 0);
    lv_obj_set_style_border_color(row, lvhex(C_LINE), 0);
    lv_obj_set_flex_flow(row, LV_FLEX_FLOW_ROW);
    lv_obj_set_flex_align(row, LV_FLEX_ALIGN_START,
                               LV_FLEX_ALIGN_CENTER,
                               LV_FLEX_ALIGN_CENTER);
    lv_obj_clear_flag(row, LV_OBJ_FLAG_SCROLLABLE);

    lv_obj_t* ic = lv_label_create(row);
    lv_label_set_text(ic, icon);
    lv_obj_set_style_text_color(ic, lvhex(danger ? C_DANGER : C_MUTED), 0);
    lv_obj_set_style_text_font(ic, &lv_font_montserrat_24, 0);
    lv_obj_set_style_pad_right(ic, 14, 0);
    lv_obj_set_width(ic, 36);

    lv_obj_t* lbl = lv_label_create(row);
    lv_label_set_text(lbl, label);
    lv_obj_set_style_text_color(lbl, lvhex(C_TEXT), 0);
    lv_obj_set_style_text_font(lbl, &lv_font_montserrat_22, 0);
    lv_obj_set_style_text_letter_space(lbl, 0, 0);
    lv_obj_set_style_transform_scale_x(lbl, 256, 0);
    lv_obj_set_style_transform_scale_y(lbl, 256, 0);
    lv_obj_set_flex_grow(lbl, 1);

    lv_obj_t* btn = lv_btn_create(row);
    lv_obj_set_size(btn, 150, 54);
    lv_obj_set_style_bg_color(btn, lvhex(danger ? C_DANGER : C_ACCENT), 0);
    lv_obj_set_style_radius(btn, 10, 0);
    lv_obj_set_style_shadow_width(btn, 0, 0);
    killWidgetAnims(btn);
    lv_obj_add_event_cb(btn, cb, LV_EVENT_CLICKED, nullptr);
    lv_obj_t* bt = lv_label_create(btn);
    lv_label_set_text(bt, btnText);
    lv_obj_set_style_text_color(bt, lv_color_white(), 0);
    lv_obj_set_style_text_font(bt, &lv_font_montserrat_20, 0);
    lv_obj_center(bt);
    return row;
}

// A row with a numeric value between minus/plus buttons — used for
// brightness. Value stored in s_brightnessPct; +/- adjust in `step` and
// queue via s_pending.has_brightness so the main loop applies to the
// display driver + persists to NVS.
static lv_obj_t* makeBrightnessRow(lv_obj_t* parent, const char* icon,
                                   const char* label, int step) {
    lv_obj_t* row = lv_obj_create(parent);
    lv_obj_remove_style_all(row);
    lv_obj_set_size(row, LCD_W - 2 * SAFE_X, 80);
    lv_obj_set_scroll_dir(row, LV_DIR_NONE);
    lv_obj_set_style_pad_hor(row, 16, 0);
    lv_obj_set_style_pad_ver(row, 10, 0);
    lv_obj_set_style_border_side(row, LV_BORDER_SIDE_BOTTOM, 0);
    lv_obj_set_style_border_width(row, 1, 0);
    lv_obj_set_style_border_color(row, lvhex(C_LINE), 0);
    lv_obj_set_flex_flow(row, LV_FLEX_FLOW_ROW);
    lv_obj_set_flex_align(row, LV_FLEX_ALIGN_START,
                               LV_FLEX_ALIGN_CENTER,
                               LV_FLEX_ALIGN_CENTER);
    lv_obj_clear_flag(row, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_set_style_bg_opa(row, LV_OPA_TRANSP, LV_STATE_PRESSED);

    lv_obj_t* ic = lv_label_create(row);
    lv_label_set_text(ic, icon);
    lv_obj_set_style_text_color(ic, lvhex(C_MUTED), 0);
    lv_obj_set_style_text_font(ic, &lv_font_montserrat_24, 0);
    lv_obj_set_style_pad_right(ic, 14, 0);
    lv_obj_set_width(ic, 36);

    lv_obj_t* lbl = lv_label_create(row);
    lv_label_set_text(lbl, label);
    lv_obj_set_style_text_color(lbl, lvhex(C_TEXT), 0);
    lv_obj_set_style_text_font(lbl, &lv_font_montserrat_22, 0);
    lv_obj_set_flex_grow(lbl, 1);

    auto makeStep = [&](const char* txt, int delta) {
        lv_obj_t* b = lv_btn_create(row);
        lv_obj_set_size(b, 52, 52);
        lv_obj_set_style_bg_color(b, lvhex(C_ACCENT), 0);
        lv_obj_set_style_bg_color(b, lvhex(C_ACCENT), LV_STATE_PRESSED);
        lv_obj_set_style_radius(b, 8, 0);
        lv_obj_set_style_shadow_width(b, 0, 0);
        killWidgetAnims(b);
        lv_obj_set_style_margin_left(b, 6, 0);
        lv_obj_add_event_cb(b, [](lv_event_t* e){
            int d = (int)(intptr_t)lv_event_get_user_data(e);
            int next = s_brightnessPct + d;
            if (next < 10)  next = 10;
            if (next > 100) next = 100;
            s_brightnessPct = next;
            if (s_brightVal) {
                char buf[8]; snprintf(buf, sizeof(buf), "%d%%", next);
                lv_label_set_text(s_brightVal, buf);
            }
            s_pending.has_brightness   = true;
            s_pending.brightness_pct   = next;
        }, LV_EVENT_CLICKED, (void*)(intptr_t)delta);
        lv_obj_t* t = lv_label_create(b);
        lv_label_set_text(t, txt);
        lv_obj_set_style_text_color(t, lv_color_white(), 0);
        lv_obj_set_style_text_font(t, &lv_font_montserrat_24, 0);
        lv_obj_center(t);
        return b;
    };
    makeStep("-", -step);
    s_brightVal = lv_label_create(row);
    char buf[8]; snprintf(buf, sizeof(buf), "%d%%", s_brightnessPct);
    lv_label_set_text(s_brightVal, buf);
    lv_obj_set_style_text_color(s_brightVal, lvhex(C_TEXT), 0);
    lv_obj_set_style_text_font(s_brightVal, &lv_font_montserrat_22, 0);
    lv_obj_set_width(s_brightVal, 72);
    lv_obj_set_style_text_align(s_brightVal, LV_TEXT_ALIGN_CENTER, 0);
    lv_obj_set_style_margin_left(s_brightVal, 6, 0);
    makeStep("+",  step);
    return row;
}

// Modal 4-digit PIN pad. Auto-submits when the 4th digit is entered
// (no OK button). Optional "Remove PIN/Duress" long button below the
// 0 clears the stored value. Overlay is parented to lv_scr_act so
// gestures on it reach the screen-level swipe handler (see comment on
// watchUiShowClock for the same reason).
typedef void (*PinPadDoneCb)(bool ok, const char* pin, bool wantRemove);
static lv_obj_t* s_pinPadRoot = nullptr;
static lv_obj_t* s_pinDigits  = nullptr;
static PinPadDoneCb s_pinDoneCb = nullptr;
static char s_pinBuf[8] = {0};

static void pinPadUpdate() {
    if (!s_pinDigits) return;
    char m[16] = {0};
    int n = strlen(s_pinBuf);
    // Spaces between digits give visible separation without the theme's
    // proportional-font kerning making the whole label feel stretched.
    // Same pattern as the clock label — fixed-width label + centered text.
    for (int i = 0; i < 4; i++) {
        m[i * 2]     = (i < n) ? '*' : '_';
        m[i * 2 + 1] = ' ';
    }
    lv_label_set_text(s_pinDigits, m);
}
static void pinPadClose() {
    if (!s_pinPadRoot) return;
    lv_obj_t* doomed = s_pinPadRoot;
    s_pinPadRoot = nullptr;
    s_pinDigits = nullptr;
    s_pinBuf[0] = '\0';
    lv_obj_del_async(doomed);
}
// Extended sig: pass a `removeLabel` — if non-empty, a full-width long
// button below the numeric grid says that ("Remove PIN" / "Remove
// duress code"). Tap → callback with wantRemove=true.
static void watchUiShowPinPad(const char* prompt,
                              const char* removeLabel,
                              PinPadDoneCb done) {
    if (s_bricked) return;
    if (s_pinPadRoot) return;
    s_pinDoneCb = done;
    s_pinBuf[0] = '\0';
    s_pinPadRoot = lv_obj_create(lv_scr_act());
    lv_obj_remove_style_all(s_pinPadRoot);
    lv_obj_set_size(s_pinPadRoot, LCD_W, LCD_H);
    lv_obj_set_pos(s_pinPadRoot, 0, 0);
    lv_obj_set_style_bg_color(s_pinPadRoot, lvhex(C_BG), 0);
    lv_obj_set_style_bg_opa(s_pinPadRoot, LV_OPA_COVER, 0);
    lv_obj_clear_flag(s_pinPadRoot, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_move_foreground(s_pinPadRoot);

    lv_obj_t* title = lv_label_create(s_pinPadRoot);
    lv_label_set_text(title, prompt);
    lv_obj_set_style_text_color(title, lvhex(C_TEXT), 0);
    lv_obj_set_style_text_font(title, &lv_font_montserrat_20, 0);
    lv_obj_set_style_text_letter_space(title, 0, 0);
    lv_obj_set_style_transform_scale_x(title, 256, 0);
    lv_obj_set_style_transform_scale_y(title, 256, 0);
    lv_obj_align(title, LV_ALIGN_TOP_MID, 0, 30);

    // Digit display: FIXED width + CENTER-align + pinned transform_scale
    // + monospaced letter-space. Same "italic/stretched" bug we hit on
    // the clock label — LV_SIZE_CONTENT + theme transform on state
    // changes gives the illusion the font is slanting.
    s_pinDigits = lv_label_create(s_pinPadRoot);
    lv_label_set_text(s_pinDigits, "_ _ _ _ ");
    lv_obj_set_style_text_color(s_pinDigits, lvhex(C_ACCENT), 0);
    lv_obj_set_style_text_font(s_pinDigits, &lv_font_montserrat_48, 0);
    lv_obj_set_style_text_letter_space(s_pinDigits, 0, 0);
    lv_obj_set_style_transform_scale_x(s_pinDigits, 256, 0);
    lv_obj_set_style_transform_scale_y(s_pinDigits, 256, 0);
    lv_obj_set_width(s_pinDigits, LCD_W - 80);
    lv_obj_set_style_text_align(s_pinDigits, LV_TEXT_ALIGN_CENTER, 0);
    lv_label_set_long_mode(s_pinDigits, LV_LABEL_LONG_CLIP);
    lv_obj_align(s_pinDigits, LV_ALIGN_TOP_MID, 0, 66);

    // 3×3 digit grid then a bottom row of [backspace] [0] [cancel]. The
    // OK button is gone — the 4th digit press auto-submits, matching
    // the "auto approve code, no yes checkmark" spec.
    const char* keys[12] = {
        "1","2","3",
        "4","5","6",
        "7","8","9",
        "<","0","X",
    };
    const int kw = 82, kh = 56, gap = 8;
    const int gridW = 3 * kw + 2 * gap;
    const int gridX = (LCD_W - gridW) / 2;
    const int gridY = 150;
    for (int i = 0; i < 12; i++) {
        int r = i / 3, c = i % 3;
        lv_obj_t* b = lv_btn_create(s_pinPadRoot);
        lv_obj_set_size(b, kw, kh);
        lv_obj_set_pos(b, gridX + c * (kw + gap), gridY + r * (kh + gap));
        bool isCancel = (strcmp(keys[i], "X") == 0);
        bool isBksp   = (strcmp(keys[i], "<") == 0);
        uint32_t bg = isCancel ? C_DANGER : (isBksp ? 0x2A2A30 : C_SURFACE);
        lv_obj_set_style_bg_color(b, lvhex(bg), 0);
        lv_obj_set_style_bg_color(b, lvhex(bg), LV_STATE_PRESSED);
        lv_obj_set_style_radius(b, 8, 0);
        lv_obj_set_style_shadow_width(b, 0, 0);
        killWidgetAnims(b);
        lv_obj_add_event_cb(b, [](lv_event_t* e){
            const char* k = (const char*)lv_event_get_user_data(e);
            if (!k) return;
            if (strcmp(k, "X") == 0) {
                PinPadDoneCb cb = s_pinDoneCb;
                s_pinDoneCb = nullptr;
                pinPadClose();
                if (cb) cb(false, "", false);
                return;
            }
            if (strcmp(k, "<") == 0) {
                int n = strlen(s_pinBuf);
                if (n > 0) { s_pinBuf[n-1] = '\0'; pinPadUpdate(); }
                return;
            }
            // digit — append + auto-submit at 4 digits.
            int n = strlen(s_pinBuf);
            if (n >= 4) return;
            s_pinBuf[n]   = k[0];
            s_pinBuf[n+1] = '\0';
            pinPadUpdate();
            if (strlen(s_pinBuf) == 4) {
                char pin[8]; strncpy(pin, s_pinBuf, sizeof(pin));
                PinPadDoneCb cb = s_pinDoneCb;
                s_pinDoneCb = nullptr;
                pinPadClose();
                if (cb) cb(true, pin, false);
            }
        }, LV_EVENT_CLICKED, (void*)keys[i]);
        lv_obj_t* tt = lv_label_create(b);
        // Backspace / cancel show a symbol; digits show the digit.
        const char* label = keys[i];
        if (strcmp(keys[i], "<") == 0) label = LV_SYMBOL_BACKSPACE;
        if (strcmp(keys[i], "X") == 0) label = LV_SYMBOL_CLOSE;
        lv_label_set_text(tt, label);
        lv_obj_set_style_text_color(tt, lv_color_white(), 0);
        lv_obj_set_style_text_font(tt, &lv_font_montserrat_22, 0);
        lv_obj_set_style_text_letter_space(tt, 0, 0);
        lv_obj_center(tt);
    }

    // Optional long "Remove" button below the grid.
    if (removeLabel && removeLabel[0]) {
        const int longY = gridY + 4 * (kh + gap);
        lv_obj_t* rb = lv_btn_create(s_pinPadRoot);
        lv_obj_set_size(rb, gridW, kh);
        lv_obj_set_pos(rb, gridX, longY);
        lv_obj_set_style_bg_color(rb, lvhex(0x8A2A2A), 0);
        lv_obj_set_style_bg_color(rb, lvhex(0x8A2A2A), LV_STATE_PRESSED);
        lv_obj_set_style_radius(rb, 8, 0);
        lv_obj_set_style_shadow_width(rb, 0, 0);
        killWidgetAnims(rb);
        lv_obj_add_event_cb(rb, [](lv_event_t*){
            PinPadDoneCb cb = s_pinDoneCb;
            s_pinDoneCb = nullptr;
            pinPadClose();
            if (cb) cb(true, "", true);   // wantRemove=true
        }, LV_EVENT_CLICKED, nullptr);
        lv_obj_t* rl = lv_label_create(rb);
        lv_label_set_text(rl, removeLabel);
        lv_obj_set_style_text_color(rl, lv_color_white(), 0);
        lv_obj_set_style_text_font(rl, &lv_font_montserrat_18, 0);
        lv_obj_set_style_text_letter_space(rl, 0, 0);
        lv_obj_center(rl);
    }
}

// Callbacks for "Set PIN" / "Set Duress" action buttons.
static void setPinBtnCb(lv_event_t*) {
    // Remove long-button only offered when a PIN is currently set.
    const char* removeLabel = (s_pinValue[0]) ? "Remove PIN" : "";
    watchUiShowPinPad("Enter new PIN", removeLabel, [](bool ok, const char* pin, bool wantRemove){
        if (!ok) return;
        if (wantRemove) {
            // Also implicitly wipe the duress code — a lone duress with no
            // PIN is a security footgun. Setter code accepts "" as clear.
            s_pending.has_pin        = true;
            s_pending.pin_value[0]   = '\0';
            s_pending.has_duress     = true;
            s_pending.duress_value[0] = '\0';
            watchUiFlash(LV_SYMBOL_OK "  PIN removed");
            return;
        }
        s_pending.has_pin = true;
        strncpy(s_pending.pin_value, pin, sizeof(s_pending.pin_value) - 1);
        s_pending.pin_value[sizeof(s_pending.pin_value) - 1] = '\0';
        watchUiFlash(LV_SYMBOL_OK "  PIN saved");
    });
}
static void setDuressBtnCb(lv_event_t*) {
    // Duress requires a PIN — a duress code only exists to be entered
    // INSTEAD of the real PIN, so with no PIN it makes no sense.
    if (!s_pinValue[0]) {
        watchUiFlash("Set a PIN first before a duress code");
        return;
    }
    const char* removeLabel = (s_duressValue[0]) ? "Remove duress code" : "";
    watchUiShowPinPad("Enter duress code", removeLabel, [](bool ok, const char* pin, bool wantRemove){
        if (!ok) return;
        if (wantRemove) {
            s_pending.has_duress       = true;
            s_pending.duress_value[0]  = '\0';
            watchUiFlash(LV_SYMBOL_OK "  Duress code removed");
            return;
        }
        // Duress must differ from PIN.
        if (s_pinValue[0] && strcmp(s_pinValue, pin) == 0) {
            watchUiFlash("Duress code can't equal PIN");
            return;
        }
        s_pending.has_duress = true;
        strncpy(s_pending.duress_value, pin, sizeof(s_pending.duress_value) - 1);
        s_pending.duress_value[sizeof(s_pending.duress_value) - 1] = '\0';
        watchUiFlash(LV_SYMBOL_OK "  Duress code saved");
    });
}

static lv_obj_t* makeSettingsPage(lv_obj_t* tab) {
    lv_obj_t* page = lv_obj_create(tab);
    lv_obj_remove_style_all(page);
    // Bug-hunt R2: the manual tab strip lives outside the tabview (on scr),
    // so the tab content already has full tabview-content height. Only
    // subtract the bottom dot indicator strip AND the strip's top padding
    // (which shifted the whole tabview down). Previously we subtracted a
    // stale 56 that used to be the built-in tab bar size.
    lv_obj_set_size(page, LCD_W, LCD_H - STRIP_H - STRIP_PAD_TOP - DOT_STRIP_H);
    lv_obj_set_pos(page, 0, 0);
    lv_obj_set_style_bg_color(page, lvhex(C_BG), 0);
    lv_obj_set_flex_flow(page, LV_FLEX_FLOW_COLUMN);
    lv_obj_set_flex_align(page, LV_FLEX_ALIGN_CENTER,
                                LV_FLEX_ALIGN_CENTER,
                                LV_FLEX_ALIGN_CENTER);
    lv_obj_set_style_pad_top   (page, SAFE_TOP, 0);
    lv_obj_set_style_pad_bottom(page, SAFE_BOT, 0);
    lv_obj_set_style_pad_left  (page, SAFE_X, 0);
    lv_obj_set_style_pad_right (page, SAFE_X, 0);
    lv_obj_set_style_pad_row   (page, 6, 0);
    lv_obj_clear_flag(page, LV_OBJ_FLAG_SCROLLABLE);
    return page;
}

static void buildSettingsTab(lv_obj_t* tab) {
    lv_obj_set_style_bg_color(tab, lvhex(C_BG), 0);
    lv_obj_set_style_pad_all(tab, 0, 0);
    lv_obj_clear_flag(tab, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_set_scrollbar_mode(tab, LV_SCROLLBAR_MODE_OFF);

    // Page 1 — Radios (WiFi, BT, BT discovery, Silent startup)
    s_settingsPages[0] = makeSettingsPage(tab);
    makeBigSwitchRow(s_settingsPages[0], LV_SYMBOL_WIFI,      "WiFi AP",         true, settingSwitchCb, SET_WIFI);
    makeBigSwitchRow(s_settingsPages[0], LV_SYMBOL_BLUETOOTH, "Bluetooth",       false, settingSwitchCb, SET_BT);
    makeBigSwitchRow(s_settingsPages[0], LV_SYMBOL_EYE_OPEN,  "BT discovery",    false, settingSwitchCb, SET_BTDISC);
    makeBigSwitchRow(s_settingsPages[0], LV_SYMBOL_EYE_CLOSE, "Silent USB",      false, settingSwitchCb, SET_SILENT);

    // Page 2 — Behaviour (Logging, COM shell, Autostart)
    // DeadNet + LAN-status row were re-added by the swarm bug-hunt agent
    // per an enum slot they saw, but the user explicitly asked earlier
    // for DeadNet to live in the WEB dashboard only (needs the risky-
    // mode acknowledge checkbox which is impractical on wrist size).
    // The watchUiSetDeadnetToggle / watchUiSetLanConnected setters
    // remain wired — they now no-op when their target widgets don't
    // exist, which is correct for this build.
    s_settingsPages[1] = makeSettingsPage(tab);
    makeBigSwitchRow(s_settingsPages[1], LV_SYMBOL_LIST,      "Log to SD",       false, settingSwitchCb, SET_LOGGING);
    makeBigSwitchRow(s_settingsPages[1], LV_SYMBOL_USB,       "COM shell (CDC)", false, settingSwitchCb, SET_COM);
    // Two mutually-exclusive bootscript triggers. Enabling one auto-
    // disables the other (see settingSwitchCb).
    makeBigSwitchRow(s_settingsPages[1], LV_SYMBOL_REFRESH,  "Autorun at boot",       false, settingSwitchCb, SET_AUTOSTART);
    makeBigSwitchRow(s_settingsPages[1], LV_SYMBOL_PLAY,     "Autostart on USB attach", false, settingSwitchCb, SET_AUTOATTACH);

    // Page 3 — Personalization (Screen sleep, Brightness, Set PIN, Set Duress)
    s_settingsPages[2] = makeSettingsPage(tab);
    makeBigSwitchRow (s_settingsPages[2], LV_SYMBOL_POWER,   "Auto screen sleep", false, settingSwitchCb, SET_SCREEN_SLEEP);
    makeBrightnessRow(s_settingsPages[2], LV_SYMBOL_IMAGE,   "Brightness", 10);
    makeBigActionRow (s_settingsPages[2], LV_SYMBOL_KEYBOARD,"Set PIN",        "SET", false, setPinBtnCb);
    makeBigActionRow (s_settingsPages[2], LV_SYMBOL_WARNING, "Set duress code","SET", true,  setDuressBtnCb);

    // Page 4 — Actions (Reboot, Reset to standard, Factory reset, Brick)
    s_settingsPages[3] = makeSettingsPage(tab);
    makeBigActionRow(s_settingsPages[3], LV_SYMBOL_REFRESH, "Reboot",            "REBOOT", false, rebootBtnCb);
    makeBigActionRow(s_settingsPages[3], LV_SYMBOL_LOOP,    "Reset to standard", "RESET",  false, resetStdBtnCb);
    makeBigActionRow(s_settingsPages[3], LV_SYMBOL_TRASH,   "Factory reset",     "WIPE",   true,  factoryResetBtnCb);
    makeBigActionRow(s_settingsPages[3], LV_SYMBOL_WARNING, "Brick firmware",    "BRICK",  true,  brickBtnCb);

    // Dot indicator strip at the bottom
    s_settingsPageIndicator = lv_obj_create(tab);
    lv_obj_remove_style_all(s_settingsPageIndicator);
    lv_obj_set_size(s_settingsPageIndicator, LCD_W, 30);
    lv_obj_align(s_settingsPageIndicator, LV_ALIGN_BOTTOM_MID, 0, 0);
    lv_obj_set_style_bg_color(s_settingsPageIndicator, lvhex(C_BG), 0);
    lv_obj_set_style_bg_opa(s_settingsPageIndicator, LV_OPA_COVER, 0);
    lv_obj_set_flex_flow(s_settingsPageIndicator, LV_FLEX_FLOW_ROW);
    lv_obj_set_flex_align(s_settingsPageIndicator, LV_FLEX_ALIGN_CENTER,
                                                    LV_FLEX_ALIGN_CENTER,
                                                    LV_FLEX_ALIGN_CENTER);
    lv_obj_set_style_pad_gap(s_settingsPageIndicator, 12, 0);
    lv_obj_clear_flag(s_settingsPageIndicator, LV_OBJ_FLAG_SCROLLABLE);
    for (int i = 0; i < SETTINGS_PAGE_COUNT; i++) {
        lv_obj_t* d = lv_obj_create(s_settingsPageIndicator);
        lv_obj_remove_style_all(d);
        lv_obj_set_size(d, 10, 10);
        lv_obj_set_style_radius(d, LV_RADIUS_CIRCLE, 0);
        lv_obj_set_style_bg_opa(d, LV_OPA_COVER, 0);
        lv_obj_set_style_bg_color(d, lvhex(C_LINE), 0);
        // Dots aren't buttons — no click, no press, no focus states.
        // Otherwise LVGL was tinting dot 0 blue via the default focused/
        // pressed style that leaked in via LV_STATE_ANY on the first
        // child of a flex container. Kill all state-driven color paths.
        lv_obj_clear_flag(d, LV_OBJ_FLAG_CLICKABLE);
        lv_obj_clear_flag(d, LV_OBJ_FLAG_CLICK_FOCUSABLE);
        lv_obj_clear_flag(d, LV_OBJ_FLAG_SCROLLABLE);
        lv_obj_set_style_border_width(d, 0, 0);
        lv_obj_set_style_border_width(d, 0, LV_STATE_PRESSED);
        lv_obj_set_style_border_width(d, 0, LV_STATE_FOCUSED);
        lv_obj_set_style_outline_width(d, 0, 0);
        lv_obj_set_style_outline_width(d, 0, LV_STATE_PRESSED);
        lv_obj_set_style_outline_width(d, 0, LV_STATE_FOCUSED);
        lv_obj_set_style_shadow_width(d, 0, 0);
        // Pin bg_color in every state so no theme override tints an
        // unselected dot.
        lv_obj_set_style_bg_color(d, lvhex(C_LINE), LV_STATE_PRESSED);
        lv_obj_set_style_bg_color(d, lvhex(C_LINE), LV_STATE_FOCUSED);
        s_settingsDots[i] = d;
    }

    // Bug-hunt R3: the tab-level gesture handler was removed; a single
    // handler on the tabview (see watchUiBegin) owns Settings LEFT/RIGHT
    // paging now. Two handlers = double-advance per swipe.

    // Show page 1 first.
    settingsShowPage(0);
}

// ---- public API ------------------------------------------------------------
void watchUiBegin() {
    static bool inited = false;
    if (inited) return;   // R15: guard against a second call re-adding widgets
    inited = true;
    Serial.println("[WatchUI] Bringing up display + touch");
    Screen.on();
    Touch.on();
    tryInitPMU();

    lv_obj_t* scr = lv_scr_act();
    lv_obj_set_style_bg_color(scr, lvhex(C_BG), 0);
    lv_obj_set_style_pad_all(scr, 0, 0);

    // Manual 3-container tab system.
    //
    // We used to use lv_tabview here, but its class handlers competed with
    // our own for LV_EVENT_GESTURE and for LV_EVENT_VALUE_CHANGED — the
    // net effect was that Settings-page swipes never advanced past page 0
    // AND every tab change triggered a redundant reset. Rebuilt as three
    // plain lv_obj containers on the screen with manual show/hide via
    // showTab(). Zero LVGL class-handler interference; every tab change
    // and page change is fully driven by our own code with no animation.
    //
    // Shift the tab CONTENT area down so the manual tab strip fits above.
    // Strip origin is STRIP_PAD_TOP (not 0) so the button row sits below
    // the AMOLED's curved top bezel. Total top footprint = STRIP_PAD_TOP +
    // STRIP_H.
    const int stripY   = STRIP_PAD_TOP;
    const int contentY = STRIP_PAD_TOP + STRIP_H;
    const int tabW     = LCD_W;
    const int tabH     = LCD_H - contentY;

    auto makeTabContainer = [&]() {
        lv_obj_t* c = lv_obj_create(scr);
        lv_obj_remove_style_all(c);
        lv_obj_set_size(c, tabW, tabH);
        lv_obj_set_pos (c, 0, contentY);
        lv_obj_set_style_bg_color(c, lvhex(C_BG), 0);
        lv_obj_set_style_bg_opa  (c, LV_OPA_COVER, 0);
        lv_obj_set_style_pad_all (c, 0, 0);
        return c;
    };

    s_tabHome  = makeTabContainer();
    s_tabFiles = makeTabContainer();
    s_tabSet   = makeTabContainer();
    buildHomeTab(s_tabHome);
    buildFilesTab(s_tabFiles);
    buildSettingsTab(s_tabSet);

    // s_tabView kept non-null as a "UI is up" flag — legacy code that
    // checks `if (s_tabView)` still works, but nothing else uses it.
    s_tabView = s_tabHome;

    // Hide Files and Settings; Home stays visible on boot.
    lv_obj_add_flag(s_tabFiles, LV_OBJ_FLAG_HIDDEN);
    lv_obj_add_flag(s_tabSet,   LV_OBJ_FLAG_HIDDEN);

    // ---- Manual tab strip -----------------------------------------------
    // stripY / contentY were computed above where the tab containers are
    // built. Tab strip sits above the containers, buttons in the strip
    // drive showTab() to swap containers.

    lv_obj_t* strip = lv_obj_create(scr);
    lv_obj_remove_style_all(strip);
    lv_obj_set_size(strip, LCD_W, STRIP_H);
    lv_obj_set_pos (strip, 0, stripY);
    lv_obj_set_style_bg_color(strip, lvhex(C_SURFACE), 0);
    lv_obj_set_style_bg_opa(strip, LV_OPA_COVER, 0);
    lv_obj_set_style_border_side(strip, LV_BORDER_SIDE_BOTTOM, 0);
    lv_obj_set_style_border_width(strip, 1, 0);
    lv_obj_set_style_border_color(strip, lvhex(C_LINE), 0);
    lv_obj_clear_flag(strip, LV_OBJ_FLAG_SCROLLABLE);

    // s_manualLbls / s_manualUnder are now FILE-SCOPE (see near top of
    // file). This lets manualStripRepaint() re-tint the active tab from
    // any handler — click, VALUE_CHANGED, or gesture. s_manualBtns stays
    // local (nothing outside this scope needs it).
    static lv_obj_t* s_manualBtns  [3] = { nullptr };
    const char* iconChars [3] = { LV_SYMBOL_HOME, LV_SYMBOL_FILE, LV_SYMBOL_SETTINGS };
    // Bug-hunt R1: "Set" was an accidental truncation ("Set" reads as the
    // verb, not "Settings"). Full word fits inside btnW=110 at Montserrat
    // 16 (icon ~14 + 2 spaces + "Settings" ~68 = ~92 px).
    const char* labels    [3] = { "Home", "Files", "Settings" };
    const int stripInner = LCD_W - 2 * SAFE_X;
    const int btnW = stripInner / 3;

    for (int i = 0; i < 3; i++) {
        lv_obj_t* btn = lv_btn_create(strip);
        lv_obj_remove_style_all(btn);
        lv_obj_set_size(btn, btnW, STRIP_H - 4);
        lv_obj_set_pos(btn, SAFE_X + i * btnW, 2);
        lv_obj_set_style_bg_opa(btn, LV_OPA_TRANSP, 0);
        lv_obj_set_style_shadow_width(btn, 0, 0);
        lv_obj_set_style_border_width(btn, 0, 0);
        lv_obj_add_event_cb(btn, [](lv_event_t* e) {
            int idx = (int)(intptr_t)lv_event_get_user_data(e);
            showTab(idx);   // hides/shows containers + updates strip + resets page
        }, LV_EVENT_CLICKED, (void*)(intptr_t)i);
        s_manualBtns[i] = btn;

        // Label — pinned Montserrat 16, static text, no state variations.
        lv_obj_t* lbl = lv_label_create(btn);
        char buf[24];
        snprintf(buf, sizeof(buf), "%s  %s", iconChars[i], labels[i]);
        lv_label_set_text(lbl, buf);
        lv_obj_set_style_text_font(lbl, &lv_font_montserrat_16, 0);
        lv_obj_set_style_text_letter_space(lbl, 0, 0);
        lv_obj_set_style_text_color(lbl, lvhex(C_MUTED), 0);
        lv_obj_center(lbl);
        s_manualLbls[i] = lbl;

        // Bottom underline — always fully opaque, visibility controlled
        // via LV_OBJ_FLAG_HIDDEN in manualStripRepaint. Starting hidden
        // for all three; initial paint below unhides tab 0.
        lv_obj_t* u = lv_obj_create(btn);
        lv_obj_remove_style_all(u);
        lv_obj_set_size(u, btnW - 20, 3);
        lv_obj_align(u, LV_ALIGN_BOTTOM_MID, 0, -2);
        lv_obj_set_style_bg_opa(u, LV_OPA_COVER, 0);
        lv_obj_set_style_bg_color(u, lvhex(C_ACCENT), 0);
        lv_obj_set_style_radius(u, 2, 0);
        lv_obj_add_flag(u, LV_OBJ_FLAG_HIDDEN);
        s_manualUnder[i] = u;
    }

    manualStripRepaint();  // initial paint (Home active)

    // Horizontal-gesture handler on the SCREEN. No lv_tabview class
    // handler to compete with anymore — this is the ONLY code that
    // reacts to LEFT/RIGHT swipes. TOP is handled by the screen-level
    // clock-swipe-up handler further down.
    //
    // Direction rules:
    //   Home  (0): LEFT → Files.                RIGHT → nothing.
    //   Files (1): LEFT → Settings.             RIGHT → Home.
    //   Set.  (2): LEFT → next page (or stay).  RIGHT → prev page or Files.
    lv_obj_add_event_cb(scr, [](lv_event_t* e){
        if (lv_event_get_code(e) != LV_EVENT_GESTURE) return;
        lv_indev_t* indev = lv_indev_active();
        if (!indev) return;
        lv_dir_t d = lv_indev_get_gesture_dir(indev);
        if (d != LV_DIR_LEFT && d != LV_DIR_RIGHT) return;
        // Bug-hunt R2: don't sneak-change hidden tabs while the clock
        // overlay is covering the screen — the user sees no change and
        // is left thinking "the swipe didn't work" while s_activeTab
        // silently marches. Ignore LEFT/RIGHT while the clock is up;
        // only the UP-swipe dismiss (see clockGestureCb + the DOWN/UP
        // scr cb below) may act on clock-time gestures.
        if (watchUiClockVisible()) return;
        if (s_activeTab == 2) {           // Settings
            if (d == LV_DIR_LEFT) {
                if (s_settingsPage < SETTINGS_PAGE_COUNT - 1)
                    settingsShowPage(s_settingsPage + 1);
            } else {  // RIGHT
                if (s_settingsPage > 0) settingsShowPage(s_settingsPage - 1);
                else                    showTab(1);   // page 0 → Files
            }
        } else if (s_activeTab == 1) {    // Files
            showTab(d == LV_DIR_LEFT ? 2 : 0);
        } else {                          // Home
            if (d == LV_DIR_LEFT) showTab(1);
        }
        lv_indev_wait_release(indev);
        lv_event_stop_processing(e);
    }, LV_EVENT_GESTURE, nullptr);

    // Toast on lv_layer_top() so watchUiFlash() from any tab is visible.
    s_flashLbl = lv_label_create(lv_layer_top());
    lv_obj_set_style_text_color(s_flashLbl, lvhex(0xFFB020), 0);
    lv_obj_set_style_text_font(s_flashLbl, &lv_font_montserrat_18, 0);
    lv_obj_set_style_text_letter_space(s_flashLbl, 0, 0);
    lv_obj_set_style_bg_color(s_flashLbl, lvhex(0x201510), 0);
    lv_obj_set_style_bg_opa(s_flashLbl, LV_OPA_90, 0);
    lv_obj_set_style_pad_all(s_flashLbl, 10, 0);
    lv_obj_set_style_radius(s_flashLbl, 8, 0);
    lv_obj_set_width(s_flashLbl, LCD_W - 40);
    lv_label_set_long_mode(s_flashLbl, LV_LABEL_LONG_WRAP);
    lv_label_set_text(s_flashLbl, "");
    lv_obj_align(s_flashLbl, LV_ALIGN_BOTTOM_MID, 0, -110);
    lv_obj_add_flag(s_flashLbl, LV_OBJ_FLAG_HIDDEN);

    // Banner on lv_layer_top() — floats above every tab and the clock. See
    // watchUiSetBanner() which move-foregrounds it on each show so the
    // clock overlay never occludes the countdown / hot-plug toast.
    s_bannerLbl = lv_label_create(lv_layer_top());
    lv_obj_set_style_text_color(s_bannerLbl, lv_color_white(), 0);
    lv_obj_set_style_text_font(s_bannerLbl, &lv_font_montserrat_18, 0);
    lv_obj_set_style_text_letter_space(s_bannerLbl, 0, 0);
    lv_obj_set_style_bg_color(s_bannerLbl, lvhex(C_LINE), 0);
    lv_obj_set_style_bg_opa(s_bannerLbl, LV_OPA_COVER, 0);
    lv_obj_set_style_pad_all(s_bannerLbl, 10, 0);
    lv_obj_set_style_radius(s_bannerLbl, 8, 0);
    lv_obj_set_width(s_bannerLbl, LCD_W - 40);
    lv_obj_set_style_text_align(s_bannerLbl, LV_TEXT_ALIGN_CENTER, 0);
    lv_label_set_long_mode(s_bannerLbl, LV_LABEL_LONG_WRAP);
    lv_label_set_text(s_bannerLbl, "");
    lv_obj_align(s_bannerLbl, LV_ALIGN_TOP_MID, 0, 70);
    lv_obj_add_flag(s_bannerLbl, LV_OBJ_FLAG_HIDDEN);

    // Persistent "RUNNING <script>" banner on lv_layer_top so it's
    // visible from EVERY tab — Home, Files, Settings, even the PIN
    // pad or clock overlay. Toggled by watchUiSetScriptState().
    s_runBanner = lv_obj_create(lv_layer_top());
    lv_obj_remove_style_all(s_runBanner);
    lv_obj_set_size(s_runBanner, LCD_W, 40);
    lv_obj_set_pos(s_runBanner, 0, 0);
    lv_obj_set_style_bg_color(s_runBanner, lvhex(0xE5A93A), 0);   // orange
    lv_obj_set_style_bg_opa(s_runBanner, LV_OPA_COVER, 0);
    lv_obj_clear_flag(s_runBanner, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_clear_flag(s_runBanner, LV_OBJ_FLAG_CLICKABLE);
    lv_obj_add_flag(s_runBanner, LV_OBJ_FLAG_HIDDEN);
    s_runBannerLbl = lv_label_create(s_runBanner);
    lv_label_set_text(s_runBannerLbl, "");
    lv_obj_set_style_text_color(s_runBannerLbl, lv_color_white(), 0);
    lv_obj_set_style_text_font(s_runBannerLbl, &lv_font_montserrat_18, 0);
    lv_obj_set_style_text_letter_space(s_runBannerLbl, 0, 0);
    lv_obj_set_width(s_runBannerLbl, LCD_W - 20);
    lv_obj_set_style_text_align(s_runBannerLbl, LV_TEXT_ALIGN_CENTER, 0);
    lv_label_set_long_mode(s_runBannerLbl, LV_LABEL_LONG_DOT);
    lv_obj_center(s_runBannerLbl);

    // Swipe-UP anywhere on the screen brings the clock back. The tabview's
    // own gesture handler is one place; attaching a screen-level fallback
    // too catches gestures inside scrollable tab-content that would
    // otherwise be consumed by the scroll.
    lv_obj_add_event_cb(lv_scr_act(), [](lv_event_t* e){
        if (lv_event_get_code(e) != LV_EVENT_GESTURE) return;
        lv_indev_t* indev = lv_indev_active();
        if (!indev) return;
        lv_dir_t d = lv_indev_get_gesture_dir(indev);
        // Screen-level handler for BOTH show + hide directions. Attaching
        // the hide handler to the clock's own overlay (which sits on
        // lv_layer_top) was unreliable — LVGL 9 didn't route gesture
        // events to layer_top children in the same way as regular screen
        // children, so the wearer was stuck on the clock. scr fires for
        // every touch regardless of what layer is on top, so both
        // directions work.
        if (d == LV_DIR_BOTTOM && !watchUiClockVisible()) {
            watchUiShowClock();
        } else if (d == LV_DIR_TOP && watchUiClockVisible()) {
            // PIN gate: only prompt if a PIN is actually set; otherwise
            // dismiss instantly.
            if (!s_pinValue[0]) {
                watchUiHideClock();
            } else {
                watchUiShowPinPad("Enter PIN", "", [](bool ok, const char* pin, bool /*wantRemove*/){
                    if (!ok) return;
                    if (s_duressValue[0] && strcmp(pin, s_duressValue) == 0) {
                        s_pendingExtras.want_duress = true;
                        watchUiHideClock();
                        return;
                    }
                    if (strcmp(pin, s_pinValue) == 0) watchUiHideClock();
                    else                              watchUiFlash("Wrong PIN");
                });
            }
        }
    }, LV_EVENT_GESTURE, nullptr);

    // NOTE: an earlier version called killAllAnimsRec(lv_scr_act()) here
    // to nuke every button-press fade and transition. It rebooted the
    // watch on any touch — lv_obj_set_style_transition(obj, nullptr, ...)
    // on the wrong part/state combos crashed LVGL's transition subsystem
    // the next time a state changed. Left disabled until we find a safe
    // per-widget kill. Per-widget style pins already zero anim_duration on
    // the switches (see makeBigSwitchRow) and the tab-strip buttons
    // remove_style_all so the theme's press-fade never applies there.

    // Clock face — shown at boot; swipe down to reveal the tabview.
    watchUiShowClock();

    Serial.println("[WatchUI] Ready");
}

// ---- Clock overlay ---------------------------------------------------------
// Cached last-rendered clock text — file-scope so watchUiShowClock() can
// invalidate it when the clock overlay is re-created (otherwise the
// change-detect in watchUiSetClockSeconds skips the first-frame paint
// and the wearer sees "00:00" for a beat before the next tick fires).
static char s_lastClockText[8] = "";

static void clockGestureCb(lv_event_t* e) {
    // Bug-hunt R1: the clock is now parented to lv_scr_act() (was
    // lv_layer_top()), so gestures DO fire directly on the clock root.
    // Handling the dismiss HERE guarantees the swipe-up reaches us even
    // if event-bubbling to scr's cb is suppressed (a scrollable ancestor
    // eating the vertical drag, a same-touch reclassification into a
    // horizontal gesture that stop_processing'd the scr cb chain, etc.).
    // Belt-and-suspenders with the scr-level cb.
    if (lv_event_get_code(e) != LV_EVENT_GESTURE) return;
    lv_indev_t* indev = lv_indev_active();
    if (!indev) return;
    lv_dir_t d = lv_indev_get_gesture_dir(indev);
    if (d != LV_DIR_TOP) return;
    if (!watchUiClockVisible()) return;
    // No PIN — dismiss directly; PIN set — challenge (duress path handled
    // in the pad callback, matches the scr-level handler exactly).
    if (!s_pinValue[0]) {
        watchUiHideClock();
        return;
    }
    watchUiShowPinPad("Enter PIN", "", [](bool ok, const char* pin, bool /*wantRemove*/){
        if (!ok) return;
        if (s_duressValue[0] && strcmp(pin, s_duressValue) == 0) {
            s_pendingExtras.want_duress = true;
            watchUiHideClock();
            return;
        }
        if (strcmp(pin, s_pinValue) == 0) watchUiHideClock();
        else                              watchUiFlash("Wrong PIN");
    });
}
static void clockClickCb(lv_event_t* /*e*/) {
    // No-op — swipe UP dismisses per user request.
}

void watchUiShowClock() {
    if (s_bricked) return;    // brick failsafe: no clock overlay
    if (s_clockRoot) {
        lv_obj_clear_flag(s_clockRoot, LV_OBJ_FLAG_HIDDEN);
        return;
    }
    // Task #2: brand-new s_clockTime is coming in below with text "00:00".
    // Invalidate the change-detect cache so the first watchUiSetClock-
    // Seconds() call below actually updates the label instead of skipping
    // as an "unchanged" write.
    s_lastClockText[0] = '\0';
    // Clock overlay parents to lv_scr_act (NOT lv_layer_top). LVGL 9
    // does not bubble gesture events from layer_top children up to
    // scr_act, which is where our screen-level swipe handler lives —
    // putting the clock on scr_act as its top child means gestures on
    // the clock fire on the clock, bubble to scr_act, and reach our
    // handler. Flash + banner overlays remain on lv_layer_top so they
    // paint above the clock.
    s_clockRoot = lv_obj_create(lv_scr_act());
    lv_obj_remove_style_all(s_clockRoot);
    lv_obj_set_size(s_clockRoot, LCD_W, LCD_H);
    lv_obj_set_pos(s_clockRoot, 0, 0);
    lv_obj_set_style_bg_color(s_clockRoot, lvhex(C_BG), 0);
    lv_obj_set_style_bg_opa(s_clockRoot, LV_OPA_COVER, 0);
    lv_obj_clear_flag(s_clockRoot, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_add_flag(s_clockRoot, LV_OBJ_FLAG_CLICKABLE);
    lv_obj_move_foreground(s_clockRoot);   // top of scr's z-order
    lv_obj_add_event_cb(s_clockRoot, clockGestureCb, LV_EVENT_GESTURE, nullptr);
    lv_obj_add_event_cb(s_clockRoot, clockClickCb,   LV_EVENT_CLICKED, nullptr);

    s_clockTime = lv_label_create(s_clockRoot);
    lv_label_set_text(s_clockTime, "00:00");
    lv_obj_set_style_text_color(s_clockTime, lv_color_white(), 0);
    lv_obj_set_style_text_font(s_clockTime, &lv_font_montserrat_48, 0);
    // "Clock stretches as time goes up" fix: LV_SIZE_CONTENT (the default)
    // makes the label re-flow to fit each new HH:MM string. Every rewrite
    // triggers a layout pass that briefly enlarges/shrinks the widget's
    // bounding box — visible as horizontal stretch. Pin an explicit width
    // that fits the widest possible HH:MM ("00:00"-"23:59") and center
    // text INSIDE that fixed frame. Height also fixed so vertical layout
    // doesn't dance either.
    lv_obj_set_size(s_clockTime, LCD_W - 40, 80);
    lv_obj_set_style_text_align(s_clockTime, LV_TEXT_ALIGN_CENTER, 0);
    lv_obj_set_style_transform_scale_x(s_clockTime, 256, 0);
    lv_obj_set_style_transform_scale_y(s_clockTime, 256, 0);
    lv_obj_set_style_text_letter_space(s_clockTime, 0, 0);
    lv_obj_center(s_clockTime);
    // Hint text removed per user request — clock face stays clean.
    s_clockHint = nullptr;

    // Task #2: kill the "00:00 for a beat when I swipe up twice" flash.
    // Previously the label was created with placeholder text "00:00" and
    // the next 1 Hz tick in the .ino updated it — that's a full second of
    // wrong time visible. Compute the current time here from the persisted
    // NVS clock and paint it before the LVGL redraw so the FIRST frame
    // the wearer sees is the right HH:MM.
    {
        uint64_t syncEpoch = preferences.getULong64("clock_epoch",   0);
        int32_t  tz        = preferences.getLong   ("clock_tz_secs", 0);
        uint32_t syncMs    = preferences.getULong  ("clock_sync_ms", 0);
        uint64_t nowSecs;
        if (syncEpoch >= 1700000000ULL) {
            uint32_t elapsedMs = millis() - syncMs;
            nowSecs = syncEpoch + (elapsedMs / 1000) + tz;
        } else {
            nowSecs = millis() / 1000;
        }
        watchUiSetClockSeconds((uint32_t)(nowSecs & 0xFFFFFFFFULL));
    }
}

void watchUiHideClock() {
    if (!s_clockRoot) return;
    // CRITICAL: this function is called SYNCHRONOUSLY from the clock's own
    // event callback (clockGestureCb / clockClickCb attached to s_clockRoot).
    // lv_obj_del() on the target of an in-flight event frees memory that
    // LVGL 9's indev still points to (act_obj, gesture tracker, last_pressed
    // ref) — the immediate delete looks fine, but the NEXT touch dispatches
    // through the freed pointer and the S3 hits LoadProhibited, reboots,
    // and boot puts the clock right back — the exact "swipe down works,
    // any touch reboots to clock" symptom.
    //
    // Fix: NULL the tracking pointers FIRST so watchUiClockVisible() and
    // watchUiSetClockSeconds() ignore the doomed object immediately, then
    // schedule the delete via lv_obj_del_async() so LVGL finishes the
    // current event dispatch before actually freeing the widget.
    lv_obj_t* doomed = s_clockRoot;
    s_clockRoot = nullptr;
    s_clockTime = nullptr;
    s_clockHint = nullptr;
    lv_obj_del_async(doomed);
}

bool watchUiClockVisible() {
    return s_clockRoot != nullptr;
}

void watchUiSetClockSeconds(uint32_t seconds) {
    if (!s_clockTime) return;
    uint32_t h = (seconds / 3600) % 24;
    uint32_t m = (seconds / 60) % 60;
    char buf[8];
    snprintf(buf, sizeof(buf), "%02u:%02u", (unsigned)h, (unsigned)m);
    if (strcmp(s_lastClockText, buf) == 0) return;
    strncpy(s_lastClockText, buf, sizeof(s_lastClockText));
    s_lastClockText[sizeof(s_lastClockText) - 1] = '\0';
    lv_label_set_text(s_clockTime, buf);
}

// ---- Power-off long-press countdown ---------------------------------------
void watchUiBeginPowerHold() {
    s_powerHold = true;
    watchUiSetBanner(LV_SYMBOL_POWER "  Hold to power off (4)", 0xC8442D);
}
void watchUiSetPowerHoldRemaining(int seconds) {
    if (!s_powerHold) return;
    if (seconds <= 0) {
        watchUiSetBanner(LV_SYMBOL_POWER "  Powering off...", 0xC8442D);
        return;
    }
    char buf[48];
    snprintf(buf, sizeof(buf),
             LV_SYMBOL_POWER "  Hold %d s to power off", seconds);
    watchUiSetBanner(buf, 0xC8442D);
}
void watchUiEndPowerHold() {
    if (!s_powerHold) return;
    s_powerHold = false;
    watchUiSetBanner(nullptr);
}

// ---- Screen sleep ---------------------------------------------------------
// AMOLED: painting black = pixels dark = ~0 power. Screen.off() blanks the
// framebuffer. We also freeze LVGL's redraw so wake latency is a single flush.
void watchUiScreenSleep() {
    if (s_screenAsleep) return;
    s_screenAsleep = true;
    Screen.off();
}
void watchUiScreenWake() {
    if (!s_screenAsleep) return;
    s_screenAsleep = false;
    // Force a full repaint so the wake tap doesn't stare at a black screen.
    lv_obj_invalidate(lv_scr_act());
    lv_obj_t* top = lv_layer_top();
    if (top) lv_obj_invalidate(top);
    Screen.on();
}
bool watchUiScreenAsleep() { return s_screenAsleep; }

void watchUiPowerOff() {
    // Bug-hunt #11 fix: real shutdown via the AXP2101 PMU. deepSleep(0)
    // was CPU-only — the PMU kept rails alive and the chip woke on any
    // USB/touch event, so DuckyScript SHUTDOWN read as "screen goes
    // black for a second, then everything's back". Now: paint the
    // screen black (visual acknowledgement) and call s_pmu.shutdown().
    Screen.off();
    if (s_pmuReady) {
        s_pmu.shutdown();
    }
    // If the PMU didn't shut us down (or wasn't up), deep-sleep as a
    // fallback so the chip at least halts.
    esp_deep_sleep_start();
}

void watchUiTick() {
    unsigned long now = millis();
    if (now - s_lastLvglTick >= 5) {
        lv_tick_inc(now - s_lastLvglTick);
        s_lastLvglTick = now;
    }
    // Watch port perf: rate-limit lv_task_handler to 10 ms. loop() runs at
    // ~1 kHz; every LVGL scheduler pass does invalidate/scan/animation work
    // regardless of dirty state. 10 ms is finer than LVGL's own 33 ms
    // refresh — no visual difference, ~10× less CPU wasted here, which
    // gives touch/server/USB more headroom. The old forced full-screen
    // invalidate every 300 ms is gone — it caused a repaint storm and was
    // only added as a diagnostic that never proved anything.
    // Watch port perf: rate-limit lv_task_handler to 3 ms so touch tracking
    // stays sharp — LVGL's own scheduler still respects LV_DEF_REFR_PERIOD
    // (15 ms). The pass early-outs cheaply when nothing is dirty (~50 µs).
    static unsigned long s_lastHandler = 0;
    if (now - s_lastHandler < 3) return;
    s_lastHandler = now;
    lv_task_handler();

    // LED dot removed from the home tab — the status label carries the same
    // information as text without a repainting circle every tick. Keep the
    // colour state around so callers (LEDManager watchUiSetLed) still compile.
    (void)s_blinkMs;
    (void)s_lastBlinkTick;
    (void)s_blinkOn;

    // Banner timeout — the power-hold banner suppresses auto-hide so the
    // countdown stays visible while the wearer keeps the key down.
    if (s_bannerLbl && !(lv_obj_has_flag(s_bannerLbl, LV_OBJ_FLAG_HIDDEN))) {
        if (!s_powerHold && s_bannerUntil && now > s_bannerUntil) {
            lv_obj_add_flag(s_bannerLbl, LV_OBJ_FLAG_HIDDEN);
            s_bannerUntil = 0;
        }
    }

    // Power-key hold poll — AXP2101 latches negative (press) and positive
    // (release) IRQs; we watch the state and drive the countdown banner.
    // AXP2101 default long-press timeout for power-off is ~4 s.
    if (s_pmuReady) {
        static bool     pkeyDown       = false;
        static uint32_t pkeyDownStart  = 0;
        static unsigned long lastPoll  = 0;
        if (now - lastPoll >= 100) {
            lastPoll = now;
            s_pmu.getIrqStatus();   // refresh cached status
            if (s_pmu.isPekeyNegativeIrq()) {
                pkeyDown = true;
                pkeyDownStart = now;
                watchUiBeginPowerHold();
                s_pmu.clearIrqStatus();
            }
            if (s_pmu.isPekeyPositiveIrq()) {
                pkeyDown = false;
                watchUiEndPowerHold();
                s_pmu.clearIrqStatus();
            }
            if (pkeyDown) {
                const uint32_t HOLD_MS = 4000;
                uint32_t elapsed = now - pkeyDownStart;
                int remaining = elapsed >= HOLD_MS ? 0
                                : (int)((HOLD_MS - elapsed + 999) / 1000);
                watchUiSetPowerHoldRemaining(remaining);
                // AXP2101 handles the actual shutdown at its long-press
                // threshold — we just paint the countdown until it fires.
            }
        }
    }

    // Toast timeout
    if (s_flashLbl && !(lv_obj_has_flag(s_flashLbl, LV_OBJ_FLAG_HIDDEN))) {
        if (s_flashUntil && now > s_flashUntil) {
            lv_obj_add_flag(s_flashLbl, LV_OBJ_FLAG_HIDDEN);
            s_flashUntil = 0;
        }
    }

    // Battery poll (~0.5 Hz). /improve-performance: only rewrite the label
    // when a *change* actually happened. On the AMOLED path, every label
    // set marks the widget dirty which forces a partial redraw; when nothing
    // changed we were spending CPU on identical rewrites 2 times/second.
    if (now - s_lastBattPoll >= 2000) {
        s_lastBattPoll = now;
        if (!s_pmuReady) tryInitPMU();
        if (s_pmuReady && s_battLbl) {
            int   pct = s_pmu.isBatteryConnect() ? s_pmu.getBatteryPercent() : -1;
            int   mv  = (int)s_pmu.getBattVoltage();
            // Quantise mV to 20 mV to dampen jitter — else the tenths digit
            // dances every poll on a stable battery.
            mv = (mv / 20) * 20;
            static int lastPct = -999;
            static int lastMv  = -999;
            if (pct != lastPct || mv != lastMv) {
                lastPct = pct;
                lastMv  = mv;
                const char* icon =
                    pct < 0            ? LV_SYMBOL_USB :
                    pct >= 80          ? LV_SYMBOL_BATTERY_FULL :
                    pct >= 60          ? LV_SYMBOL_BATTERY_3    :
                    pct >= 40          ? LV_SYMBOL_BATTERY_2    :
                    pct >= 20          ? LV_SYMBOL_BATTERY_1    :
                                         LV_SYMBOL_BATTERY_EMPTY;
                char buf[48];
                if (pct >= 0) snprintf(buf, sizeof(buf), "%s  %d%%   %d.%02dV",
                                       icon, pct, mv / 1000, (mv % 1000) / 10);
                else          snprintf(buf, sizeof(buf), "%s  %s  %d.%02dV",
                                       icon,
                                       s_pmu.isVbusIn() ? "USB" : "no batt",
                                       mv / 1000, (mv % 1000) / 10);
                lv_label_set_text(s_battLbl, buf);
            }
        }
    }
}

void watchUiSetLed(uint32_t rgb, uint16_t blinkMs) {
    s_ledColor = rgb & 0xFFFFFF;
    s_blinkMs  = blinkMs;
}

void watchUiFlash(const char* text) {
    if (!s_flashLbl || !text) return;
    lv_label_set_text(s_flashLbl, text);
    lv_obj_clear_flag(s_flashLbl, LV_OBJ_FLAG_HIDDEN);
    // Toast lives on lv_layer_top() with the clock and the banner — force it
    // to the front of the sibling order so a running clock overlay can't
    // occlude the message.
    lv_obj_move_foreground(s_flashLbl);
    s_flashUntil = millis() + 5000;
}

void watchUiSetStatus(const char* text) {
    if (!s_statusLbl || !text) return;
    // /improve-performance: don't rewrite the label when the text didn't
    // change — the .ino loop calls this every 250 ms with the same string
    // 99% of the time.
    static char last[48] = "";
    if (strncmp(last, text, sizeof(last) - 1) == 0) return;
    strncpy(last, text, sizeof(last) - 1);
    last[sizeof(last) - 1] = '\0';
    lv_label_set_text(s_statusLbl, text);
}

void watchUiSetScript(const char* name) {
    if (!s_scriptLbl) return;
    // /improve-performance: same change-detect as setStatus. Called from the
    // 4 Hz loop tick, the script name only changes when execution transitions.
    static char last[64] = "";
    const char* payload = (name && *name) ? name : "";
    if (strncmp(last, payload, sizeof(last) - 1) == 0) return;
    strncpy(last, payload, sizeof(last) - 1);
    last[sizeof(last) - 1] = '\0';
    if (!*payload) lv_label_set_text(s_scriptLbl, LV_SYMBOL_FILE "  no script");
    else           lv_label_set_text_fmt(s_scriptLbl,
                       LV_SYMBOL_FILE "  %s", payload);
}

void watchUiSetClients(int n) {
    if (!s_clientsLbl) return;
    // /improve-performance: only rewrite when the count actually changed.
    // loop() calls this 4x/second — otherwise we mark the label dirty and
    // re-render an identical string.
    static int lastN = -1;
    if (n == lastN) return;
    lastN = n;
    // Bug #12: LV_SYMBOL_WIFI matches the AP+PSK+IP block below better than USB.
    lv_label_set_text_fmt(s_clientsLbl,
        LV_SYMBOL_WIFI "  %d client%s", n, n == 1 ? "" : "s");
}

void watchUiSetAP(const char* ssid, const char* password, const char* ip) {
    if (s_apSsidLbl && ssid)     lv_label_set_text_fmt(s_apSsidLbl,
                                     LV_SYMBOL_WIFI "  AP: %s", ssid);
    if (s_apPassLbl && password) lv_label_set_text_fmt(s_apPassLbl,
                                     "PSK: %s", password);
    if (s_ipLbl && ip)           lv_label_set_text_fmt(s_ipLbl,
                                     "http://%s", ip);
}

void watchUiSetProgress(int percent) {
    if (!s_progress) return;
    if (percent < 0 || percent > 100) {
        lv_obj_add_flag(s_progress, LV_OBJ_FLAG_HIDDEN);
        return;
    }
    lv_obj_clear_flag(s_progress, LV_OBJ_FLAG_HIDDEN);
    lv_bar_set_value(s_progress, percent, LV_ANIM_OFF);
}

bool watchUiConsumeStopPressed() {
    bool v = s_stopEdge;
    s_stopEdge = false;
    return v;
}

WatchUiPendingSettings watchUiConsumePendingSettings() {
    WatchUiPendingSettings p = s_pending;
    s_pending = WatchUiPendingSettings{};
    return p;
}

WatchUiPendingScriptAction watchUiConsumePendingScriptAction() {
    WatchUiPendingScriptAction p = s_pendingAction;
    s_pendingAction = WatchUiPendingScriptAction{};
    return p;
}

void watchUiSetBanner(const char* text, uint32_t rgb) {
    if (!s_bannerLbl) return;
    if (!text || !*text) {
        lv_obj_add_flag(s_bannerLbl, LV_OBJ_FLAG_HIDDEN);
        s_bannerUntil = 0;
        return;
    }
    lv_label_set_text(s_bannerLbl, text);
    lv_obj_set_style_bg_color(s_bannerLbl, lvhex(rgb), 0);
    lv_obj_clear_flag(s_bannerLbl, LV_OBJ_FLAG_HIDDEN);
    // Banner and clock both live on lv_layer_top(). Force the banner to the
    // top of the sibling order so the power-off countdown stays visible
    // even while the clock overlay is up.
    lv_obj_move_foreground(s_bannerLbl);
    // 4-second banner; call watchUiSetBanner(nullptr) to clear early.
    s_bannerUntil = millis() + 4000;
}

void watchUiSetFileList(const std::vector<String>& names,
                        const String& autostartName) {
    if (!s_filesList || !s_filesEmpty) return;
    s_currentAutostart = autostartName;
    // Wipe existing rows. LVGL frees the child widgets; the user_data we set
    // on each button points at std::string entries in s_fileRowNames, so we
    // clear the LVGL tree BEFORE mutating the vector so no callback fires
    // against a freed pointer.
    lv_obj_clean(s_filesList);
    s_fileRowNames.clear();
    s_fileRowNames.reserve(names.size());

    if (names.empty()) {
        lv_obj_clear_flag(s_filesEmpty, LV_OBJ_FLAG_HIDDEN);
        return;
    }
    lv_obj_add_flag(s_filesEmpty, LV_OBJ_FLAG_HIDDEN);

    for (const String& n : names) {
        s_fileRowNames.emplace_back(n.c_str());
        const char* nameCstr = s_fileRowNames.back().c_str();

        lv_obj_t* row = lv_obj_create(s_filesList);
        lv_obj_remove_style_all(row);
        lv_obj_set_size(row, LCD_W - 2 * SAFE_X, 68);
        lv_obj_set_style_pad_hor(row, 8, 0);
        lv_obj_set_style_pad_ver(row, 8, 0);
        lv_obj_set_style_border_side(row, LV_BORDER_SIDE_BOTTOM, 0);
        lv_obj_set_style_border_width(row, 1, 0);
        lv_obj_set_style_border_color(row, lvhex(C_LINE), 0);
        lv_obj_set_flex_flow(row, LV_FLEX_FLOW_ROW);
        lv_obj_set_flex_align(row, LV_FLEX_ALIGN_START,
                                   LV_FLEX_ALIGN_CENTER,
                                   LV_FLEX_ALIGN_CENTER);
        lv_obj_clear_flag(row, LV_OBJ_FLAG_SCROLLABLE);
        lv_obj_set_style_bg_opa(row, LV_OPA_TRANSP, LV_STATE_PRESSED);
        // Row itself is clickable: tapping the name area (anywhere not on
        // a button) selects THIS script + jumps to Home so the wearer can
        // Play/Stop it. LVGL 9: children (buttons) with their own click
        // callbacks eat their clicks first before bubbling, so tapping a
        // button doesn't ALSO trigger the row-click.
        lv_obj_add_flag(row, LV_OBJ_FLAG_CLICKABLE);
        lv_obj_add_event_cb(row, [](lv_event_t* e){
            const char* name = (const char*)lv_event_get_user_data(e);
            if (!name) return;
            watchUiSetSelectedScript(name);
            showTab(0);   // jump to Home
        }, LV_EVENT_CLICKED, (void*)nameCstr);

        lv_obj_t* lbl = lv_label_create(row);
        lv_label_set_text(lbl, nameCstr);
        lv_obj_set_style_text_color(lbl, lvhex(C_TEXT), 0);
        lv_obj_set_style_text_font(lbl, &lv_font_montserrat_18, 0);
        lv_obj_set_style_transform_scale_x(lbl, 256, 0);
        lv_obj_set_style_transform_scale_y(lbl, 256, 0);
        lv_obj_set_style_text_letter_space(lbl, 0, 0);
        lv_label_set_long_mode(lbl, LV_LABEL_LONG_DOT);
        // Task #3: EXPLICIT width instead of flex_grow. flex_grow made the
        // label bounce between its content-size and remaining-space size
        // for a frame on every setFileList rebuild — visible as horizontal
        // stretch. Fixed width takes it out of the layout dance entirely.
        //   Row inner = (LCD_W - 2*SAFE_X) - 2*hpad_8 = 410 - 48 - 16 = 346
        //   Actions   = 3 buttons × 52 px + 2 gaps × 6 = 168 px
        //   Trailing gap after label                    = 8 px
        //   Label width = 346 - 168 - 8                 = 170 px
        // Recompute if any of those constants (SAFE_X, button size, gap)
        // change.
        const int rowInner = (LCD_W - 2 * SAFE_X) - 2 * 8;
        const int actionsW = 3 * 52 + 2 * 6;
        const int labelW   = rowInner - actionsW - 8;
        lv_obj_set_width(lbl, labelW);
        lv_obj_set_style_pad_right(lbl, 8, 0);

        bool isAutostart = (autostartName.length() > 0 &&
                            autostartName == n);

        // Autorun/autostart button. Vivid orange when THIS script is the
        // stored bootscript, muted gray otherwise. Tapping cycles the
        // relationship:
        //   * on any OTHER script       → make this script the bootscript
        //     (writes boot_script = <this>, ensuring ONLY one script at
        //     a time is armed — the .ino overwrites the pref).
        //   * on the CURRENTLY-armed one → CLEAR the bootscript (pref
        //     wiped, no script fires on next trigger).
        // If neither autorun nor autostart is enabled in Settings, tap
        // toasts "not enabled".
        lv_obj_t* bootBtn = lv_btn_create(row);
        lv_obj_set_size(bootBtn, 52, 52);
        uint32_t bootColor = isAutostart ? 0xE5A93A : 0x2A2A30;  // orange when armed, dark-gray otherwise
        lv_obj_set_style_bg_color(bootBtn, lvhex(bootColor), 0);
        lv_obj_set_style_bg_color(bootBtn, lvhex(bootColor), LV_STATE_PRESSED);
        lv_obj_set_style_bg_opa(bootBtn, LV_OPA_COVER, 0);
        lv_obj_set_style_radius(bootBtn, 8, 0);
        lv_obj_set_style_shadow_width(bootBtn, 0, 0);
        killWidgetAnims(bootBtn);
        // user_data is the raw nameCstr pointer — no bit-tagging. The
        // earlier version OR'd 0x1 into the pointer's LSB to carry an
        // "is currently armed" flag, which corrupted the pointer when
        // std::string's internal buffer wasn't even-byte-aligned. Use
        // the file-scope s_currentAutostart String to test armed state
        // at click time instead.
        lv_obj_add_event_cb(bootBtn, [](lv_event_t* e){
            const char* name = (const char*)lv_event_get_user_data(e);
            if (!name) return;
            bool wasArmed = (s_currentAutostart.length() > 0 &&
                             s_currentAutostart == name);
            bool autorunEnabled = s_settingSw[SET_AUTOSTART] &&
                lv_obj_has_state(s_settingSw[SET_AUTOSTART], LV_STATE_CHECKED);
            bool autostartOnAttach = s_settingSw[SET_AUTOATTACH] &&
                lv_obj_has_state(s_settingSw[SET_AUTOATTACH], LV_STATE_CHECKED);
            if (!autorunEnabled && !autostartOnAttach) {
                watchUiFlash("Enable Autorun or Autostart first");
                return;
            }
            s_pendingAction.has_toggle_autostart = true;
            if (wasArmed) {
                s_pendingAction.autostart_name[0] = '\0';
                watchUiFlash("Bootscript cleared");
            } else {
                strncpy(s_pendingAction.autostart_name, name,
                        sizeof(s_pendingAction.autostart_name) - 1);
                s_pendingAction.autostart_name[sizeof(s_pendingAction.autostart_name) - 1] = '\0';
                watchUiFlash(LV_SYMBOL_OK "  Bootscript set");
            }
        }, LV_EVENT_CLICKED, (void*)nameCstr);
        lv_obj_t* bootLbl = lv_label_create(bootBtn);
        lv_label_set_text(bootLbl, LV_SYMBOL_UPLOAD);
        lv_obj_set_style_text_color(bootLbl,
            isAutostart ? lv_color_white() : lvhex(C_MUTED), 0);
        lv_obj_set_style_text_font(bootLbl, &lv_font_montserrat_22, 0);
        lv_obj_center(bootLbl);

        // ▶ Run — green square
        lv_obj_t* runBtn = lv_btn_create(row);
        lv_obj_set_size(runBtn, 52, 52);
        lv_obj_set_style_bg_color(runBtn, lvhex(C_OK), 0);
        lv_obj_set_style_radius(runBtn, 8, 0);
        lv_obj_set_style_shadow_width(runBtn, 0, 0);
        lv_obj_set_style_margin_left(runBtn, 6, 0);
        killWidgetAnims(runBtn);
        lv_obj_add_event_cb(runBtn, filesRunCb, LV_EVENT_CLICKED, (void*)nameCstr);
        lv_obj_t* runLbl = lv_label_create(runBtn);
        lv_label_set_text(runLbl, LV_SYMBOL_PLAY);
        lv_obj_set_style_text_color(runLbl, lv_color_white(), 0);
        lv_obj_set_style_text_font(runLbl, &lv_font_montserrat_22, 0);
        lv_obj_center(runLbl);

        // 🗑 Delete — red square, rightmost
        lv_obj_t* delBtn = lv_btn_create(row);
        lv_obj_set_size(delBtn, 52, 52);
        lv_obj_set_style_bg_color(delBtn, lvhex(C_DANGER), 0);
        lv_obj_set_style_radius(delBtn, 8, 0);
        lv_obj_set_style_shadow_width(delBtn, 0, 0);
        lv_obj_set_style_margin_left(delBtn, 6, 0);
        killWidgetAnims(delBtn);
        lv_obj_add_event_cb(delBtn, filesDelCb, LV_EVENT_CLICKED, (void*)nameCstr);
        lv_obj_t* delLbl = lv_label_create(delBtn);
        lv_label_set_text(delLbl, LV_SYMBOL_TRASH);
        lv_obj_set_style_text_color(delLbl, lv_color_white(), 0);
        lv_obj_set_style_text_font(delLbl, &lv_font_montserrat_22, 0);
        lv_obj_center(delLbl);
    }
}

// Sync switches without firing our event handler (would re-queue a pending
// change and loop forever). LVGL clears the checked state via lv_obj_add/
// clear_state directly — LV_EVENT_VALUE_CHANGED only fires on interactive
// toggles, so this is safe.
static void syncSw(int idx, bool on) {
    if (idx < 0 || idx >= SET_COUNT) return;
    if (!s_settingSw[idx]) return;
    bool was = lv_obj_has_state(s_settingSw[idx], LV_STATE_CHECKED);
    if (was == on) return;
    if (on) lv_obj_add_state (s_settingSw[idx], LV_STATE_CHECKED);
    else    lv_obj_clear_state(s_settingSw[idx], LV_STATE_CHECKED);
}

void watchUiRefreshSettings(bool wifi, bool bt, bool btdisc,
                            bool led,  bool silent, bool logging, bool com) {
    syncSw(SET_WIFI,    wifi);
    syncSw(SET_BT,      bt);
    syncSw(SET_BTDISC,  btdisc);
    syncSw(SET_LED,     led);
    syncSw(SET_SILENT,  silent);
    syncSw(SET_LOGGING, logging);
    syncSw(SET_COM,     com);
}

void watchUiSetAutostartToggle(bool on) {
    syncSw(SET_AUTOSTART, on);
}
void watchUiSetAutoattachToggle(bool on) {
    syncSw(SET_AUTOATTACH, on);
}

bool watchUiVbusPresent() {
    // Bug-hunt: if the PMU wasn't up on the first watchUiBegin() attempt
    // (I2C not warmed up yet, AXP2101 slow to respond), a caller polling
    // for VBUS here would just get `false` forever until something else
    // happened to call tryInitPMU() again. Retry the init lazily right
    // in this hot path — tryInitPMU() has its own 2 s throttle so this
    // is cheap.
    if (!s_pmuReady) tryInitPMU();
    if (!s_pmuReady) return false;
    // Two-signal check. Some AXP2101 revisions clear isVbusIn under
    // specific charge-state conditions but still report the actual
    // voltage over 4 V.
    if (s_pmu.isVbusIn()) return true;
    uint16_t mv = s_pmu.getVbusVoltage();
    return mv > 4000;
}

void watchUiApplyBrightness(int pct) {
    if (pct < 5)   pct = 5;
    if (pct > 100) pct = 100;
    // Screen.setBrightness now expects percent (0..100) directly — it's
    // the software overlay from QWavey/AMOLEDBrightness, not MIPI DCS.
    Screen.setBrightness((uint8_t)pct);
}

void watchUiRefreshExtras(bool screen_sleep_on, int brightness_pct,
                          const char* pin_value, const char* duress_value) {
    s_screenSleepOn = screen_sleep_on;
    syncSw(SET_SCREEN_SLEEP, screen_sleep_on);
    if (brightness_pct < 10)  brightness_pct = 10;
    if (brightness_pct > 100) brightness_pct = 100;
    s_brightnessPct = brightness_pct;
    if (s_brightVal) {
        char buf[8];
        snprintf(buf, sizeof(buf), "%d%%", s_brightnessPct);
        lv_label_set_text(s_brightVal, buf);
    }
    strncpy(s_pinValue, pin_value ? pin_value : "", sizeof(s_pinValue) - 1);
    s_pinValue[sizeof(s_pinValue) - 1] = '\0';
    strncpy(s_duressValue, duress_value ? duress_value : "", sizeof(s_duressValue) - 1);
    s_duressValue[sizeof(s_duressValue) - 1] = '\0';
}

// Enter a full-black failsafe screen — used when the "Brick Firmware"
// action fires. Deletes every widget on scr and every top-layer overlay,
// paints a black lv_obj covering the whole panel, disables touch input
// so the ROM bootloader can be reached only via BOOT+RESET.
void watchUiEnterBrickBlackscreen() {
    // Latch bricked FIRST so any GESTURE dispatched by lv_task_handler()
    // during/after the clean below early-outs of showTab / showClock /
    // showPinPad and cannot re-materialize widgets on the black failsafe.
    s_bricked = true;
    // Wipe the active screen — no toasts, no clock, no tabs.
    lv_obj_clean(lv_scr_act());
    lv_obj_clean(lv_layer_top());
    // Cover with a pure-black opaque rect that ignores touch.
    lv_obj_t* black = lv_obj_create(lv_scr_act());
    lv_obj_remove_style_all(black);
    lv_obj_set_size(black, LCD_W, LCD_H);
    lv_obj_set_pos(black, 0, 0);
    lv_obj_set_style_bg_color(black, lv_color_black(), 0);
    lv_obj_set_style_bg_opa(black, LV_OPA_COVER, 0);
    lv_obj_clear_flag(black, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_clear_flag(black, LV_OBJ_FLAG_CLICKABLE);
    lv_obj_invalidate(lv_scr_act());
    // Null all previously-tracked handles so later code paths that touch
    // s_scriptLbl / s_bannerLbl / etc. skip cleanly.
    s_statusLbl = nullptr; s_scriptLbl = nullptr; s_progress = nullptr;
    s_apSsidLbl = nullptr; s_apPassLbl = nullptr; s_ipLbl = nullptr;
    s_clientsLbl = nullptr; s_battLbl = nullptr; s_flashLbl = nullptr;
    s_bannerLbl = nullptr; s_runBanner = nullptr; s_runBannerLbl = nullptr;
    s_stopBtn = nullptr; s_playBtn = nullptr;
    s_playLbl = nullptr; s_selectedLbl = nullptr; s_filesList = nullptr;
    s_filesEmpty = nullptr; s_lanStatusLbl = nullptr; s_brightVal = nullptr;
    s_clockRoot = nullptr; s_clockTime = nullptr; s_clockHint = nullptr;
    s_tabHome = nullptr; s_tabFiles = nullptr; s_tabSet = nullptr;
    s_tabView = nullptr;
    for (int i = 0; i < 3; i++) { s_manualLbls[i] = nullptr; s_manualUnder[i] = nullptr; }
    for (int i = 0; i < SETTINGS_PAGE_COUNT; i++) { s_settingsPages[i] = nullptr; s_settingsDots[i] = nullptr; }
    for (int i = 0; i < SET_COUNT; i++) s_settingSw[i] = nullptr;
}

// idle=0 / running=1 / paused=2 — controls dim on Stop and Play icon.
void watchUiSetScriptState(int state) {
    s_scriptState = state;
    if (s_stopBtn) {
        lv_obj_set_style_bg_opa(s_stopBtn,
            state == 0 ? LV_OPA_40 : LV_OPA_COVER, 0);
        // Force invalidate — LVGL doesn't always re-render on a
        // state-selector-0 style change during a running frame, so the
        // wearer sometimes saw the Stop button stay dim even after the
        // script started. Explicit invalidate schedules a redraw.
        lv_obj_invalidate(s_stopBtn);
    }
    if (s_playBtn) {
        uint32_t bg = (state == 1) ? 0xE5A93A : C_OK;
        const char* icon = (state == 1) ? LV_SYMBOL_PAUSE : LV_SYMBOL_PLAY;
        lv_obj_set_style_bg_color(s_playBtn, lvhex(bg), 0);
        lv_obj_set_style_bg_color(s_playBtn, lvhex(bg), LV_STATE_PRESSED);
        if (s_playLbl) lv_label_set_text(s_playLbl, icon);
        lv_obj_invalidate(s_playBtn);
    }
    // Persistent top-of-screen banner visible from ANY tab. Only shown
    // while state != 0. On state 0 hide + rearm.
    if (s_runBanner && s_runBannerLbl) {
        if (state == 0) {
            lv_obj_add_flag(s_runBanner, LV_OBJ_FLAG_HIDDEN);
        } else {
            char buf[96];
            const char* name = s_selectedName[0] ? s_selectedName : "script";
            if (state == 1) {
                snprintf(buf, sizeof(buf), LV_SYMBOL_PLAY "  RUNNING  %s", name);
                lv_obj_set_style_bg_color(s_runBanner, lvhex(0xE5A93A), 0);
            } else {
                snprintf(buf, sizeof(buf), LV_SYMBOL_PAUSE "  PAUSED  %s", name);
                lv_obj_set_style_bg_color(s_runBanner, lvhex(C_ACCENT), 0);
            }
            lv_label_set_text(s_runBannerLbl, buf);
            lv_obj_clear_flag(s_runBanner, LV_OBJ_FLAG_HIDDEN);
            lv_obj_move_foreground(s_runBanner);
        }
        lv_obj_invalidate(s_runBanner);
    }
    // Persistent status label — replaces the 4-second toast so the
    // wearer sees "RUNNING xyz.txt" for the entire lifetime of the
    // script, not just the first four seconds. State 0 restores the
    // idle "Ready" line.
    if (s_statusLbl) {
        char buf[96];
        const char* name = s_selectedName[0] ? s_selectedName : "script";
        if (state == 1) {
            snprintf(buf, sizeof(buf), LV_SYMBOL_PLAY "  RUNNING  %s", name);
            lv_label_set_text(s_statusLbl, buf);
            lv_obj_set_style_text_color(s_statusLbl, lvhex(0xE5A93A), 0);
        } else if (state == 2) {
            snprintf(buf, sizeof(buf), LV_SYMBOL_PAUSE "  PAUSED  %s", name);
            lv_label_set_text(s_statusLbl, buf);
            lv_obj_set_style_text_color(s_statusLbl, lvhex(C_ACCENT), 0);
        } else {
            lv_label_set_text(s_statusLbl, "Ready");
            lv_obj_set_style_text_color(s_statusLbl, lvhex(C_TEXT), 0);
        }
    }
    lv_obj_invalidate(lv_scr_act());
}

void watchUiSetSelectedScript(const char* name) {
    strncpy(s_selectedName, name ? name : "", sizeof(s_selectedName) - 1);
    s_selectedName[sizeof(s_selectedName) - 1] = '\0';
    // Bug-hunt R2: the .ino keeps its OWN copy of the selection in
    // `selectedScriptName` (only that copy is used when Play fires), and
    // its ONLY assignment path is through the WatchUiPendingSettings bridge
    // — has_selected + selected_name. Nothing in this file was setting
    // those, so Play always toasted "No script selected". Queue the bridge
    // fields alongside the local cache so both stay in sync.
    s_pending.has_selected = true;
    strncpy(s_pending.selected_name, s_selectedName,
            sizeof(s_pending.selected_name) - 1);
    s_pending.selected_name[sizeof(s_pending.selected_name) - 1] = '\0';
    if (!s_selectedLbl) return;
    if (!*s_selectedName) {
        lv_label_set_text(s_selectedLbl, "");
    } else {
        char buf[96];
        snprintf(buf, sizeof(buf), LV_SYMBOL_FILE "  %s", s_selectedName);
        lv_label_set_text(s_selectedLbl, buf);
    }
}

void watchUiSetDeadnetToggle(bool on) {
    syncSw(SET_DEADNET, on);
}

void watchUiSetLanConnected(bool connected, const char* ssid) {
    if (!s_lanStatusLbl) return;
    static char last[64] = "";
    char buf[64];
    if (connected) snprintf(buf, sizeof(buf), "LAN: %s", ssid ? ssid : "(joined)");
    else           snprintf(buf, sizeof(buf), "LAN: not connected");
    if (strncmp(last, buf, sizeof(last) - 1) == 0) return;
    strncpy(last, buf, sizeof(last) - 1); last[sizeof(last) - 1] = '\0';
    lv_label_set_text(s_lanStatusLbl, buf);
    lv_obj_set_style_text_color(s_lanStatusLbl, lvhex(connected ? C_OK : C_DANGER), 0);
}

WatchUiPendingExtras watchUiConsumePendingExtras() {
    WatchUiPendingExtras p = s_pendingExtras;
    s_pendingExtras = WatchUiPendingExtras{};
    return p;
}

// ---- First-boot walkthrough overlay ----------------------------------------
// A three-slide modal that covers the tabview. Same restrained palette as the
// Settings tab — no gradient, no glow. Slides:
//   0. Welcome — what this device is
//   1. Dashboard — SSID / PSK / URL, tap to switch tabs
//   2. STOP — how the button works, safety cue
// Progress dots at the bottom-centre, Skip button top-right, primary CTA
// full-width on the bottom.
static lv_obj_t* s_wtRoot        = nullptr;
static lv_obj_t* s_wtHeadline    = nullptr;
static lv_obj_t* s_wtBody        = nullptr;
static lv_obj_t* s_wtDots[3]     = { nullptr, nullptr, nullptr };
static lv_obj_t* s_wtNextBtn     = nullptr;
static lv_obj_t* s_wtNextLbl     = nullptr;
static int       s_wtSlide       = 0;
static char      s_wtSsid[48]    = "";
static char      s_wtPsk[48]     = "";
static char      s_wtIp[24]      = "";
static WatchUiWalkthroughDone s_wtDoneCb = nullptr;

static void wtSetSlide(int idx);
static void wtRender();

static void wtDismiss() {
    if (!s_wtRoot) return;
    lv_obj_del(s_wtRoot);
    s_wtRoot = nullptr;
    if (s_wtDoneCb) { s_wtDoneCb(); s_wtDoneCb = nullptr; }
}

static void wtNextCb(lv_event_t*) {
    if (s_wtSlide >= 2) wtDismiss();
    else                wtSetSlide(s_wtSlide + 1);
}
static void wtSkipCb(lv_event_t*) { wtDismiss(); }

static void wtSetSlide(int idx) {
    s_wtSlide = idx;
    for (int i = 0; i < 3; ++i) {
        if (!s_wtDots[i]) continue;
        lv_obj_set_style_bg_color(s_wtDots[i],
            lvhex(i == idx ? C_ACCENT : C_LINE), 0);
    }
    wtRender();
}

static void wtRender() {
    if (!s_wtHeadline || !s_wtBody || !s_wtNextLbl) return;
    switch (s_wtSlide) {
        case 0:
            lv_label_set_text(s_wtHeadline, "Welcome");
            lv_label_set_text_fmt(s_wtBody,
                "This is your ESP-Watch-BadUSB.\n\n"
                "USB HID + web-driven BadUSB in a wrist form-factor. "
                "Tap through in a second to see how it's laid out.");
            lv_label_set_text(s_wtNextLbl, "Next  " LV_SYMBOL_RIGHT);
            break;
        case 1:
            lv_label_set_text(s_wtHeadline, "Dashboard");
            lv_label_set_text_fmt(s_wtBody,
                "Join the watch's WiFi and open the dashboard.\n\n"
                LV_SYMBOL_WIFI "  %s\n"
                "PSK: %s\n\n"
                "%s",
                s_wtSsid, s_wtPsk, s_wtIp);
            lv_label_set_text(s_wtNextLbl, "Next  " LV_SYMBOL_RIGHT);
            break;
        case 2:
            lv_label_set_text(s_wtHeadline, "STOP button");
            lv_label_set_text_fmt(s_wtBody,
                "The red button on the Home tab kills any running "
                "DuckyScript instantly. Same effect as a short-press on "
                "the BOOT button.\n\n"
                "The " LV_SYMBOL_SETTINGS " Settings tab toggles WiFi, "
                "Bluetooth, LED, silent boot and more.");
            lv_label_set_text(s_wtNextLbl, LV_SYMBOL_OK "  Got it");
            break;
    }
}

bool watchUiWalkthroughActive() { return s_wtRoot != nullptr; }

void watchUiShowWalkthrough(const char* ssid, const char* psk, const char* ip,
                            WatchUiWalkthroughDone onDoneCb) {
    if (s_wtRoot) return;   // already shown
    s_wtDoneCb = onDoneCb;
    strncpy(s_wtSsid, ssid ? ssid : "(none)",     sizeof(s_wtSsid) - 1);
    strncpy(s_wtPsk,  psk  ? psk  : "(open)",     sizeof(s_wtPsk)  - 1);
    strncpy(s_wtIp,   ip   ? ip   : "http://192.168.4.1", sizeof(s_wtIp) - 1);
    s_wtSsid[sizeof(s_wtSsid) - 1] = '\0';
    s_wtPsk [sizeof(s_wtPsk)  - 1] = '\0';
    s_wtIp  [sizeof(s_wtIp)   - 1] = '\0';

    // Full-screen modal. Sits on top layer so it hides the tabview cleanly.
    s_wtRoot = lv_obj_create(lv_layer_top());
    lv_obj_remove_style_all(s_wtRoot);
    lv_obj_set_size(s_wtRoot, LCD_W, LCD_H);
    lv_obj_set_pos(s_wtRoot, 0, 0);
    lv_obj_set_style_bg_color(s_wtRoot, lvhex(C_BG), 0);
    lv_obj_set_style_bg_opa(s_wtRoot, LV_OPA_COVER, 0);
    lv_obj_set_style_pad_all(s_wtRoot, 20, 0);
    lv_obj_clear_flag(s_wtRoot, LV_OBJ_FLAG_SCROLLABLE);

    // Top-right Skip button.
    lv_obj_t* skip = lv_btn_create(s_wtRoot);
    lv_obj_set_size(skip, 90, 40);
    lv_obj_align(skip, LV_ALIGN_TOP_RIGHT, 0, 0);
    lv_obj_set_style_bg_color(skip, lvhex(C_SURFACE), 0);
    lv_obj_set_style_radius(skip, 8, 0);
    lv_obj_set_style_shadow_width(skip, 0, 0);
    killWidgetAnims(skip);
    lv_obj_add_event_cb(skip, wtSkipCb, LV_EVENT_CLICKED, nullptr);
    lv_obj_t* sk = lv_label_create(skip);
    lv_label_set_text(sk, "Skip");
    lv_obj_set_style_text_color(sk, lvhex(C_MUTED), 0);
    lv_obj_set_style_text_font(sk, &lv_font_montserrat_16, 0);
    lv_obj_center(sk);

    // Headline
    s_wtHeadline = lv_label_create(s_wtRoot);
    lv_obj_set_style_text_color(s_wtHeadline, lvhex(C_TEXT), 0);
    lv_obj_set_style_text_font(s_wtHeadline, &lv_font_montserrat_32, 0);
    lv_obj_align(s_wtHeadline, LV_ALIGN_TOP_LEFT, 0, 70);

    // Body
    s_wtBody = lv_label_create(s_wtRoot);
    lv_obj_set_style_text_color(s_wtBody, lvhex(C_MUTED), 0);
    lv_obj_set_style_text_font(s_wtBody, &lv_font_montserrat_18, 0);
    lv_obj_set_width(s_wtBody, LCD_W - 40);
    lv_label_set_long_mode(s_wtBody, LV_LABEL_LONG_WRAP);
    lv_obj_align(s_wtBody, LV_ALIGN_TOP_LEFT, 0, 130);

    // Progress dots (three)
    lv_obj_t* dotRow = lv_obj_create(s_wtRoot);
    lv_obj_remove_style_all(dotRow);
    lv_obj_set_size(dotRow, LV_SIZE_CONTENT, 20);
    lv_obj_align(dotRow, LV_ALIGN_BOTTOM_MID, 0, -110);
    lv_obj_set_flex_flow(dotRow, LV_FLEX_FLOW_ROW);
    lv_obj_set_style_pad_gap(dotRow, 10, 0);
    for (int i = 0; i < 3; ++i) {
        lv_obj_t* d = lv_obj_create(dotRow);
        lv_obj_remove_style_all(d);
        lv_obj_set_size(d, 10, 10);
        lv_obj_set_style_radius(d, LV_RADIUS_CIRCLE, 0);
        lv_obj_set_style_bg_color(d, lvhex(C_LINE), 0);
        lv_obj_set_style_bg_opa(d, LV_OPA_COVER, 0);
        s_wtDots[i] = d;
    }

    // Primary CTA — Next / Got it
    s_wtNextBtn = lv_btn_create(s_wtRoot);
    lv_obj_set_size(s_wtNextBtn, LCD_W - 40, 68);
    lv_obj_align(s_wtNextBtn, LV_ALIGN_BOTTOM_MID, 0, -20);
    lv_obj_set_style_bg_color(s_wtNextBtn, lvhex(C_ACCENT), 0);
    lv_obj_set_style_radius(s_wtNextBtn, 12, 0);
    lv_obj_set_style_shadow_width(s_wtNextBtn, 0, 0);
    killWidgetAnims(s_wtNextBtn);
    lv_obj_add_event_cb(s_wtNextBtn, wtNextCb, LV_EVENT_CLICKED, nullptr);
    s_wtNextLbl = lv_label_create(s_wtNextBtn);
    lv_obj_set_style_text_color(s_wtNextLbl, lv_color_white(), 0);
    lv_obj_set_style_text_font(s_wtNextLbl, &lv_font_montserrat_20, 0);
    lv_obj_center(s_wtNextLbl);

    wtSetSlide(0);
}

#include "GlobalState.h"

// Hardware instances
GpioLed pixels(NUMPIXELS, LED_PIN);
WebServer server(80);
USBHIDKeyboard keyboard;
Preferences preferences;

// WiFi & AP
String ap_ssid = DEFAULT_AP_SSID;
String ap_password = DEFAULT_AP_PASSWORD;
int wifiScanTime = WIFI_SCAN_TIMEOUT;
bool autoConnectEnabled = false;
bool saveOnConnectEnabled = false;
bool silentStartup = false;
bool hidConnected = true;
bool usbStarted = false;

bool updatePackageReady = false;
int  updateProgress = 0;
String updateStatus = "";
bool updateApplying = false;
// v4.4: deferred /execute handoff. See /execute handler + loop() dispatch.
String pendingScript = "";
bool   pendingScriptReady = false;
int buttonPin = 0;
bool buttonState = false;

// Scripting & Execution
std::vector<String> availableLanguages;
std::vector<String> availableScripts;
std::map<String, String> currentKeymap;
String currentLanguage = "us";
int defaultDelay = 0;
int delayBetweenKeys = 0;
std::map<String, String> variables;
// v4.23 (bug-hunt HIGH #2): script-scoped BUTTON_DEF flag. Reset at each
// executeScript() entry so a stopped script's leftover BUTTON_DEF doesn't
// swallow the next script's lines.
bool g_buttonDefActive = false;
// v4.34 bug-hunt CRITICAL #1: ATTACKMODE composition-change reboot flag.
// AttackMode sets it; DuckyInterpreter's main executor sees it after the
// current line, persists the remaining lines to /temp_resume.txt, then
// reboots. Avoids losing the rest of a payload after ATTACKMODE flips
// HID/STORAGE mid-script.
volatile bool g_composeRebootPending = false;
// v4.35: top-level script text captured at every executeScript(depth=0)
// entry. Used ONLY by the ATTACKMODE reboot-persist path so the resume
// file contains the WHOLE user script (not the internal preprocessed
// lines[i+1..], which - if ATTACKMODE fires inside a FUNCTION body -
// would resume mid-function without its declaration).
String g_currentTopScript = "";
String lastCommand = "";
bool scriptRunning = false;
bool stopRequested = false;
bool bootModeEnabled = false;
String bootScript = "";
std::vector<String> currentBootScriptFiles;
unsigned long lastExecutionTime = 0;
int executionDelay = 0;
unsigned long scriptStartTime = 0;
int currentLineNum = 0;
bool holdTillStringActive = false;
RowerState rower = {{}, 0, false};
std::map<String, String> automationRules;

// LED State
bool ledEnabled = true;
bool blinkingEnabled = false;
unsigned long lastBlinkTime = 0;
bool blinkState = false;
int blinkInterval = 100;
int ledMode = 0; // 0=idle, 1=running, 2=error, 3=completion, 4=warning
uint8_t currentR = 0, currentG = 255, currentB = 0;
int completionBlinkCount = 0;
unsigned long lastCompletionBlinkTime = 0;
bool completionBlinkState = false;

// Conditional Execution
bool skipConditionalBlock = false;
String skipUntilSSID = "";

// File System
bool sdCardPresent = true;
unsigned long lastSDCheck = 0;
String currentDirectory = "/";
String copiedFilePath = "";
String cutFilePath = "";
bool fileCopied = false;
bool fileCut = false;
std::vector<String> selectedFiles;

// Logging & History
bool loggingEnabled = false;
bool logFileOpen = false;
File logFile;
std::vector<String> commandHistory;

// Statistics & Monitoring
unsigned long lastStatusUpdate = 0;
int errorCount = 0;
String lastError = "";
unsigned long totalScriptsExecuted = 0;
unsigned long totalCommandsExecuted = 0;
String detectedOS = "Unknown";

// File Upload
File uploadFile;
String uploadFilename = "";

std::vector<BackgroundTask> activeTasks;
int nextTaskId = 1;

bool wifiToggleEnabled = true;
bool bluetoothToggleEnabled = false;
bool btDiscoveryEnabled = false;
String bluetoothName = "ESP32-S3";
bool wifiJoining = false;
unsigned long wifiJoinStartTime = 0;
String current_sta_ssid = "";
String current_sta_password = "";

USBConfig currentUSBConfig;

// Delay Progress Tracking
unsigned long currentDelayTotal = 0;
unsigned long currentDelayStart = 0;

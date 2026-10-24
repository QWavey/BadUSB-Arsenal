#include "WiFiManager.h"
#include "LogManager.h"
#include "BTManager.h"
#include "FSManager.h"           // replaceFileOnSD, jsonEscapeStr
#include "WatchUI.h"             // watchUiFlash: shows an AP start failure on the watch
#include <string.h>              // strlen
#include <HTTPClient.h>
#include <DNSServer.h>
#include <esp_wifi.h>   // v4.12: esp_wifi_set_mac for Randomize-MAC on STA join

// Captive portal DNS: answer every query with the AP's own IP so joining
// clients (Windows/Android/iOS/macOS) auto-open the dashboard.
static DNSServer captiveDNS;
static bool      captivePortalUp = false;

// ============================================================
// AP Management
// ============================================================
void setupAP() {
  // Watch-port setup — using the EXACT AP recipe from
  // ESP-32-S3-devkitc-BadUSB (older branch), the code we verified works
  // on this ESP32-S3 with modern phones. Every richer setup we tried
  // (WIFI_AP_STA + setTxPower + setSleep + channel/hidden/max args)
  // produced AP_STOP bounces or auth failures. Vanilla wins.
  //
  // Bug-hunt round 9 fix: if captive DNS is already up (e.g. a /api/
  // toggle-wifi off→on cycle without an intermediate stopAP, or a
  // factory reset that re-runs setupAP), calling captiveDNS.start(53,...)
  // again tries to bind an already-open UDP socket. DNSServer silently
  // fails, we leak the second attempt and never re-arm the flag. Stop
  // the previous instance first so start() sees a fresh socket.
  if (captivePortalUp) {
    captiveDNS.stop();
    captivePortalUp = false;
  }
  WiFi.mode(WIFI_AP);

  // Web-crash mitigation: cap softAP to 4 stations (default). Above that,
  // the ESP32 core WebServer library has been observed to reset the chip
  // when clients churn (phone browser leaves the page → half-open socket
  // never cleaned up, then next connect crashes on WiFiServer::available()).
  // Also raise the beacon interval slightly and use a fixed channel so the
  // ROM's AP task doesn't roam.
  // Fail closed: never fall back to an open network. A passphrase outside the
  // WPA2 range, or one softAP rejects, leaves the AP off and sets lastError
  // (shown in the dashboard stats) instead of opening the network.
  if (ap_password.length() < 8 || ap_password.length() > 63) {
    Serial.println("[WiFi] AP NOT started: passphrase must be 8-63 characters");
    lastError = "AP not started: passphrase must be 8-63 characters";
    errorCount++;
    watchUiFlash("AP not started: passphrase must be 8-63 chars");
    WiFi.softAPdisconnect(true);
    return;
  }
  if (!WiFi.softAP(ap_ssid.c_str(), ap_password.c_str(), /*channel=*/1,
                   /*ssid_hidden=*/0, /*max_connection=*/4)) {
    Serial.println("[WiFi] AP NOT started: softAP rejected the passphrase - AP stays off");
    lastError = "AP not started: softAP rejected the passphrase";
    errorCount++;
    watchUiFlash("AP not started: check passphrase");
    WiFi.softAPdisconnect(true);
    return;
  }
  // Watch port bug hunt: default modem sleep on the AP path parks the radio
  // between beacons — TCP RTTs balloon and pages "load forever" in the
  // browser. Kill sleep after softAP is up.
  WiFi.setSleep(false);

  IPAddress IP = WiFi.softAPIP();
  Serial.print("[WiFi] AP started. IP: ");
  Serial.println(IP);
  logDebug("AP started, IP: " + IP.toString() + ", SSID: " + ap_ssid + " (Ch 1)");

  // Captive-portal DNS: hijack all queries -> AP IP, so client OSes pop the
  // built-in captive-portal browser straight at http://192.168.4.1/.
  captiveDNS.setErrorReplyCode(DNSReplyCode::NoError);
  if (captiveDNS.start(53, "*", IP)) {
    captivePortalUp = true;
    Serial.println("[WiFi] Captive-portal DNS started on port 53");
  } else {
    Serial.println("[WiFi] Captive-portal DNS failed to start");
  }
}

void loopCaptivePortal() {
  if (captivePortalUp) captiveDNS.processNextRequest();
}

void stopAP() {
  if (captivePortalUp) { captiveDNS.stop(); captivePortalUp = false; }
  WiFi.softAPdisconnect(true);
  Serial.println("[WiFi] AP stopped");
  logDebug("AP stopped");
}

// ============================================================
// Async WiFi Scan — safe to run while AP is active (AP_STA mode)
// ============================================================
// Mirror Wifi-Cloner: use a volatile flag so it's safe across interrupt/loop context
volatile static bool wifiScanStarted = false;
static unsigned long wifiScanStartedAt = 0;

void startWiFiScan() {
  // Guard 1: don't double-start
  if (wifiScanStarted) {
    Serial.println("[WiFi] Scan already flagged in progress, skipping");
    return;
  }
  // Guard 2: check the driver too
  if (WiFi.scanComplete() == WIFI_SCAN_RUNNING) {
    Serial.println("[WiFi] Driver reports scan running, skipping");
    return;
  }
  // Guard 3: BT conflict
  if (btScanInProgress()) {
    Serial.println("[WiFi] BT scan active — deferring WiFi scan");
    return;
  }

  // Allow the Web Server to finish sending the 202 response before we hijack the radio
  delay(200);

  // Watch port: setupAP boots the radio in plain WIFI_AP. Promote to
  // WIFI_AP_STA lazily just before scanning, so the STA netif is
  // available for the scan. (Auto-connect / joinWiFi does the same.)
  if (WiFi.getMode() != WIFI_AP_STA) {
    WiFi.mode(WIFI_AP_STA);
    delay(50);
  }

  // STOP Bluetooth advertising during WiFi scan
  stopBTAdvertising();

  WiFi.scanDelete();
  wifiScanStarted = true;
  wifiScanStartedAt = millis();

  // Trigger PASSIVE scan (quieter) with shorter channel time (110ms)
  // This prevents the AP from being "gone" for too long and dropping clients.
  int result = WiFi.scanNetworks(/*async=*/true, /*show_hidden=*/false, /*passive=*/true, /*max_ms_per_chan=*/110);

  if (result == WIFI_SCAN_FAILED) {
    Serial.println("[WiFi] Failed to start async scan");
    logDebug("WiFi scan failed to start");
    WiFi.scanDelete();
    wifiScanStarted = false;
    startBTAdvertising(); 
    return;
  }

  Serial.println("[WiFi] Async passive scan started");
  logDebug("WiFi async passive scan started");
}

bool wifiScanComplete() {
  return !wifiScanStarted;
}

// Called every loop() tick — collects async scan results without blocking
void pollWiFiScan() {
  if (!wifiScanStarted) return;
  // Watch port perf: was hitting WiFi.scanComplete() every ~1 ms while a
  // scan runs — 1000-3000 driver calls per scan. Gate to 100 ms; well
  // under the startWiFiScan() 200 ms settle and the 15 s timeout.
  static unsigned long s_lastPoll = 0;
  if (millis() - s_lastPoll < 100) return;
  s_lastPoll = millis();

  int n = WiFi.scanComplete();
  if (n == WIFI_SCAN_RUNNING) {
    // Timeout guard — if scan runs >15s something is very wrong
    if (millis() - wifiScanStartedAt > 15000) {
      Serial.println("[WiFi] Scan timeout — resetting");
      logDebug("WiFi scan timeout after 15s");
      WiFi.scanDelete();
      wifiScanStarted = false;
      startBTAdvertising();
    }
    return;
  }

  // Scan finished (n >= 0 or n == WIFI_SCAN_FAILED)
  if (n >= 0) {
    Serial.println("[WiFi] Scan complete — found " + String(n) + " networks");
    logDebug("WiFi scan done, found: " + String(n) + " networks");
  } else {
    Serial.println("[WiFi] Scan failed");
    logDebug("WiFi scan FAILED");
  }

  // Resume Bluetooth after WiFi scan is complete
  startBTAdvertising();
  
  // NOTE: We DON'T call WiFi.scanDelete() here anymore.
  // We keep the results in the driver buffer so they can be read by the API.
  // scanDelete() will be called at the START of the next scan.
  wifiScanStarted = false;
}

void scanWiFi() {
  Serial.println("[WiFi] Blocking scan (during script)...");
  logDebug("WiFi blocking scan started");

  WiFi.scanDelete();
  int n = WiFi.scanNetworks(/*async=*/false);

  if (n == WIFI_SCAN_FAILED || n < 0) {
    Serial.println("[WiFi] Blocking scan failed");
    lastError = "WiFi scan failed";
    errorCount++;
    return;
  }
  Serial.println("[WiFi] Blocking scan done, found " + String(n) + " networks.");
}

bool isSSIDPresent(String ssid) {
  int n = WiFi.scanComplete();
  if (n <= 0) return false;
  for (int i = 0; i < n; i++) {
    if (WiFi.SSID(i) == ssid) return true;
  }
  return false;
}

// ============================================================
// Non-blocking WiFi Join — creates background task
// ============================================================
void joinWiFi(String ssid, String password) {
  if (wifiJoining) {
    Serial.println("[WiFi] Aborting previous join attempt before starting new one");
    WiFi.disconnect(false); // false = keep STA config
    wifiJoining = false;
    // Remove old WIFI_JOINING tasks
    for (auto it = activeTasks.begin(); it != activeTasks.end();) {
      if (it->type == "WIFI_JOINING") it = activeTasks.erase(it);
      else ++it;
    }
  }

  Serial.println("[WiFi] Joining network: " + ssid);
  logDebug("WiFi join started: " + ssid);

  // Watch port: promote to AP_STA lazily (setupAP boots plain WIFI_AP).
  if (WiFi.getMode() != WIFI_AP_STA) {
    WiFi.mode(WIFI_AP_STA);
    delay(50);
  }
  current_sta_ssid = ssid;
  current_sta_password = password;

  // v4.12: Randomize MAC on STA connect if the user opted in (setup wizard
  // or Settings toggle). The upstream router only ever sees this random MAC,
  // so it can't track the ESP across networks by hardware address. We use
  // esp_wifi_set_mac(WIFI_IF_STA, ...) with a locally-administered address
  // (2nd LSB of first byte = 1, LSB = 0 for unicast).
  if (preferences.getBool("random_mac", false)) {
    uint8_t mac[6];
    for (int i = 0; i < 6; i++) mac[i] = (uint8_t)(esp_random() & 0xFF);
    mac[0] = (mac[0] & 0xFE) | 0x02;   // locally-administered, unicast
    esp_err_t r = esp_wifi_set_mac(WIFI_IF_STA, mac);
    if (r == ESP_OK) {
      Serial.printf("[WiFi] Random STA MAC: %02X:%02X:%02X:%02X:%02X:%02X\n",
                    mac[0], mac[1], mac[2], mac[3], mac[4], mac[5]);
    } else {
      Serial.printf("[WiFi] esp_wifi_set_mac failed (%d) - using factory MAC\n", (int)r);
    }
  }

  WiFi.begin(ssid.c_str(), password.c_str());

  wifiJoining = true;
  wifiJoinStartTime = millis();

  BackgroundTask task;
  task.id = nextTaskId++;
  task.description = "Connecting to WiFi: " + ssid;
  task.type = "WIFI_JOINING";
  task.condition = "";
  task.payload = ssid;
  task.active = true;
  activeTasks.push_back(task);
  Serial.println("[WiFi] Join background task created (ID " + String(task.id) + ")");
}

void stopJoiningWiFi() {
  if (!wifiJoining) return;
  WiFi.disconnect(false);
  wifiJoining = false;
  for (auto it = activeTasks.begin(); it != activeTasks.end();) {
    if (it->type == "WIFI_JOINING") it = activeTasks.erase(it);
    else ++it;
  }
  Serial.println("[WiFi] Join aborted by user");
  logDebug("WiFi join aborted by user");
}

void leaveWiFi() {
  current_sta_ssid = "";
  current_sta_password = "";
  WiFi.disconnect(false); // Disconnect STA, keep AP alive
  wifiJoining = false;
  variables["WIFI_CONNECTED"] = "false";
  variables["WIFI_SSID"] = "";
  Serial.println("[WiFi] Disconnected from internet WiFi");
  logDebug("WiFi STA disconnected");
}

// ============================================================
// Utility
// ============================================================
String getTime(String region) {
  if (WiFi.status() != WL_CONNECTED) return "00:00:00";
  
  // Basic offset logic
  long offset = 0;
  if (region.equalsIgnoreCase("de")) offset = 3600; // CET (UTC+1) - neglecting DST for simplicity unless requested
  
  configTime(offset, 0, "pool.ntp.org", "time.nist.gov");
  struct tm timeinfo;
  if (!getLocalTime(&timeinfo)) return "Failed to get time";
  char timeStr[9];
  strftime(timeStr, sizeof(timeStr), "%H:%M:%S", &timeinfo);
  return String(timeStr);
}

String getDay(String region) {
  if (WiFi.status() != WL_CONNECTED) return "Unknown";
  struct tm timeinfo;
  if (!getLocalTime(&timeinfo)) return "Failed to get day";
  char dayStr[10];
  strftime(dayStr, sizeof(dayStr), "%A", &timeinfo); // %A is full weekday name
  return String(dayStr);
}

String makeHttpRequest(String url) {
  if (WiFi.status() != WL_CONNECTED) return "Error: Not connected";
  HTTPClient http;
  http.begin(url);
  int httpCode = http.GET();
  String payload = (httpCode > 0) ? http.getString() : "Error: " + String(httpCode);
  http.end();
  return payload;
}

// ============================================================
// WiFi Credential Management & Auto-Connect
// ============================================================
// On-disk format, one network per line:
//   v2 SSID="<ssid>" PASSWORD="<password>"
// Inside the quotes, backslash and double quote are written with a backslash
// in front, and CR and LF as \r and \n, so any SSID or password round-trips.
// Lines without the "v2 " marker are the old format (no escaping) and are read
// as before, so existing cards keep working.
struct SavedWiFi { String ssid; String pass; };

static String wifiFieldEscape(const String& in) {
    String out;
    out.reserve(in.length() + 4);
    for (size_t i = 0; i < in.length(); ++i) {
        char c = in[i];
        if (c == '\\')      out += "\\\\";
        else if (c == '"')  out += "\\\"";
        else if (c == '\n') out += "\\n";
        else if (c == '\r') out += "\\r";
        else                out += c;
    }
    return out;
}

// Reads the quoted value that follows `key` (for example SSID=") at or after
// index `from`. escaped selects the v2 rules that undo wifiFieldEscape().
// On success `next` is the index just past the closing quote.
static bool readQuotedField(const String& line, const char* key, size_t from,
                            bool escaped, String& value, size_t& next) {
    int start = line.indexOf(key, (unsigned int)from);
    if (start < 0) return false;
    size_t i = (size_t)start + strlen(key);
    value = "";
    while (i < line.length()) {
        char c = line[i];
        if (escaped && c == '\\' && i + 1 < line.length()) {
            char n = line[i + 1];
            if (n == 'n')      value += '\n';
            else if (n == 'r') value += '\r';
            else               value += n;   // \\ and \" (and anything else) stand for themselves
            i += 2;
            continue;
        }
        if (c == '"') { next = i + 1; return true; }
        value += c;
        ++i;
    }
    return false;   // no closing quote
}

static bool parseSavedWiFiLine(String line, SavedWiFi& out) {
    line.trim();
    if (line.length() == 0) return false;
    bool escaped = line.startsWith("v2 ");
    size_t afterSsid = 0;
    if (!readQuotedField(line, "SSID=\"", 0, escaped, out.ssid, afterSsid)) return false;
    // PASSWORD is searched only after the SSID's closing quote, so an SSID that
    // ends in PASSWORD= cannot be mistaken for the password field.
    size_t afterPass = 0;
    if (!readQuotedField(line, "PASSWORD=\"", afterSsid, escaped, out.pass, afterPass)) out.pass = "";
    return true;
}

// Parses /wifi_creds.txt once into a list. Every reader goes through here, so
// the file format is handled in one place.
static std::vector<SavedWiFi> readSavedWiFi() {
    std::vector<SavedWiFi> list;
    if (!sdCardPresent || !SD.exists("/wifi_creds.txt")) return list;
    File f = SD.open("/wifi_creds.txt");
    if (!f) return list;
    while (f.available()) {
        String line = f.readStringUntil('\n');
        SavedWiFi e;
        if (parseSavedWiFiLine(line, e)) list.push_back(e);
    }
    f.close();
    return list;
}

// Looks up the stored password for a saved network. The password itself is
// only used server-side to join; it is never sent to a client.
bool getSavedWiFiPassword(const String& ssid, String& pass) {
    for (const SavedWiFi& e : readSavedWiFi()) {
        if (e.ssid == ssid) { pass = e.pass; return true; }
    }
    return false;
}

void saveWiFiCredentials(String ssid, String pass) {
    if (!sdCardPresent) return;
    
    // Duplicate check against the parsed list. The old check searched JSON text
    // for the file-format string, which never matched, so a network was added
    // again on every save.
    for (const SavedWiFi& e : readSavedWiFi()) {
        if (e.ssid == ssid) {
            Serial.println("[WiFi] Credentials for " + ssid + " already saved.");
            return;
        }
    }

    File f = SD.open("/wifi_creds.txt", FILE_APPEND);
    if (f) {
        f.println("v2 SSID=\"" + wifiFieldEscape(ssid) + "\" PASSWORD=\"" + wifiFieldEscape(pass) + "\"");
        f.close();
        Serial.println("[WiFi] Credentials saved for: " + ssid);
    } else {
        Serial.println("[WiFi] Failed to open /wifi_creds.txt for writing");
    }
}

std::vector<String> getSavedSSIDs() {
    // SSIDs only. Passwords are never returned by any endpoint; they are read
    // server-side through getSavedWiFiPassword() when joining.
    std::vector<String> ssids;
    for (const SavedWiFi& e : readSavedWiFi()) ssids.push_back(e.ssid);
    return ssids;
}

void deleteWiFiCredential(String ssid) {
    if (!sdCardPresent || !SD.exists("/wifi_creds.txt")) return;

    File f = SD.open("/wifi_creds.txt");
    File temp = SD.open("/temp_creds.txt", FILE_WRITE);

    // v4.4: if EITHER handle fails to open, DON'T touch the source file. The
    // previous code plowed on and did SD.remove("/wifi_creds.txt") followed
    // by SD.rename("/temp_creds.txt", "/wifi_creds.txt") — if the temp file
    // never opened, ALL saved WiFi credentials were silently lost.
    if (!f || !temp) {
        if (f) f.close();
        if (temp) temp.close();
        Serial.println("[WiFi] deleteWiFiCredential: could not open working files — aborted (no data lost)");
        return;
    }

    while (f.available()) {
        String line = f.readStringUntil('\n');
        SavedWiFi e;
        // Copy every other line through unchanged; drop only the target network.
        if (!(parseSavedWiFiLine(line, e) && e.ssid == ssid)) {
            temp.println(line);
        }
    }
    f.close();
    temp.close();

    // Swap the new list in. replaceFileOnSD keeps the old list until the new one
    // is in place, and restores it if the rename fails.
    if (!replaceFileOnSD("/temp_creds.txt", "/wifi_creds.txt")) {
        Serial.println("[WiFi] deleteWiFiCredential: replace failed - original list kept, new list left in /temp_creds.txt");
        return;
    }
    Serial.println("[WiFi] Deleted credentials for: " + ssid);
}

void processAutoConnect() {
    static unsigned long lastAutoAttempt = 0;
    static bool scanTriggeredForAutoConnect = false;

    if (!autoConnectEnabled || scriptRunning || wifiJoining || WiFi.status() == WL_CONNECTED) return;

    // Attempt cycle every 60 seconds
    if (millis() - lastAutoAttempt < 60000) return;

    // Phase 1: trigger an async scan and wait for it to complete
    if (!scanTriggeredForAutoConnect) {
        // Only start a scan if one isn't already running
        if (wifiScanComplete()) {
            Serial.println("[WiFi] Auto-connect: Starting async scan for saved networks...");
            logDebug("Auto-connect: triggering async WiFi scan");
            startWiFiScan();
            scanTriggeredForAutoConnect = true;
        }
        return; // Come back next loop tick
    }

    // Phase 2: scan still in progress — wait
    if (!wifiScanComplete()) return;

    // Phase 3: scan done — check results against saved credentials
    scanTriggeredForAutoConnect = false;
    lastAutoAttempt = millis(); // reset timer after full attempt

    int n = WiFi.scanComplete();
    if (n <= 0) {
        Serial.println("[WiFi] Auto-connect: No networks found.");
        return;
    }

    // Parsed list and direct SSID comparison: no JSON round trip, so SSIDs and
    // passwords with quotes or backslashes are matched and joined correctly.
    std::vector<SavedWiFi> saved = readSavedWiFi();
    for (int i = 0; i < n; i++) {
        String netSSID = WiFi.SSID(i);
        for (const SavedWiFi& e : saved) {
            if (e.ssid == netSSID) {
                Serial.println("[WiFi] Auto-connect: Found visible saved network " + netSSID + ". Joining...");
                logDebug("Auto-connect: joining " + netSSID);
                joinWiFi(e.ssid, e.pass);
                return;
            }
        }
    }
}

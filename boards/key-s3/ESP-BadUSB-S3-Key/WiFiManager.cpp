#include "WiFiManager.h"
#include "LogManager.h"
#include "BTManager.h"
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
  // AP_STA mode lets us scan for networks while the AP is running
  WiFi.mode(WIFI_AP_STA);
  delay(100);

  // v4.4 thermal management: this is a USB-stick form factor so the chip is
  // right on the enclosure — every mW of TX power translates to felt heat.
  // 11 dBm (12.5 mW) still reaches ~5 m indoors, which is plenty for a device
  // that's typically 30 cm from the user's phone.
  WiFi.setTxPower(WIFI_POWER_11dBm);
  // Enable modem sleep between beacons — cuts idle radio draw ~40%. STA-side
  // latency stays imperceptible for the dashboard's 5 s polling cadence.
  WiFi.setSleep(WIFI_PS_MIN_MODEM);

  // Fix AP to channel 1. Specify max connections to reduce overhead.
  // max_connection=2 is plenty for a personal tool (was 4).
  if (!WiFi.softAP(ap_ssid.c_str(), ap_password.c_str(), 1, 0, 2)) {
    Serial.println("[WiFi] Failed to setup AP with password — trying open AP");
    WiFi.softAP(ap_ssid.c_str(), "", 1, 0, 2);
  }

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

  // DO NOT call WiFi.mode() — already in AP_STA
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
void saveWiFiCredentials(String ssid, String pass) {
    if (!sdCardPresent) return;
    
    // Check if already exists to avoid duplicates
    String existing = getSavedWiFiCredentials();
    if (existing.indexOf("SSID=\"" + ssid + "\"") != -1) {
        Serial.println("[WiFi] Credentials for " + ssid + " already saved.");
        return;
    }

    File f = SD.open("/wifi_creds.txt", FILE_APPEND);
    if (f) {
        f.println("SSID=\"" + ssid + "\" PASSWORD=\"" + pass + "\"");
        f.close();
        Serial.println("[WiFi] Credentials saved for: " + ssid);
    } else {
        Serial.println("[WiFi] Failed to open /wifi_creds.txt for writing");
    }
}

String getSavedWiFiCredentials() {
    if (!sdCardPresent || !SD.exists("/wifi_creds.txt")) return "[]";
    
    File f = SD.open("/wifi_creds.txt");
    if (!f) return "[]";
    
    String json = "[";
    bool first = true;
    while (f.available()) {
        String line = f.readStringUntil('\n');
        line.trim();
        if (line.length() == 0) continue;
        
        int s1 = line.indexOf("SSID=\"");
        int s2 = line.indexOf("\"", s1 + 6);
        int p1 = line.indexOf("PASSWORD=\"");
        int p2 = line.indexOf("\"", p1 + 10);
        
        if (s1 != -1 && s2 != -1) {
            if (!first) json += ",";
            String ssid = line.substring(s1 + 6, s2);
            String pass = (p1 != -1 && p2 != -1) ? line.substring(p1 + 10, p2) : "";
            json += "{\"ssid\":\"" + ssid + "\",\"pass\":\"" + pass + "\"}";
            first = false;
        }
    }
    f.close();
    json += "]";
    return json;
}

std::vector<String> getSavedSSIDs() {
    std::vector<String> ssids;
    if (!sdCardPresent || !SD.exists("/wifi_creds.txt")) return ssids;
    
    File f = SD.open("/wifi_creds.txt");
    if (!f) return ssids;
    
    while (f.available()) {
        String line = f.readStringUntil('\n');
        line.trim();
        if (line.length() == 0) continue;
        
        int s1 = line.indexOf("SSID=\"");
        int s2 = line.indexOf("\"", s1 + 6);
        
        if (s1 != -1 && s2 != -1) {
            ssids.push_back(line.substring(s1 + 6, s2));
        }
    }
    f.close();
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
        if (line.indexOf("SSID=\"" + ssid + "\"") == -1) {
            temp.println(line);
        }
    }
    f.close();
    temp.close();
    // Only swap into place after we've built a valid temp file.
    SD.remove("/wifi_creds.txt");
    if (!SD.rename("/temp_creds.txt", "/wifi_creds.txt")) {
        Serial.println("[WiFi] deleteWiFiCredential: rename failed — creds file MAY be missing; check /temp_creds.txt");
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

    String credsJson = getSavedWiFiCredentials();
    for (int i = 0; i < n; i++) {
        String netSSID = WiFi.SSID(i);
        if (credsJson.indexOf("\"ssid\":\"" + netSSID + "\"") != -1) {
            int start = credsJson.indexOf("\"ssid\":\"" + netSSID + "\"");
            int passStart = credsJson.indexOf("\"pass\":\"", start) + 8;
            int passEnd = credsJson.indexOf("\"", passStart);
            String pass = credsJson.substring(passStart, passEnd);

            Serial.println("[WiFi] Auto-connect: Found visible saved network " + netSSID + ". Joining...");
            logDebug("Auto-connect: joining " + netSSID);
            joinWiFi(netSSID, pass);
            break;
        }
    }
}

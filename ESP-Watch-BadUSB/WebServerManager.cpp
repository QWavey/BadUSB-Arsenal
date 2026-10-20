#include "WebServerManager.h"
#include "WebServerHandlers.h"
#include "FSManager.h"
#include "DuckyInterpreter.h"
#include "USBManager.h"
#include "UpdateManager.h"
#include "LogManager.h"
#include "LEDManager.h"
#include "WiFiManager.h"
#include "BTManager.h"
#include "AttackMode.h"
#include "MSCManager.h"          // v4.9: mscBaseSector/mscSubSectors for /api/stats
#include "WatchUI.h"             // watchUiFlash / watchUiTick for on-screen feedback
#include "esp32-hal-tinyusb.h"   // usb_persist_restart — clean USB shutdown before reset
#include "deadnet.h"             // DeadNet API endpoints
#include <ArduinoJson.h>

extern Deadnet g_deadnet;

// Watch port: ESP.restart() on the ESP32-S3 with USB-Serial/JTAG can leave
// the ROM stub in a half-init state → chip boots into download mode after
// setup wizard's "reboot" step, forcing the user to unplug/replug USB.
// usb_persist_restart(RESTART_NO_PERSIST) registers the TinyUSB shutdown
// handler first so the host sees a clean unplug before reset — the chip
// then boots the app normally.
static inline void watchSafeRestart() {
  usb_persist_restart(RESTART_NO_PERSIST);
}

void setupWebServer() {
  server.enableCORS(true);

  // API Endpoints
  server.on("/api/change-directory", HTTP_POST, handleChangeDirectory);
  server.on("/api/current-directory", handleGetCurrentDirectory);
  server.on("/api/detect-os", HTTP_POST, handleDetectOS);
  server.on("/api/use-file", HTTP_POST, handleUseFile);
  server.on("/api/copy-file", HTTP_POST, handleCopyFile);
  server.on("/api/cut-file", HTTP_POST, handleCutFile);
  server.on("/api/paste-file", HTTP_POST, handlePasteFile);
  server.on("/api/upload", HTTP_POST, []() {
    server.send(200, "text/plain", "Upload complete: " + uploadFilename);
  }, handleFileUpload);

  // Bundled .espkg update: upload streams to SD, POST finalizer arms the apply,
  // status endpoint reports progress while it writes SD files then OTAs firmware.
  server.on("/api/update-package", HTTP_POST, handleUpdatePackagePost, handleUpdatePackageUpload);
  server.on("/api/update-status", HTTP_GET, handleUpdateStatus);

  server.on("/api/download", handleFileDownload);
  server.on("/api/list-files", handleListFiles);
  server.on("/api/delete-file", HTTP_DELETE, handleDeleteFile);
  server.on("/api/create-directory", HTTP_POST, handleCreateDirectory);
  server.on("/api/file-info", handleFileInfo);

  // Main UI
  // Main UI. If the SD is mounted but the dashboard files aren't installed
  // yet, serve a self-contained fallback page (embedded literal below) that
  // offers to upload an .espkg via the existing /api/update-package endpoint.
  // Prior behaviour was a raw 404 that made a fresh unit look bricked even
  // though the AP was up and the update endpoint was live.
  static const char kSDEmptyPage[] PROGMEM = R"HTML(<!doctype html>
<html lang="en"><head><meta charset="utf-8">
<meta name="viewport" content="width=device-width,initial-scale=1">
<title>Set up this device</title>
<style>
:root{color-scheme:dark;--bg:#0a0a0b;--surface:#141416;--line:#2a2a30;--text:#e8e8ea;--muted:#8a8a90;--accent:#3aa7f5;--danger:#c8442d}
html,body{margin:0;background:var(--bg);color:var(--text);font:16px/1.5 -apple-system,Segoe UI,Roboto,sans-serif}
.wrap{max-width:640px;margin:0 auto;padding:32px 20px}
h1{font-size:28px;margin:0 0 8px;letter-spacing:-0.01em}
.sub{color:var(--muted);margin:0 0 28px}
.card{background:var(--surface);border:1px solid var(--line);border-radius:12px;padding:20px;margin:16px 0}
.card h2{font-size:18px;margin:0 0 8px}
.card p{margin:0 0 12px;color:var(--muted)}
label.file{display:block;border:1px dashed var(--line);border-radius:10px;padding:22px;text-align:center;cursor:pointer;color:var(--muted)}
label.file:hover{color:var(--text);border-color:var(--accent)}
label.file input{display:none}
button{background:var(--accent);color:#fff;border:0;border-radius:10px;padding:12px 18px;font:600 16px/1 inherit;cursor:pointer;width:100%;margin-top:12px}
button[disabled]{opacity:.5;cursor:not-allowed}
.log{background:#000;color:#9ac;font:13px/1.5 ui-monospace,SFMono-Regular,Menlo,monospace;border-radius:10px;padding:12px;min-height:80px;white-space:pre-wrap;margin-top:12px}
.err{color:var(--danger)}
.ok{color:#30d158}
code{background:#000;padding:2px 6px;border-radius:4px;font-size:14px}
</style></head><body><div class="wrap">
<h1>Set up this device</h1>
<p class="sub">The SD card is mounted but doesn&rsquo;t have the dashboard files yet. Upload the update package and this device will finish setting itself up.</p>
<div class="card">
<h2>1&nbsp; Get the package</h2>
<p>Download the latest <code>.espkg</code> from
<a href="https://github.com/QWavey/ESP-S3-Key-BadUSB/releases" style="color:var(--accent)">the ESP-S3-Key releases page</a>
or build one with <code>python tools/build_espkg.py --web &quot;Website [ESP SD]&quot;</code>.</p>
</div>
<div class="card">
<h2>2&nbsp; Upload it</h2>
<label class="file" id="drop"><input type="file" id="pkg" accept=".espkg"><span id="dropText">Drop <code>.espkg</code> here or tap to choose a file</span></label>
<button id="go" disabled>Upload &amp; apply</button>
<div class="log" id="log">Idle.</div>
</div>
<p class="sub">This form posts to <code>/api/update-package</code> &mdash; the same endpoint the full dashboard uses. Nothing leaves this device.</p>
</div>
<script>
const $ = q => document.querySelector(q);
const log = (m, cls) => { const l = $('#log'); const s = document.createElement('div'); if (cls) s.className = cls; s.textContent = m; l.appendChild(s); l.scrollTop = l.scrollHeight; };
$('#pkg').addEventListener('change', e => { const f = e.target.files[0]; if (f) { $('#dropText').textContent = f.name + '  (' + Math.round(f.size/1024) + ' KB)'; $('#go').disabled = false; } });
$('#go').addEventListener('click', async () => {
  const f = $('#pkg').files[0]; if (!f) return;
  $('#go').disabled = true; log('Uploading ' + f.name + '…');
  const fd = new FormData(); fd.append('package', f, f.name);
  try {
    const r = await fetch('/api/update-package', { method: 'POST', body: fd });
    if (!r.ok) throw new Error('HTTP ' + r.status);
    log('Applied. Reloading in a few seconds…', 'ok');
    setTimeout(() => location.reload(), 5000);
  } catch (e) { log('Upload failed: ' + e, 'err'); $('#go').disabled = false; }
});
</script></body></html>)HTML";

  server.on("/", []() {
    if (!sdCardPresent) { server.send(500, "text/plain", "SD Card not present"); return; }
    if (!SD.exists(FILE_INDEX)) {
      // SD is mounted but the dashboard isn't installed — show the setup
      // fallback with an .espkg upload form instead of a raw 404.
      server.sendHeader("Cache-Control", "no-cache, no-store, must-revalidate");
      server.send_P(200, "text/html", kSDEmptyPage);
      return;
    }
    File file = SD.open(FILE_INDEX);
    if (!file) { server.send(500, "text/plain", "Failed to open index.html"); return; }
    server.sendHeader("Cache-Control", "no-cache, no-store, must-revalidate");
    server.streamFile(file, "text/html");
    file.close();
  });

  server.on("/style.css", []() {
    if (!sdCardPresent || !SD.exists("/style.css")) { server.send(404, "text/plain", "style.css not found"); return; }
    File file = SD.open("/style.css");
    server.sendHeader("Cache-Control", "no-cache, no-store, must-revalidate");
    server.streamFile(file, "text/css");
    file.close();
  });

  server.on("/script.js", []() {
    if (!sdCardPresent || !SD.exists("/script.js")) { server.send(404, "text/plain", "script.js not found"); return; }
    File file = SD.open("/script.js");
    server.sendHeader("Cache-Control", "no-cache, no-store, must-revalidate");
    server.streamFile(file, "application/javascript");
    file.close();
  });

  // ---- Captive-portal probe URLs (pop the OS-native browser to /) --------
  auto captiveRedirect = []() {
    server.sendHeader("Location", "http://192.168.4.1/", true);
    server.sendHeader("Cache-Control", "no-store");
    server.send(302, "text/plain", "");
  };
  // Android
  server.on("/generate_204",     captiveRedirect);
  server.on("/gen_204",          captiveRedirect);
  // Windows
  server.on("/ncsi.txt",         captiveRedirect);
  server.on("/connecttest.txt",  captiveRedirect);
  server.on("/redirect",         captiveRedirect);
  // Apple (iOS/macOS)
  server.on("/hotspot-detect.html", captiveRedirect);
  server.on("/library/test/success.html", captiveRedirect);
  // Firefox
  server.on("/success.txt",      captiveRedirect);
  server.on("/canonical.html",   captiveRedirect);

  server.onNotFound([]() {
    String path = server.uri();

    // If the requested Host isn't our IP, the client came in via captive
    // portal DNS -> redirect straight to the dashboard.
    String host = server.hostHeader();
    if (host.length() > 0 && host != "192.168.4.1" && !host.startsWith("192.168.4.1:")) {
      server.sendHeader("Location", "http://192.168.4.1/", true);
      server.sendHeader("Cache-Control", "no-store");
      server.send(302, "text/plain", "");
      return;
    }

    // Unknown /api/* paths must return 404 so XHR errors surface honestly
    // rather than silently redirecting to the dashboard HTML.
    if (path.startsWith("/api/")) { server.send(404, "text/plain", "Not Found"); return; }
    if (!sdCardPresent) { server.send(404, "text/plain", "Not Found"); return; }
    if (SD.exists(path)) {
      File file = SD.open(path);
      server.sendHeader("Cache-Control", "no-cache, no-store, must-revalidate");
      if (path.endsWith(".html")) server.streamFile(file, "text/html");
      else if (path.endsWith(".css")) server.streamFile(file, "text/css");
      else if (path.endsWith(".js")) server.streamFile(file, "application/javascript");
      else server.streamFile(file, "text/plain");
      file.close();
    } else {
      // Unknown path but on our IP -> fall back to the dashboard.
      server.sendHeader("Location", "http://192.168.4.1/", true);
      server.send(302, "text/plain", "");
    }
  });


  server.on("/execute", HTTP_POST, []() {
    if (updateApplying) { server.send(409, "text/plain; charset=utf-8", "Busy: firmware update in progress"); return; }
    String script = server.arg("plain");

    // v4.4 (was v4.3): the previous fix called server.handleClient() from
    // INSIDE this handler to "flush" the response before ESP.restart(). That
    // was reentrant — WebServer's client FSM isn't safe to dispatch into
    // recursively, and it could corrupt the current request. New approach:
    // detect a rebooting first line, hand off to loop() via a flag, and
    // return from the handler cleanly. loop() runs executeScript() the next
    // tick after the FIN has flushed naturally.
    String upperFirstLine;
    {
      int nl = script.indexOf('\n');
      upperFirstLine = (nl < 0 ? script : script.substring(0, nl));
      upperFirstLine.trim();
      upperFirstLine.toUpperCase();
    }
    bool willReboot = upperFirstLine.startsWith("ATTACKMODE") ||
                      upperFirstLine == "REBOOT" ||
                      upperFirstLine.startsWith("REBOOT ") ||
                      upperFirstLine == "SHUTDOWN";
    if (willReboot) {
      // Defer to loop() so the HTTP 200 flushes cleanly first.
      pendingScript = script;
      pendingScriptReady = true;
      server.send(200, "text/plain; charset=utf-8", "OK (queued; reboot may follow)");
      return;
    }
    executeScript(script);
    server.send(200, "text/plain; charset=utf-8", "OK");
  });

  server.on("/stop", HTTP_POST, []() {
    stopRequested = true;
    server.send(200, "text/plain; charset=utf-8", "Stop requested - waiting for current command to finish");
  });

  server.on("/language", []() {
    if (server.hasArg("lang")) {
      String lang = server.arg("lang");
      if (loadLanguage(lang)) {
        // Persist immediately so it survives reboots
        preferences.putString("language", currentLanguage);
        server.send(200, "text/plain; charset=utf-8", "OK");
      } else {
        server.send(400, "text/plain; charset=utf-8", "Language not found");
      }
    } else {
      server.send(400, "text/plain; charset=utf-8", "No language specified");
    }
  });

  server.on("/status", []() {
    String status = "Ready - Language: " + currentLanguage;
    status += " - Scripts: " + String(availableScripts.size());
    status += " - Clients: " + String(WiFi.softAPgetStationNum());
    status += " - Scan Time: " + String(wifiScanTime) + "ms";
    status += " - LED: " + String(ledEnabled ? "ON" : "OFF");
    status += " - Logging: " + String(loggingEnabled ? "ON" : "OFF");
    status += " - SD Card: " + String(sdCardPresent ? "OK" : "REMOVED");
    status += " - Errors: " + String(errorCount);
    status += " - Total Scripts: " + String(totalScriptsExecuted);
    status += " - Total Commands: " + String(totalCommandsExecuted);
    status += " - Detected OS: " + detectedOS;
    status += " - Current Dir: " + currentDirectory;
    if (scriptRunning) status += " - Script Running";
    if (bootModeEnabled) status += " - Boot: " + (currentBootScriptFiles.size() > 0 ? currentBootScriptFiles[0] : "Active");
    if (WiFi.status() == WL_CONNECTED) status += " - WiFi: " + WiFi.SSID();
    server.send(200, "text/plain; charset=utf-8", status);
  });

  server.on("/selfdestruct", HTTP_POST, []() {
    String body = server.arg("plain");
    DynamicJsonDocument doc(256);
    DeserializationError error = deserializeJson(doc, body);

    if (error) {
      server.send(400, "text/plain; charset=utf-8", "Invalid JSON");
      return;
    }

    String password = doc["password"].as<String>();
    if (password == ap_password) {
      server.send(200, "text/plain; charset=utf-8", "Self-destruct initiated");
      delay(1000);
      selfDestruct();
    } else {
      server.send(403, "text/plain; charset=utf-8", "Invalid password");
    }
  });

  server.on("/api/toggle-led", HTTP_POST, []() {
    String body = server.arg("plain");
    DynamicJsonDocument doc(128);
    if (!deserializeJson(doc, body) && doc.containsKey("enabled")) {
      ledEnabled = doc["enabled"];
    } else {
      ledEnabled = !ledEnabled;
    }
    preferences.putBool("led_enabled", ledEnabled);

    if (ledEnabled) {
      setLEDMode(0);
    } else {
      pixels.setPixelColor(0, pixels.Color(0, 0, 0));
      pixels.show();
    }

    DynamicJsonDocument resp(128);
    resp["ledEnabled"] = ledEnabled;
    String response;
    serializeJson(resp, response);
    server.send(200, "application/json", response);
    logDebug("LED " + String(ledEnabled ? "enabled" : "disabled"));
  });

  server.on("/api/toggle-logging", HTTP_POST, []() {
    String body = server.arg("plain");
    DynamicJsonDocument doc(256);
    DeserializationError error = deserializeJson(doc, body);

    if (error) {
      server.send(400, "text/plain; charset=utf-8", "Invalid JSON");
      return;
    }

    loggingEnabled = doc["enabled"];
    preferences.putBool("logging_enabled", loggingEnabled);
    server.send(200, "text/plain; charset=utf-8", "Logging " + String(loggingEnabled ? "enabled" : "disabled"));
  });

  server.on("/api/toggle-wifi", HTTP_POST, []() {
    String body = server.arg("plain");
    DynamicJsonDocument doc(256);
    // v4.11 FIX: guard on containsKey. Previously an empty {} body silently
    // set wifiToggleEnabled = false (ArduinoJson returns null -> bool false)
    // and disabled WiFi + persisted it to NVS.
    if (!deserializeJson(doc, body) && doc.containsKey("enabled")) {
      wifiToggleEnabled = doc["enabled"];
      preferences.putBool("wifi_toggle", wifiToggleEnabled);
      if (wifiToggleEnabled) setupAP(); else stopAP();
    }
    server.send(200, "application/json", "{\"enabled\":" + String(wifiToggleEnabled ? "true" : "false") + "}");
  });

  server.on("/api/toggle-bluetooth", HTTP_POST, []() {
    String body = server.arg("plain");
    DynamicJsonDocument doc(256);
    if (!deserializeJson(doc, body) && doc.containsKey("enabled")) {
      bluetoothToggleEnabled = doc["enabled"];
      preferences.putBool("bt_toggle", bluetoothToggleEnabled);
      if (bluetoothToggleEnabled) setupBT(); else stopBT();
    }
    server.send(200, "application/json", "{\"enabled\":" + String(bluetoothToggleEnabled ? "true" : "false") + "}");
  });

  server.on("/api/toggle-bt-discovery", HTTP_POST, []() {
    String body = server.arg("plain");
    DynamicJsonDocument doc(256);
    if (!deserializeJson(doc, body) && doc.containsKey("enabled")) {
      btDiscoveryEnabled = doc["enabled"];
      preferences.putBool("bt_discovery", btDiscoveryEnabled);
    }
    server.send(200, "application/json", "{\"enabled\":" + String(btDiscoveryEnabled ? "true" : "false") + "}");
  });

  // Legacy /api/toggle-shell + /api/reset-shell-pw fully removed (v4.6).
  // The new PuTTY-over-COM slider uses /api/toggle-com below.

  // v4.4: one-shot fetch of the WiFi passwords for the Settings tab. Not part
  // of /api/stats because that endpoint is polled every 5 s and broadcasted
  // the passwords back to every connected client. Only fetch on tab open.
  server.on("/api/wifi-secrets", []() {
    String out = "{\"apPassword\":\"" + ap_password + "\"" +
                 ",\"staPassword\":\"" + ((WiFi.status() == WL_CONNECTED) ? current_sta_password : String("")) + "\"}";
    server.sendHeader("Cache-Control", "no-cache, no-store, must-revalidate");
    server.send(200, "application/json", out);
  });

  // Reset ATTACKMODE to defaults (HID only, no storage, no size cap) - the
  // way out when a script left the device in a stuck composition.
  server.on("/api/reset-attackmode", HTTP_POST, []() {
    preferences.putBool  ("am_hid",           true);
    preferences.putBool  ("am_msc",           false);
    preferences.putBool  ("am_no_hid_intent", false);
    preferences.putUShort("am_vid",           0x303A);
    preferences.putUShort("am_pid",           0x0002);
    preferences.putULong64("am_size",         0);
    // v4.8: also clear the MSC sub-region so a SIZE_-created hiding window
    // is dropped and MSC exposes the whole SD again on next boot.
    preferences.remove   ("msc_base");
    preferences.remove   ("msc_sect");
    server.send(200, "application/json", "{\"ok\":true,\"status\":\"ATTACKMODE reset - rebooting to rebuild descriptors\"}");
    delay(300);
    watchSafeRestart();
  });

  // Silent Startup (stealth HID): persist + apply immediately (no reboot needed)
  server.on("/api/set-silent", HTTP_POST, []() {
    String body = server.arg("plain");
    DynamicJsonDocument doc(256);
    if (!deserializeJson(doc, body)) {
      silentStartup = doc["enabled"];
      preferences.putBool("silent_boot", silentStartup);
      // Arm the RTC flag so the pre-setup() constructor picks it up on the
      // next boot (kills USB pads ~100 ms earlier than STEP 0 in setup()).
      if (silentStartup) silentArmForNextBoot();
      else               silentClearForNextBoot();
      if (silentStartup) {
        if (usbStarted) hidDetach();    // only unplug if USB is actually running
      } else {
        ensureHidReady();               // start USB if it was deferred at boot, else re-attach
      }
    }
    server.send(200, "application/json", "{\"enabled\":" + String(silentStartup ? "true" : "false") + "}");
  });

  // Live keyboard: type a character / string / special key straight to the host
  server.on("/api/live-type", HTTP_POST, []() {
    if (scriptRunning) { server.send(409, "text/plain; charset=utf-8", "Busy: a script is running"); return; }
    if (updateApplying) { server.send(409, "text/plain; charset=utf-8", "Busy: firmware update in progress"); return; }
    if (!currentAttackMode.hid) { server.send(409, "text/plain; charset=utf-8", "No HID interface (ATTACKMODE disabled it) — run 'ATTACKMODE HID' first"); return; }
    String body = server.arg("plain");
    // v4.4: was DynamicJsonDocument(8192) — 8 KB alloc/free EVERY keystroke
    // in live typing, murdering heap fragmentation. Static reserves it once
    // on the .bss and never touches malloc. 4 KB is still plenty for a
    // "Send All Text" paste (< 3 KB of typical text after JSON overhead).
    StaticJsonDocument<4096> doc;
    if (deserializeJson(doc, body)) { server.send(400, "text/plain", "Invalid JSON"); return; }

    // Turning Live typing off asks us to detach again (when silent-startup is on)
    if (!doc["release"].isNull()) { hidReleaseIfSilent(); server.send(200, "text/plain", "released"); return; }

    ensureHidReady();          // attach once; cheap no-op if already mounted
    stopRequested = false;

    if (!doc["text"].isNull()) {
      fastTypeString(doc["text"].as<String>());
    } else if (!doc["k"].isNull()) {
      fastTypeString(doc["k"].as<String>());
    } else if (!doc["special"].isNull()) {
      String s = doc["special"].as<String>();
      if (s == "ARROW_UP") s = "UP";
      else if (s == "ARROW_DOWN") s = "DOWN";
      else if (s == "ARROW_LEFT") s = "LEFT";
      else if (s == "ARROW_RIGHT") s = "RIGHT";
      fastPressKey(s);
    } else if (!doc["combo"].isNull()) {
      handleKeyInput(doc["combo"].as<String>());
    }
    server.send(200, "text/plain", "ok");
  });


  server.on("/api/save-bt-settings", HTTP_POST, []() {
    String body = server.arg("plain");
    DynamicJsonDocument doc(256);
    if (!deserializeJson(doc, body)) {
      bluetoothName = doc["name"].as<String>();
      preferences.putString("bt_name", bluetoothName);
    }
    server.send(200, "text/plain", "OK");
  });

  server.on("/api/save-usb", HTTP_POST, []() {
    String body = server.arg("plain");
    DynamicJsonDocument doc(512);
    if (!deserializeJson(doc, body)) {
      currentUSBConfig.vid = doc["vid"].as<String>();
      currentUSBConfig.pid = doc["pid"].as<String>();
      currentUSBConfig.rndVid = doc["rndVid"];
      currentUSBConfig.rndPid = doc["rndPid"];
      currentUSBConfig.mfr = doc["mfr"].as<String>();
      currentUSBConfig.prod = doc["prod"].as<String>();

      preferences.putString("usb_vid", currentUSBConfig.vid);
      preferences.putString("usb_pid", currentUSBConfig.pid);
      preferences.putBool("usb_rndVid", currentUSBConfig.rndVid);
      preferences.putBool("usb_rndPid", currentUSBConfig.rndPid);
      preferences.putString("usb_mfr", currentUSBConfig.mfr);
      preferences.putString("usb_prod", currentUSBConfig.prod);
      
      server.send(200, "text/plain", "USB settings saved. Rebooting for changes to take effect...");
      delay(1000);
      // Bug-hunt #9: was plain ESP.restart() — on the S3's USB-Serial/
      // JTAG boot path that leaves the ROM stub half-init and the chip
      // reboots into download mode instead of the app. Use the safe path.
      watchSafeRestart();
    } else {
      server.send(400, "text/plain", "Invalid JSON");
    }
  });

  server.on("/api/stop-ap", HTTP_POST, []() {
    logDebug("HTTP: /api/stop-ap called");
    stopAP();
    server.send(200, "text/plain", "AP stopped. You may lose connection if not connected to another WiFi.");
  });

  server.on("/api/tasks", []() {
    String json = "[";
    for (size_t i = 0; i < activeTasks.size(); i++) {
      if (i > 0) json += ",";
      json += "{\"id\":" + String(activeTasks[i].id) + ",\"description\":\"" + activeTasks[i].description + "\"}";
    }
    json += "]";
    server.send(200, "application/json", json);
  });

  server.on("/api/cancel-task", HTTP_POST, []() {
    String body = server.arg("plain");
    DynamicJsonDocument doc(256);
    if (!deserializeJson(doc, body)) {
      int id = doc["id"];
      for (auto it = activeTasks.begin(); it != activeTasks.end(); ++it) {
        if (it->id == id) {
          activeTasks.erase(it);
          server.send(200, "text/plain", "Task cancelled");
          return;
        }
      }
    }
    server.send(404, "text/plain", "Task not found");
  });


  server.on("/api/scripts", []() {
    String json = "[";
    for (size_t i = 0; i < availableScripts.size(); i++) {
      if (i > 0) json += ",";
      json += "\"" + availableScripts[i] + "\"";
    }
    json += "]";
    server.send(200, "application/json; charset=utf-8", json);
  });

  server.on("/api/languages", []() {
    String json = "[";
    for (size_t i = 0; i < availableLanguages.size(); i++) {
      if (i > 0) json += ",";
      json += "\"" + availableLanguages[i] + "\"";
    }
    json += "]";
    server.send(200, "application/json; charset=utf-8", json);
  });

  server.on("/api/load", []() {
    if (server.hasArg("file")) {
      String filename = server.arg("file");
      String content = loadScript(filename);
      if (content.length() > 0) {
        server.send(200, "text/plain; charset=utf-8", content);
      } else {
        server.send(404, "text/plain; charset=utf-8", "File not found or empty");
      }
    } else {
      server.send(400, "text/plain; charset=utf-8", "No file specified");
    }
  });

  server.on("/api/save", HTTP_POST, []() {
    String body = server.arg("plain");
    DynamicJsonDocument doc(4096);
    DeserializationError error = deserializeJson(doc, body);

    if (error) {
      server.send(400, "text/plain; charset=utf-8", "Invalid JSON");
      return;
    }

    String filename = doc["filename"].as<String>();
    String content = doc["content"].as<String>();

    if (saveScript(filename, content)) {
      server.send(200, "text/plain; charset=utf-8", "Script saved: " + filename);
    } else {
      server.send(500, "text/plain; charset=utf-8", "Failed to save script");
    }
  });

  server.on("/api/delete", HTTP_DELETE, []() {
    if (server.hasArg("file")) {
      String filename = server.arg("file");
      if (deleteScript(filename)) {
        server.send(200, "text/plain; charset=utf-8", "Script deleted: " + filename);
      } else {
        server.send(500, "text/plain; charset=utf-8", "Failed to delete script");
      }
    } else {
      server.send(400, "text/plain; charset=utf-8", "No file specified");
    }
  });

  server.on("/api/check-file", []() {
    if (server.hasArg("file")) {
      String filename = server.arg("file");
      if (!filename.endsWith(".txt")) filename += ".txt";
      String filePath = String(DIR_SCRIPTS) + "/" + filename;
      bool exists = SD.exists(filePath);
      server.send(200, "application/json", "{\"exists\":" + String(exists ? "true" : "false") + "}");
    } else {
      server.send(400, "text/plain; charset=utf-8", "No file specified");
    }
  });

  server.on("/api/save-settings", HTTP_POST, []() {
    String body = server.arg("plain");
    DynamicJsonDocument doc(256);
    DeserializationError error = deserializeJson(doc, body);

    if (error) {
      server.send(400, "text/plain; charset=utf-8", "Invalid JSON");
      return;
    }

    String type = doc["type"].as<String>();
    if (type == "language") {
      String newLang = doc["language"].as<String>();
      if (loadLanguage(newLang)) {
        saveSettings();
        server.send(200, "text/plain; charset=utf-8", "Language applied: " + newLang);
      } else {
        server.send(500, "text/plain; charset=utf-8", "Failed to load language file");
      }
    } else {
      server.send(400, "text/plain; charset=utf-8", "Unknown settings type");
    }
  });

  server.on("/api/save-wifi", HTTP_POST, []() {
    // Same silent-400 fix as /api/setup-complete — reject with JSON +
    // Serial log + on-screen toast so the wizard/settings panel can
    // actually show the error, not sit spinning.
    auto reject = [](int code, const char* err) {
      Serial.printf("[/api/save-wifi] %d: %s\n", code, err);
      watchUiFlash(err);
      String body = String("{\"ok\":false,\"error\":\"") + err + "\"}";
      server.send(code, "application/json", body);
    };
    String body = server.arg("plain");
    DynamicJsonDocument doc(512);
    if (deserializeJson(doc, body)) { reject(400, "Invalid JSON body"); return; }

    String newSsid = doc["ssid"].as<String>();
    String newPsk  = doc["password"].as<String>();
    if (newSsid.length() == 0 || newSsid.length() > 32) {
      reject(400, "SSID must be 1-32 chars"); return;
    }
    if (newPsk.length() < 8 || newPsk.length() > 63) {
      reject(400, "AP password must be 8-63 chars (WPA2)"); return;
    }
    ap_ssid = newSsid;
    ap_password = newPsk;
    // v4.4: clamp scanTime — was unbounded; a user-supplied 0 broke scans and
    // a huge value could freeze the async scan flow.
    int t = doc["scanTime"] | WIFI_SCAN_TIMEOUT;
    if (t < 500)   t = 500;
    if (t > 15000) t = 15000;
    wifiScanTime = t;
    saveSettings();

    Serial.printf("[/api/save-wifi] OK: ssid='%s' pw-len=%u — rebooting\n",
                  newSsid.c_str(), (unsigned)newPsk.length());
    watchUiFlash("WiFi saved - rebooting");
    server.send(200, "application/json", "{\"ok\":true,\"rebooting\":true}");
    for (int i = 0; i < 30; ++i) { watchUiTick(); delay(20); }
    watchSafeRestart();
  });

  server.on("/api/set-boot-script", HTTP_POST, []() {
    String body = server.arg("plain");
    DynamicJsonDocument doc(1024);
    DeserializationError error = deserializeJson(doc, body);

    if (error) {
      server.send(400, "text/plain; charset=utf-8", "Invalid JSON");
      return;
    }

    JsonArray files = doc["filenames"].as<JsonArray>();
    currentBootScriptFiles.clear();
    bootScript = "";
    String prefString = "";

    for (JsonVariant v : files) {
      String filename = v.as<String>();
      if (SD.exists(String(DIR_SCRIPTS) + "/" + filename)) {
        currentBootScriptFiles.push_back(filename);
        if (prefString.length() > 0) prefString += ",";
        prefString += filename;
        bootScript += loadScript(filename) + "\n";
      }
    }

    if (currentBootScriptFiles.size() > 0) {
      bootModeEnabled = true;
      preferences.putString("boot_script", prefString);
      server.send(200, "text/plain; charset=utf-8", "Boot scripts set: " + prefString);
    } else {
      bootModeEnabled = false;
      preferences.remove("boot_script");
      server.send(200, "text/plain; charset=utf-8", "Boot scripts disabled");
    }
  });

  server.on("/api/test-boot-script", HTTP_POST, []() {
    String body = server.arg("plain");
    DynamicJsonDocument doc(256);
    DeserializationError error = deserializeJson(doc, body);

    if (error) {
      server.send(400, "text/plain; charset=utf-8", "Invalid JSON");
      return;
    }

    String filename = doc["filename"].as<String>();

    if (SD.exists(String(DIR_SCRIPTS) + "/" + filename)) {
      String script = loadScript(filename);
      executeScript(script);
      server.send(200, "text/plain; charset=utf-8", "Testing boot script: " + filename);
    } else {
      server.send(404, "text/plain; charset=utf-8", "Script file not found");
    }
  });

  // ---- LAN / DeadNet API endpoints --------------------------------------
  // Reverted the "wired-only" gate (the Waveshare 2.06" board doesn't have
  // an OTG boost on VBUS, so wired-USB-Ethernet was never going to happen
  // on this hardware anyway). DeadNet now targets the LAN the watch is
  // joined to via WiFi STA — same design as the flashnuke original — and
  // /api/lan/status reports STA link.
  //
  // Modes that cost dashboard uptime are refused unless the caller
  // explicitly acknowledges: DEAUTH sends management frames on the shared
  // channel and can knock the dashboard's own AP clients loose; SNIFF
  // flips the whole radio into promiscuous, so TCP through our AP takes
  // heavy latency hits. Both need ?risky=1 on the start call.

  server.on("/api/lan/status", []() {
    DynamicJsonDocument doc(256);
    bool up = (WiFi.status() == WL_CONNECTED);
    doc["connected"]      = up;
    doc["driverPresent"]  = true;    // WiFi STA is our LAN link now
    doc["reason"]         = up ? "" : "not joined to any WiFi (Internet Connection above)";
    doc["staWifiConnected"] = up;
    if (up) {
      doc["ssid"]           = WiFi.SSID();
      doc["ip"]             = WiFi.localIP().toString();
      doc["gateway"]        = WiFi.gatewayIP().toString();
      doc["rssi"]           = WiFi.RSSI();
      doc["staSsid"]        = WiFi.SSID();
      doc["staIp"]          = WiFi.localIP().toString();
    }
    String out; serializeJson(doc, out);
    server.send(200, "application/json", out);
  });

  server.on("/api/dead/start", HTTP_POST, []() {
    // Gate: DeadNet targets the LAN the watch is joined to via WiFi STA.
    if (WiFi.status() != WL_CONNECTED) {
      server.send(409, "application/json",
        "{\"ok\":false,\"error\":\"not joined to any WiFi — pick a network under Internet Connection first\"}");
      return;
    }
    String body = server.arg("plain");
    DynamicJsonDocument doc(1024);
    if (deserializeJson(doc, body)) {
      server.send(400, "application/json", "{\"ok\":false,\"error\":\"bad json\"}");
      return;
    }
    uint8_t mode = doc["mode"] | ATTACK_MODE_ARP;
    bool  risky = doc["risky"] | false;
    // Fence off modes that break dashboard uptime unless the caller
    // explicitly opts in with risky=true. This matches the honest
    // engineering read: ARP + RA are safe to run alongside the AP
    // (STA-side data traffic, no radio-state change). DEAUTH sends
    // management frames on the shared channel — real risk of knocking
    // dashboard clients loose. SNIFF flips the entire radio into
    // promiscuous mode, so TCP through our AP takes heavy latency
    // hits. DNS silently doesn't run (port 53 held by captive DNS,
    // documented at bug-hunt #3).
    uint8_t risky_bits = ATTACK_MODE_DEAUTH | ATTACK_MODE_SNIFF;
    if ((mode & risky_bits) && !risky) {
      String bad;
      if (mode & ATTACK_MODE_DEAUTH) bad += "DEAUTH ";
      if (mode & ATTACK_MODE_SNIFF)  bad += "SNIFF ";
      String err = String("{\"ok\":false,\"error\":\"mode(s) ") + bad +
                   String("degrade dashboard uptime — call again with risky:true to acknowledge\"}");
      server.send(409, "application/json", err);
      return;
    }
    if (mode & ATTACK_MODE_DNS) {
      Serial.println("[/api/dead/start] ATTACK_MODE_DNS requested but the firmware's captive DNS holds port 53 — DNS spoof task will fail to bind and self-exit");
    }
    std::vector<IPAddress> targets;
    if (doc.containsKey("targets") && doc["targets"].is<JsonArray>()) {
      for (JsonVariant v : doc["targets"].as<JsonArray>()) {
        IPAddress ip;
        if (ip.fromString(v.as<const char*>())) targets.push_back(ip);
      }
    }
    bool ok = g_deadnet.startAttack(mode, targets);
    if (ok) {
      // Task: "when its on everything gets offed except wifi". Detach
      // TinyUSB (HID+MSC+CDC) so packet-inject frames aren't stealing
      // CPU / USB IRQs; stop BT so the coexistence engine doesn't
      // preempt the WiFi driver during a poison cycle.
      extern void hidDetach();
      extern void stopBT();
      extern bool bluetoothToggleEnabled;
      hidDetach();
      if (bluetoothToggleEnabled) { stopBT(); bluetoothToggleEnabled = false; }
      Serial.println("[/api/dead/start] HID + BT stopped while DeadNet runs");
    }
    server.send(ok ? 200 : 500, "application/json",
                ok ? "{\"ok\":true}" : "{\"ok\":false,\"error\":\"start failed\"}");
  });
  server.on("/api/dead/stop", HTTP_POST, []() {
    g_deadnet.stopAttack();
    // Symmetrical: bring HID back for whoever wants it next. BT stays
    // off — that was a user pref that we deliberately overrode; user
    // can flip it back on in Settings when they want it.
    // USBManager.h already declares hidAttach() as `bool hidAttach()`; a
    // local `extern void hidAttach()` here re-declared ambiguously.
    hidAttach();
    Serial.println("[/api/dead/stop] HID re-attached; BT left off");
    server.send(200, "application/json", "{\"ok\":true}");
  });
  server.on("/api/dead/status", []() {
    DynamicJsonDocument doc(512);
    doc["running"]  = g_deadnet.isRunning();
    doc["mode"]     = g_deadnet.getMode();
    doc["packets"]  = g_deadnet.getPacketsSent();
    doc["cycles"]   = g_deadnet.getCycleCount();
    doc["gateway"]  = g_deadnet.getGatewayIpStr();
    doc["gwMac"]    = g_deadnet.getGatewayMacStr();
    String out; serializeJson(doc, out);
    server.send(200, "application/json", out);
  });
  server.on("/api/dead/hosts", []() {
    auto hosts = g_deadnet.getHosts();
    // Bug-hunt round 3 fix: sized to fit MAX_HOSTS (64) fully populated
    // with a resolved hostname. Per-entry cost in v6 ArduinoJson is
    // ~JSON_OBJECT_SIZE(4) + string pool for ip(16) + mac(18) + host(<=48).
    // 64 * ~180 = 11.5 KB. Was 8 KB and silently truncated once a busy
    // /24 populated all 64 slots, leaving the dashboard host list empty.
    DynamicJsonDocument doc(16384);
    JsonArray arr = doc.to<JsonArray>();
    for (auto& h : hosts) {
      JsonObject o = arr.createNestedObject();
      o["ip"]   = h.ip.toString();
      char mac[18];
      snprintf(mac, sizeof(mac), "%02X:%02X:%02X:%02X:%02X:%02X",
               h.mac[0], h.mac[1], h.mac[2], h.mac[3], h.mac[4], h.mac[5]);
      o["mac"]  = mac;
      o["ping"] = h.pingMs;
      o["host"] = h.hostname;
    }
    String out; serializeJson(doc, out);
    server.send(200, "application/json", out);
  });
  server.on("/api/dead/sniff", []() {
    auto ss = g_deadnet.getSniffedData();
    DynamicJsonDocument doc(16384);
    JsonArray arr = doc.to<JsonArray>();
    for (auto& s : ss) {
      JsonObject o = arr.createNestedObject();
      o["src"]  = s.source; o["type"] = s.type; o["msg"] = s.content;
      o["ts"]   = s.timestamp;
    }
    String out; serializeJson(doc, out);
    server.send(200, "application/json", out);
  });

  // Real-clock sync: the dashboard fetches this on load and posts the
  // browser's current epoch + tz offset. The firmware stores both in NVS
  // and uses them to render HH:MM on the on-screen clock face. Persisted
  // across reboots — after the first sync the clock is always meaningful
  // as long as the wearer has opened the dashboard at least once.
  server.on("/api/set-time", HTTP_POST, []() {
    String body = server.arg("plain");
    DynamicJsonDocument doc(256);
    if (deserializeJson(doc, body)) {
      server.send(400, "application/json", "{\"ok\":false,\"error\":\"bad json\"}");
      return;
    }
    uint64_t epoch = doc["epoch"].as<uint64_t>();     // UTC seconds
    int32_t  tz    = doc["tz_seconds"].as<int32_t>(); // local offset from UTC
    if (epoch < 1700000000ULL) {                       // sanity: pre-2023 = bogus
      server.send(400, "application/json", "{\"ok\":false,\"error\":\"epoch too small\"}");
      return;
    }
    // Bug-hunt round 5 fix: reject bogus epoch (>2100) and out-of-range tz.
    // Without this a malformed browser call (or a compromised client) could
    // shove e.g. epoch=UINT64_MAX / tz=INT32_MAX into NVS, and the on-screen
    // clock face would render as garbage forever until the user re-syncs.
    if (epoch > 4102444800ULL) {                       // >2100-01-01
      server.send(400, "application/json", "{\"ok\":false,\"error\":\"epoch too large\"}");
      return;
    }
    if (tz < -43200 || tz > 50400) {                   // UTC-12 .. UTC+14
      server.send(400, "application/json", "{\"ok\":false,\"error\":\"tz_seconds out of range\"}");
      return;
    }
    preferences.putULong64("clock_epoch",   epoch);
    preferences.putLong   ("clock_tz_secs", tz);
    // Record the local millis() at time of sync so the drift correction
    // watchUiSetClockSeconds does can subtract it from millis() at
    // display time. Wrap-around at ~49 days is handled by re-syncing.
    preferences.putULong  ("clock_sync_ms", millis());
    Serial.printf("[/api/set-time] epoch=%llu tz=%d ms=%lu\n",
                  (unsigned long long)epoch, (int)tz, (unsigned long)millis());
    server.send(200, "application/json", "{\"ok\":true}");
  });

  server.on("/api/stats", []() {
    DynamicJsonDocument doc(4096);
    // Watch port: expose a `hidden_settings` list so a single dashboard
    // HTML can serve every firmware variant. The frontend reads this array
    // on /api/stats and hides any switch/setting whose `data-setting`
    // attribute matches an entry here. Extend the list when a firmware
    // variant lacks a physical feature. Key/devkitc variants can ship
    // the same index.html and just leave this array empty (their build
    // of /api/stats will return no entries).
    {
      JsonArray hidden = doc.createNestedArray("hidden_settings");
      // Watch has no addressable / plain status LED — hide the "LED status"
      // switch on both the AMOLED settings modal (already omitted) and the
      // web dashboard.
      hidden.add("led");
    }
    doc["errorCount"] = errorCount;
    doc["totalScripts"] = totalScriptsExecuted;
    doc["totalCommands"] = totalCommandsExecuted;
    doc["clientCount"] = WiFi.softAPgetStationNum();
    doc["lastError"] = lastError;
    doc["sdCardPresent"] = sdCardPresent;
    doc["uptime"] = millis() / 1000;
    doc["freeMemory"] = ESP.getFreeHeap();
    doc["totalMemory"] = ESP.getHeapSize();
    doc["cpuMhz"]  = ESP.getCpuFreqMHz();
    doc["cpuCores"] = ESP.getChipCores();
    // v4.16: flash usage stats for the Stats tile.
    //   flashSize        - total flash chip size in bytes
    //   sketchSize       - the running app's size (what fw took of the app partition)
    //   sketchTotal      - the app partition size (max sketch)
    //   sketchUsedPct    - sketchSize / sketchTotal * 100 (int)
    {
      uint32_t sSize  = ESP.getSketchSize();
      uint32_t sFree  = ESP.getFreeSketchSpace();
      uint32_t sTotal = sSize + sFree;   // sketch partition capacity
      doc["sketchSize"]    = sSize;
      doc["sketchTotal"]   = sTotal;
      doc["sketchUsedPct"] = (sTotal > 0) ? (int)((uint64_t)sSize * 100ULL / sTotal) : 0;
    }
    // v4.10: live busy% computed in loop() (see cpuBusyPercent()).
    extern uint32_t cpuBusyPercent();
    doc["cpuBusyPct"] = cpuBusyPercent();
    // v4.11: live die temperature + thermal governor state.
    // State: 0=normal (green), 1=warm (yellow), 2=hot (orange), 3=critical (red).
    extern float   cpuTemperatureC();
    extern uint8_t thermalState();
    extern bool    isThermalShutdown();
    doc["cpuTempC"]        = cpuTemperatureC();
    doc["thermalState"]    = thermalState();
    doc["thermalShutdown"] = isThermalShutdown();
    doc["flashSize"] = ESP.getFlashChipSize();
    // v4.16 perf: SD.usedBytes walks the FAT chain and can take hundreds of
    // ms on a big card, while also holding the SPI mutex that MSC needs.
    // Cache the values for 30 s so the 5 s /api/stats poll doesn't murder
    // host copy throughput.
    {
      static uint64_t __sdTotalCached = 0;
      static uint64_t __sdUsedCached  = 0;
      static uint32_t __sdCachedAt    = 0;
      if (sdCardPresent && (millis() - __sdCachedAt > 30000 || __sdCachedAt == 0)) {
        __sdTotalCached = SD.totalBytes();
        __sdUsedCached  = SD.usedBytes();
        __sdCachedAt    = millis();
      }
      doc["sdTotal"] = (double)(sdCardPresent ? __sdTotalCached : 0);
      doc["sdUsed"]  = (double)(sdCardPresent ? __sdUsedCached  : 0);
    }
    doc["shellEnabled"] = false;                     // legacy — always false in v4.5
    doc["comEnabled"]   = preferences.getBool("com_on", false);   // v4.5: Allow COM connections
    doc["usbRndVid"] = currentUSBConfig.rndVid;
    doc["usbRndPid"] = currentUSBConfig.rndPid;
    doc["detectedOS"] = detectedOS;
    doc["currentLanguage"] = currentLanguage;
    doc["wifiConnected"] = (WiFi.status() == WL_CONNECTED);
    doc["wifiSSID"] = ap_ssid;
    // v4.4: passwords removed from /api/stats — the dashboard polled this
    // every 5 s and broadcasted AP + STA passwords over WiFi in every reply.
    // Settings page can hit /api/wifi-secrets on demand for the initial
    // populate. Leave empty strings here for JS backward-compat.
    doc["wifiPassword"] = "";
    doc["staSSID"] = (WiFi.status() == WL_CONNECTED) ? current_sta_ssid : "";
    doc["staPassword"] = "";
    doc["wifiScanTime"] = wifiScanTime;
    doc["usbVID"] = currentUSBConfig.vid;
    doc["usbPID"] = currentUSBConfig.pid;
    doc["usbRndVID"] = currentUSBConfig.rndVid;
    doc["usbRndPID"] = currentUSBConfig.rndPid;
    doc["usbMfr"] = currentUSBConfig.mfr;
    doc["usbProd"] = currentUSBConfig.prod;
    doc["ledEnabled"] = ledEnabled;
    doc["loggingEnabled"] = loggingEnabled;
    doc["wifiToggleEnabled"] = wifiToggleEnabled;
    doc["btToggleEnabled"] = bluetoothToggleEnabled;
    doc["btDiscoveryEnabled"] = btDiscoveryEnabled;
    doc["autoConnectEnabled"] = autoConnectEnabled;
    doc["saveOnConnectEnabled"] = saveOnConnectEnabled;
    doc["silentStartup"] = silentStartup;
    // User ask: web UI toggle state should track AMOLED switch state and
    // vice versa. /api/stats is polled by the dashboard every 5 s, so
    // include every toggle the AMOLED can flip so the web-side JS can
    // sync its checkbox DOMs to reality.
    doc["autostartEnabled"] = preferences.getBool("autostart_on", false);
    doc["deadnetRunning"]   = g_deadnet.isRunning();
    // Wired-LAN state: DeadNet can only fire against wired Ethernet.
    // driverPresent=false today (USB Host + adapter driver TBD).
    doc["lanWiredUp"]       = false;
    doc["lanDriverPresent"] = false;
    // v4.4: expose tutorial state so the frontend knows whether to show the
    // first-boot walk-through overlay. Factory reset wipes 'tutorial_done'
    // via preferences.clear(), so it re-fires on next boot.
    doc["firstBoot"]    = !preferences.getBool("tutorial_done", false);
    doc["tutorialDone"] =  preferences.getBool("tutorial_done", false);
    // v4.12: setup wizard state + Randomize-MAC toggle for the Settings tab.
    doc["setupDone"]    =  preferences.getBool("setup_done", false);
    doc["randomMac"]    =  preferences.getBool("random_mac", false);
    doc["blinkOnRun"]   =  preferences.getBool("blink_on_run", true);
    // v4.12: publish the running firmware version so the .espkg upload UI can
    // warn on downgrade.
    doc["fwVersion"]    =  "4.23";
    // v4.9: MSC sub-region status. When mscSubSectors > 0 the host only sees
    // that many sectors of the SD (offset by mscBaseSector). Dashboard uses
    // this to show a "SD hidden - MSC exposes X MB from LBA Y" indicator.
    JsonObject sub = doc.createNestedObject("mscSubRegion");
    sub["active"]     = (mscSubSectors > 0);
    sub["baseSector"] = mscBaseSector;
    sub["sectors"]    = mscSubSectors;
    sub["bytes"]      = (double)mscSubSectors * 512.0;
    // ATTACKMODE / SIZE snapshot for the web UI
    {
      char vidHex[8]; char pidHex[8];
      snprintf(vidHex, sizeof(vidHex), "0x%04X", currentAttackMode.vid);
      snprintf(pidHex, sizeof(pidHex), "0x%04X", currentAttackMode.pid);
      JsonObject am = doc["attackMode"].to<JsonObject>();
      am["hid"]       = currentAttackMode.hid;
      am["storage"]   = currentAttackMode.storage;
      am["vid"]       = vidHex;
      am["pid"]       = pidHex;
      am["sizeBytes"] = (double)currentAttackMode.sizeBytes;  // JSON int precision limit
    }
    doc["silentStartup"] = silentStartup;
    // Currently-selected boot scripts, so the Boot tab can show them checked
    JsonArray bootArr = doc.createNestedArray("bootScripts");
    for (auto& f : currentBootScriptFiles) bootArr.add(f);
    // Delay progress (0-100)
    if (currentDelayTotal > 0) {
      unsigned long elapsed = millis() - currentDelayStart;
      int progress = (int)min(100UL, (elapsed * 100UL) / currentDelayTotal);
      doc["delayProgress"] = progress;
      doc["delayTotal"] = currentDelayTotal;
    } else {
      doc["delayProgress"] = 0;
      doc["delayTotal"] = 0;
    }

    String response;
    serializeJson(doc, response);
    server.sendHeader("Cache-Control", "no-cache, no-store, must-revalidate, max-age=0");
    server.sendHeader("Pragma", "no-cache");
    server.sendHeader("Expires", "0");
    server.send(200, "application/json", response);
  });


  server.on("/api/history", []() {
    String json = "[";
    for (size_t i = 0; i < commandHistory.size(); i++) {
      if (i > 0) json += ",";
      json += "\"" + commandHistory[i] + "\"";
    }
    json += "]";
    server.send(200, "application/json", json);
  });

  server.on("/api/clear-history", HTTP_POST, []() {
    commandHistory.clear();
    saveCommandHistory();
    server.send(200, "text/plain; charset=utf-8", "Command history cleared");
  });

  server.on("/api/export-history", []() {
    String historyContent = "";
    for (String cmd : commandHistory) {
      historyContent += cmd + "\n";
    }
    server.send(200, "text/plain", historyContent);
  });

  server.on("/api/clear-errors", HTTP_POST, []() {
    clearErrors();
    server.send(200, "text/plain; charset=utf-8", "Error log cleared");
  });

  // Factory reset (v4.4 rewrite):
  //   * Wipes every user script, upload, and log on the SD card.
  //   * KEEPS the website payload (index.html/style.css/script.js) so the
  //     dashboard still boots after the reboot.
  //   * KEEPS the languages folder (compiled-in resource, not "user data").
  //   * Wipes the NVS 'badusb' namespace so the next boot is truly fresh —
  //     tutorial reappears (tutorial_done flag was in there), AP creds reset
  //     to defaults, silent-boot flag reset, ATTACKMODE resets to HID-only.
  //   * Sends the HTTP 200 FIRST, then flashes the blue update-progress LED
  //     while it wipes, so the browser sees "Factory reset in progress..."
  //     before the ESP reboots and the socket dies.
  server.on("/api/factory-reset", HTTP_POST, []() {
    server.send(200, "text/plain; charset=utf-8",
                "Factory reset started — scripts, uploads, logs, and settings will be wiped. LED will blink blue. Reboots in ~5s.");
    delay(150);   // let LWIP drain the response
    setLEDMode(6);   // fast BLUE blink (same mode used by .espkg update)
    Serial.println("[FACTORY] Wiping SD user content (scripts/uploads/logs)...");

    // Helper: delete every file under a directory (non-recursive; we don't
    // have nested user content). Robust against ESP32 SD's file.name()
    // returning the full path.
    // v4.16 FIX: collect-then-delete (iterator invalidation - see .ino).
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
      for (auto& full : victims) if (SD.remove(full)) Serial.println("  removed: " + full);
    };
    wipeDir(DIR_SCRIPTS);
    wipeDir(DIR_UPLOADS);
    wipeDir(DIR_LOGS);
    // Also wipe transient state files at the SD root (KEEP web files).
    const char* transientRoot[] = {
      "/reboot_script.txt", "/temp_resume.txt", "/wifi_creds.txt",
      "/temp_creds.txt",    "/update.espkg",   "/.sdtest", "/history.txt"
    };
    for (const char* p : transientRoot) if (SD.exists(p)) SD.remove(p);

    Serial.println("[FACTORY] Clearing NVS 'badusb' namespace...");
    preferences.clear();
    preferences.end();

    Serial.println("[FACTORY] Reboot in 1500 ms.");
    delay(1500);
    watchSafeRestart();
  });

  // Tutorial state — the frontend calls this on skip / finish to mark the
  // walk-through as seen. Factory reset clears this via preferences.clear().
  server.on("/api/tutorial-done", HTTP_POST, []() {
    preferences.putBool("tutorial_done", true);
    server.send(200, "application/json", "{\"ok\":true}");
  });

  // v4.16: lightweight /api/temp for the 1 Hz temperature poll. Returns only
  // temp + thermal state so the dashboard can update the tile every second
  // without pulling the full ~4 KB /api/stats payload.
  server.on("/api/temp", []() {
    extern float   cpuTemperatureC();
    extern uint8_t thermalState();
    extern bool    isThermalShutdown();
    char buf[128];
    snprintf(buf, sizeof(buf),
      "{\"tempC\":%.2f,\"state\":%u,\"shutdown\":%s}",
      cpuTemperatureC(), (unsigned)thermalState(),
      isThermalShutdown() ? "true" : "false");
    server.sendHeader("Cache-Control", "no-cache, no-store, must-revalidate");
    server.send(200, "application/json", buf);
  });

  // v4.12: first-boot setup wizard endpoint. Frontend POSTs after the user
  // walks through EULA / SSID / password / silent / usb-random / mac-random.
  // v4.16: SECURITY - validate SSID length (<=32), reject WPA2-invalid short
  // passwords, REQUIRE the toggle keys be present (no silent default flip),
  // and refuse re-run after setup unless the caller supplies the current AP
  // password (blocks a guest on the AP from rewriting creds).
  server.on("/api/setup-complete", HTTP_POST, []() {
    // Watch port bug hunt (recovered scout finding): every early-return
    // path was `server.send(400, "text/plain", ...)` with NO Serial log
    // and NO watchUiFlash. User taps "Save WiFi" in the wizard, ESP
    // rejects (missing key / short PSK), wizard frontend doesn't surface
    // the plain-text error, user sees "hang" — exact reported symptom.
    // Now: every rejection logs to Serial, flashes on the AMOLED, and
    // returns JSON with an `error` field so the frontend has structured
    // data to render.
    auto reject = [](int code, const char* err) {
      Serial.printf("[/api/setup-complete] %d: %s\n", code, err);
      watchUiFlash(err);
      String body = String("{\"ok\":false,\"error\":\"") + err + "\"}";
      server.send(code, "application/json", body);
    };

    String body = server.arg("plain");
    DynamicJsonDocument doc(1024);
    if (deserializeJson(doc, body)) { reject(400, "Invalid JSON body"); return; }

    // Always require the current AP password — protects first-run hijack where
    // an attacker who knows the default passphrase could call setup-complete
    // before the owner does and lock them out.
    {
      String claimed = doc["currentPassword"] | "";
      if (claimed != ap_password) {
        reject(403, "Supply currentPassword (current AP passphrase) to run setup");
        return;
      }
    }

    String ssid = doc["ssid"] | "";
    String pw   = doc["password"] | "";

    // Required toggle keys - refuse silent defaults.
    if (!doc.containsKey("silentStartup") || !doc.containsKey("randomizeUsb") ||
        !doc.containsKey("randomizeMac")) {
      reject(400, "Missing keys: silentStartup / randomizeUsb / randomizeMac");
      return;
    }
    bool silent = doc["silentStartup"];
    bool usbRnd = doc["randomizeUsb"];
    bool macRnd = doc["randomizeMac"];

    // Input validation.
    if (ssid.length() == 0 || ssid.length() > 32) {
      reject(400, "SSID must be 1-32 chars"); return;
    }
    if (pw.length() < 8 || pw.length() > 63) {
      reject(400, "AP password must be 8-63 chars (WPA2)");
      return;
    }

    preferences.putString("ap_ssid",     ssid);
    preferences.putString("ap_password", pw);
    preferences.putBool("silent_boot", silent);
    preferences.putBool("usb_rndVid",  usbRnd);
    preferences.putBool("usb_rndPid",  usbRnd);
    preferences.putBool("random_mac",  macRnd);
    preferences.putBool("setup_done",  true);
    preferences.putBool("tutorial_done", true);

    Serial.printf("[/api/setup-complete] OK: ssid='%s' pw-len=%u silent=%d — rebooting\n",
                  ssid.c_str(), (unsigned)pw.length(), (int)silent);
    watchUiFlash("Setup saved - rebooting");
    server.send(200, "application/json", "{\"ok\":true,\"rebooting\":true}");
    // Let LWIP flush the 200 before we reset, and let the toast paint.
    for (int i = 0; i < 30; ++i) { watchUiTick(); delay(20); }
    watchSafeRestart();
  });

  // ================================================================
  // v4.17: Extensions - Hak5-compatible reusable payload snippets.
  // Stored on the SD under /extensions/ as .txt or .dsx files.
  // ================================================================
  // v4.22: folder-aware listing. Returns {"hak5":[...], "custom":[...]} so
  // the frontend can render two grouped sections. Also lists any files still
  // at /extensions/ root (legacy from before v4.22) under "hak5" so nothing
  // gets orphaned.
  server.on("/api/list-extensions", []() {
    if (!sdCardPresent) { server.send(500, "text/plain", "SD not present"); return; }
    if (!SD.exists("/extensions"))        SD.mkdir("/extensions");
    if (!SD.exists("/extensions/hak5"))   SD.mkdir("/extensions/hak5");
    if (!SD.exists("/extensions/custom")) SD.mkdir("/extensions/custom");
    // v4.17-post-hunt BUG #10 fix: JSON-escape filenames. A single stray "
    // in a leaf name (creatable via MSC direct access) broke the whole
    // tab before this. Also skip entries whose leaf contains a slash or
    // backslash - those are FAT anomalies and can't be safely round-tripped.
    auto jsonEscape = [](const String& s) -> String {
      String out; out.reserve(s.length() + 4);
      for (size_t i = 0; i < s.length(); i++) {
        char c = s.charAt(i);
        switch (c) {
          case '"':  out += "\\\""; break;
          case '\\': out += "\\\\"; break;
          case '\b': out += "\\b"; break;
          case '\f': out += "\\f"; break;
          case '\n': out += "\\n"; break;
          case '\r': out += "\\r"; break;
          case '\t': out += "\\t"; break;
          default:
            if ((unsigned char)c < 0x20) {
              char buf[8]; snprintf(buf, sizeof(buf), "\\u%04x", (unsigned)c);
              out += buf;
            } else out += c;
        }
      }
      return out;
    };
    // Helper: emit the contents of one folder as a JSON array. Only files
    // (not sub-directories) are listed; leaf names with backslash or empty
    // are dropped (FAT anomalies that can't be round-tripped).
    auto listFolder = [&jsonEscape](const char* path) -> String {
      String arr = "[";
      bool first = true;
      File dir = SD.open(path);
      if (dir) {
        File f = dir.openNextFile();
        while (f) {
          if (!f.isDirectory()) {
            String leaf = String(f.name());
            int slash = leaf.lastIndexOf('/');
            if (slash >= 0) leaf = leaf.substring(slash + 1);
            if (leaf.indexOf('\\') < 0 && leaf.length() > 0) {
              if (!first) arr += ",";
              arr += "{\"name\":\"" + jsonEscape(leaf) + "\",\"size\":" + String((unsigned)f.size()) + "}";
              first = false;
            }
          }
          f = dir.openNextFile();
        }
        dir.close();
      }
      arr += "]";
      return arr;
    };
    String hak5   = listFolder("/extensions/hak5");
    String custom = listFolder("/extensions/custom");
    String legacy = listFolder("/extensions");   // pre-v4.22 files at root
    String json = "{\"hak5\":" + hak5 + ",\"custom\":" + custom + ",\"legacy\":" + legacy + "}";
    server.send(200, "application/json", json);
  });

  // v4.22: helper - resolve folder arg ("hak5" | "custom" | "" legacy) to a
  // safe path prefix. Returns empty string on invalid input.
  static auto __extFolder = [](const String& folder) -> String {
    if (folder == "hak5")   return "/extensions/hak5";
    if (folder == "custom") return "/extensions/custom";
    if (folder == "")       return "/extensions";   // legacy fall-through
    return "";
  };

  server.on("/api/load-extension", []() {
    if (!server.hasArg("name")) { server.send(400, "text/plain", "Missing name"); return; }
    String name   = server.arg("name");
    String folder = server.hasArg("folder") ? server.arg("folder") : "";
    if (name.indexOf("..") >= 0 || name.indexOf('/') >= 0) {
      server.send(400, "text/plain", "Bad name"); return;
    }
    String base = __extFolder(folder);
    if (base.length() == 0) { server.send(400, "text/plain", "Bad folder"); return; }
    String path = base + "/" + name;
    // If not in the specified folder, fall back to the sibling folder before
    // giving up (so the UI's "load" works whether the file lives in hak5/
    // or custom/ or legacy root).
    if (!SD.exists(path)) {
      const char* fallbacks[] = {"/extensions/hak5", "/extensions/custom", "/extensions"};
      for (const char* fb : fallbacks) {
        String p2 = String(fb) + "/" + name;
        if (SD.exists(p2)) { path = p2; break; }
      }
    }
    if (!SD.exists(path)) { server.send(404, "text/plain", "Not found"); return; }
    File f = SD.open(path);
    if (!f) { server.send(500, "text/plain", "Open failed"); return; }
    server.streamFile(f, "text/plain");
    f.close();
  });

  server.on("/api/save-extension", HTTP_POST, []() {
    String body = server.arg("plain");
    // v4.17-post-hunt BUG #5 fix: was DynamicJsonDocument(16384) which
    // silently dropped payloads > ~14 KB after JSON overhead. Bumped to
    // 96 KB (well past the largest Hak5 corpus extension), and return a
    // DISTINCT 413 status + message on parse failure so the UI can
    // distinguish "too big" from "malformed".
    DynamicJsonDocument doc(98304);
    DeserializationError je = deserializeJson(doc, body);
    if (je) {
      if (je == DeserializationError::NoMemory) {
        server.send(413, "text/plain", "Extension too large (>96 KB). Trim it or split into multiple extensions.");
      } else {
        server.send(400, "text/plain", "Bad JSON");
      }
      return;
    }
    String name    = doc["name"] | "";
    String content = doc["content"] | "";
    // v4.22: optional folder param; when absent, auto-route by extension:
    // .txt -> hak5/, .ext -> custom/, anything else -> custom/ (safer).
    String folder  = doc["folder"] | "";
    if (folder.length() == 0) {
      String lower = name; lower.toLowerCase();
      if      (lower.endsWith(".txt"))                folder = "hak5";
      else if (lower.endsWith(".ext"))                folder = "custom";
      else                                             folder = "custom";
    }
    if (folder != "hak5" && folder != "custom") {
      server.send(400, "text/plain", "Bad folder (must be hak5 or custom)"); return;
    }
    // v4.23 (bug-hunt MEDIUM #9): also reject backslash. Enforce leaf-only
    // AND a text-file suffix so a poisoned extension can't land as
    // "index.html" or similar in a served URL space.
    String lowerName = name; lowerName.toLowerCase();
    bool goodSuffix = lowerName.endsWith(".txt") || lowerName.endsWith(".ext") ||
                       lowerName.endsWith(".dd")  || lowerName.endsWith(".dsx");
    if (name.length() == 0 || name.indexOf("..") >= 0 || name.indexOf('/') >= 0 ||
        name.indexOf('\\') >= 0 || !goodSuffix) {
      server.send(400, "text/plain", "Bad name (leaf-only + .txt/.ext/.dd/.dsx)"); return;
    }
    String base = "/extensions/" + folder;
    if (!SD.exists(base)) SD.mkdir(base);
    File f = SD.open(base + "/" + name, FILE_WRITE);
    if (!f) { server.send(500, "text/plain", "Open failed"); return; }
    f.print(content);
    f.close();
    server.send(200, "application/json", "{\"ok\":true,\"size\":" + String((unsigned)content.length()) + "}");
  });

  server.on("/api/delete-extension", HTTP_DELETE, []() {
    if (!server.hasArg("name")) { server.send(400, "text/plain", "Missing name"); return; }
    String name   = server.arg("name");
    String folder = server.hasArg("folder") ? server.arg("folder") : "";
    if (name.indexOf("..") >= 0 || name.indexOf('/') >= 0) {
      server.send(400, "text/plain", "Bad name"); return;
    }
    // v4.22: try the specified folder first, then fall back across all three
    // so a "delete" from the UI works even when the folder hint is wrong.
    const char* tries[] = {
      folder == "hak5"   ? "/extensions/hak5"   :
      folder == "custom" ? "/extensions/custom" : "/extensions",
      "/extensions/hak5", "/extensions/custom", "/extensions"
    };
    for (const char* base : tries) {
      String path = String(base) + "/" + name;
      if (SD.exists(path)) {
        if (SD.remove(path)) {
          server.send(200, "application/json", "{\"ok\":true}");
          return;
        }
      }
    }
    server.send(404, "text/plain", "Not found");
  });

  server.on("/api/pull-extension", HTTP_POST, []() {
    if (WiFi.status() != WL_CONNECTED) {
      server.send(503, "text/plain", "WiFi not connected - join a network under Settings > Internet Connection first");
      return;
    }
    String body = server.arg("plain");
    DynamicJsonDocument doc(512);
    if (deserializeJson(doc, body)) { server.send(400, "text/plain", "Bad JSON"); return; }
    String url    = doc["url"] | "";
    String saveAs = doc["saveAs"] | "";
    // v4.23 (bug-hunt MEDIUM #9): tighten saveAs validation - reject
    // backslash + require a text-file suffix so an attacker can't drop
    // arbitrary content into a URL-served extension name.
    String lowerSA = saveAs; lowerSA.toLowerCase();
    bool goodSuffix = lowerSA.endsWith(".txt") || lowerSA.endsWith(".ext") ||
                       lowerSA.endsWith(".dd")  || lowerSA.endsWith(".dsx");
    if (url.length() == 0 || saveAs.length() == 0 ||
        saveAs.indexOf("..") >= 0 || saveAs.indexOf('/') >= 0 || saveAs.indexOf('\\') >= 0 ||
        !goodSuffix) {
      server.send(400, "text/plain", "url required; saveAs must be leaf-only + .txt/.ext/.dd/.dsx"); return;
    }
    // v4.22: pull always saves to /extensions/hak5/ (that's where repo
    // downloads belong; custom stays local-only until the user chooses to
    // move it via the UI).
    if (!SD.exists("/extensions/hak5")) SD.mkdir("/extensions/hak5");
    String path = "/extensions/hak5/" + saveAs;
    bool ok = downloadFileFromURL(url, path);
    if (ok) {
      File f = SD.open(path);
      unsigned sz = f ? (unsigned)f.size() : 0;
      if (f) f.close();
      server.send(200, "application/json", "{\"ok\":true,\"size\":" + String(sz) + "}");
    } else {
      server.send(502, "text/plain", "Download failed");
    }
  });

  // v4.17: toggle "Blink LED while executing payloads".
  server.on("/api/toggle-blink-on-run", HTTP_POST, []() {
    String body = server.arg("plain");
    DynamicJsonDocument doc(128);
    if (!deserializeJson(doc, body) && doc.containsKey("enabled")) {
      bool want = doc["enabled"];
      preferences.putBool("blink_on_run", want);
      server.send(200, "application/json", "{\"enabled\":" + String(want ? "true" : "false") + "}");
    } else {
      server.send(400, "text/plain", "Invalid JSON");
    }
  });

  // v4.12: toggle Randomize-MAC (Settings tab checkbox). Also updated by the
  // setup wizard above. Applied at STA connect time in WiFiManager.
  server.on("/api/toggle-random-mac", HTTP_POST, []() {
    String body = server.arg("plain");
    DynamicJsonDocument doc(128);
    if (!deserializeJson(doc, body) && doc.containsKey("enabled")) {
      bool want = doc["enabled"];
      preferences.putBool("random_mac", want);
      server.send(200, "application/json", "{\"enabled\":" + String(want ? "true" : "false") + "}");
    } else {
      server.send(400, "text/plain", "Invalid JSON");
    }
  });

  // v4.4: BEHAVE_BROKEN — flips the device into permanent SD_READER-only mode
  // on next boot. Requires the AP password (like /selfdestruct) so a random
  // client on the AP can't nuke someone else's device. Recovery is via the
  // GPIO0 5-second hold on boot; this is documented in the confirmation UX.
  server.on("/api/behave-broken", HTTP_POST, []() {
    String body = server.arg("plain");
    DynamicJsonDocument doc(256);
    if (deserializeJson(doc, body)) {
      server.send(400, "text/plain", "Invalid JSON");
      return;
    }
    String password = doc["password"].as<String>();
    if (password != ap_password) {
      server.send(403, "text/plain", "Invalid password");
      return;
    }
    server.send(200, "text/plain",
      "BEHAVE_BROKEN scheduled — device will reboot as SD_READER. "
      "Recovery: hold the GPIO0 button for 5 s at boot.");
    delay(150);
    performBehaveBroken();
  });

  // v4.5: "Allow COM connections" — enables USB CDC on next boot. HID + MSC
  // are disabled while this is on (they'd share USB endpoints with CDC that
  // are already claimed). Composite change requires reboot, so we persist
  // and restart. The Ducky script running on the device can toggle it back
  // off via a WiFi request, or the user can type `disable-com` in the shell.
  server.on("/api/toggle-com", HTTP_POST, []() {
    String body = server.arg("plain");
    DynamicJsonDocument doc(128);
    bool want = false;
    if (!deserializeJson(doc, body)) want = doc["enabled"];
    preferences.putBool("com_on", want);
    server.send(200, "application/json",
      "{\"enabled\":" + String(want ? "true" : "false") + ",\"needsReboot\":true}");
    // Give the response a beat to flush, then reboot to rebuild the USB
    // descriptors with (or without) CDC.
    delay(200);
    watchSafeRestart();
  });

  server.on("/api/validate-script", HTTP_POST, []() {
    String body = server.arg("plain");
    DynamicJsonDocument doc(4096);
    DeserializationError error = deserializeJson(doc, body);

    if (error) {
      server.send(400, "text/plain; charset=utf-8", "Invalid JSON");
      return;
    }

    String script = doc["script"].as<String>();
    int lineCount = 0;
    int errCount = 0;
    String errs = "";

    std::vector<String> lines;
    int startIndex = 0;
    int endIndex = script.indexOf('\n');

    while (endIndex != -1) {
      String line = script.substring(startIndex, endIndex);
      line.trim();
      if (line.length() > 0 && !line.startsWith("REM") && !line.startsWith("//")) {
        lines.push_back(line);
      }
      startIndex = endIndex + 1;
      endIndex = script.indexOf('\n', startIndex);
    }
    if (startIndex < script.length()) {
      String line = script.substring(startIndex);
      line.trim();
      if (line.length() > 0 && !line.startsWith("REM") && !line.startsWith("//")) {
        lines.push_back(line);
      }
    }

    lineCount = lines.size();

    for (String line : lines) {
      if (line.startsWith("STRING ") && line.length() == 7) {
        errs += "Empty STRING command\\n";
        errCount++;
      }
      if (line.startsWith("DELAY ") && line.substring(6).toInt() <= 0) {
        errs += "Invalid DELAY value\\n";
        errCount++;
      }
      if (line.startsWith("REPEAT ") && line.substring(7).toInt() <= 0) {
        errs += "Invalid REPEAT count\\n";
        errCount++;
      }
    }

    if (errCount == 0) {
      server.send(200, "text/plain; charset=utf-8", "Script validation passed! " + String(lineCount) + " commands found.");
    } else {
      server.send(200, "text/plain; charset=utf-8", "Script validation found " + String(errCount) + " issues:\\n" + errs);
    }
  });

  // Trigger async WiFi scan — returns immediately, results via /api/scan-results
  server.on("/api/scan-wifi", []() {
    logDebug("HTTP: /api/scan-wifi called");
    startWiFiScan();
    server.send(202, "application/json", "{\"status\":\"scanning\"}");
  });

  // Poll for async scan results
  // Returns: {"done": false, "networks": []}  while scanning
  //          {"done": true,  "networks": [{ssid,rssi},...]} when complete
  server.on("/api/scan-results", []() {
    int n = WiFi.scanComplete();
    bool done = (n >= 0);
    String json = "{\"done\":" + String(done ? "true" : "false") + ",\"networks\":[";
    if (done) {
      std::vector<String> savedSSIDs = getSavedSSIDs();
      for (int i = 0; i < n; i++) {
        if (i > 0) json += ",";
        String ssid = WiFi.SSID(i);
        bool isSaved = false;
        for (const String& s : savedSSIDs) {
          if (s == ssid) { isSaved = true; break; }
        }

        json += "{";
        json += "\"ssid\":\"" + ssid + "\",";
        json += "\"bssid\":\"" + WiFi.BSSIDstr(i) + "\",";
        json += "\"channel\":" + String(WiFi.channel(i)) + ",";
        json += "\"rssi\":" + String(WiFi.RSSI(i)) + ",";
        json += "\"encryption\":" + String(WiFi.encryptionType(i)) + ",";
        json += "\"saved\":" + String(isSaved ? "true" : "false");
        json += "}";
      }
      logDebug("HTTP: /api/scan-results — done, " + String(n) + " networks");
    }
    json += "]}";
    server.send(200, "application/json", json);
  });

  // Non-blocking WiFi join
  server.on("/api/join-internet", HTTP_POST, []() {
    String body = server.arg("plain");
    logDebug("HTTP: /api/join-internet body=" + body);
    DynamicJsonDocument doc(512);
    if (deserializeJson(doc, body)) {
      server.send(400, "text/plain", "Invalid JSON");
      logDebug("HTTP: /api/join-internet — invalid JSON");
      return;
    }
    String ssid = doc["ssid"].as<String>();
    String password = doc["password"].as<String>();
    bool saveCredentials = doc.containsKey("save") ? (bool)doc["save"] : false;
    if (ssid.length() == 0) {
      server.send(400, "text/plain", "SSID required");
      return;
    }
    logDebug("HTTP: joining WiFi SSID=" + ssid);
    joinWiFi(ssid, password);
    if (saveCredentials && sdCardPresent) {
      saveWiFiCredentials(ssid, password);
      logDebug("WiFi credentials saved for: " + ssid);
    }
    server.send(200, "application/json", "{\"status\":\"connecting\",\"ssid\":\"" + ssid + "\"}");
  });

  // WiFi join status polling
  server.on("/api/wifi-join-status", []() {
    String status;
    if (wifiJoining) {
      unsigned long elapsed = (millis() - wifiJoinStartTime) / 1000;
      status = "connecting";
      logDebug("HTTP: wifi-join-status=connecting, elapsed=" + String(elapsed) + "s");
    } else if (WiFi.status() == WL_CONNECTED) {
      status = "connected";
    } else {
      status = "idle";
    }
    String ssid = (WiFi.status() == WL_CONNECTED) ? current_sta_ssid : "";
    server.sendHeader("Cache-Control", "no-cache, no-store, must-revalidate, max-age=0");
    server.sendHeader("Pragma", "no-cache");
    server.sendHeader("Expires", "0");
    server.send(200, "application/json",
      "{\"status\":\"" + status + "\",\"ssid\":\"" + ssid + "\",\"ip\":\"" + WiFi.localIP().toString() + "\"}");
  });

  // Abort WiFi join
  server.on("/api/stop-wifi-join", HTTP_POST, []() {
    logDebug("HTTP: /api/stop-wifi-join");
    stopJoiningWiFi();
    server.send(200, "text/plain", "WiFi join aborted");
  });

  // Leave internet (disconnect STA)
  server.on("/api/leave-internet", HTTP_POST, []() {
    logDebug("HTTP: /api/leave-internet");
    leaveWiFi();
    server.send(200, "application/json", "{\"message\":\"Disconnected from WiFi\"}");
  });

  // ---- Saved WiFi Credentials ----
  server.on("/api/saved-wifi", []() {
    server.send(200, "application/json", getSavedWiFiCredentials());
  });

  server.on("/api/delete-saved-wifi", HTTP_POST, []() {
    String body = server.arg("plain");
    DynamicJsonDocument doc(256);
    if (!deserializeJson(doc, body) && doc.containsKey("ssid")) {
      deleteWiFiCredential(doc["ssid"].as<String>());
      server.send(200, "text/plain", "Deleted");
    } else {
      server.send(400, "text/plain", "Invalid request");
    }
  });

  server.on("/api/toggle-autoconnect", HTTP_POST, []() {
    String body = server.arg("plain");
    DynamicJsonDocument doc(128);
    if (!deserializeJson(doc, body) && doc.containsKey("enabled")) {
      autoConnectEnabled = doc["enabled"];
      preferences.putBool("autoconnect", autoConnectEnabled);
    }
    server.send(200, "application/json", "{\"enabled\":" + String(autoConnectEnabled ? "true" : "false") + "}");
  });

  server.on("/api/toggle-save-on-connect", HTTP_POST, []() {
    String body = server.arg("plain");
    DynamicJsonDocument doc(128);
    if (!deserializeJson(doc, body) && doc.containsKey("enabled")) {
      saveOnConnectEnabled = doc["enabled"];
      preferences.putBool("save_creds", saveOnConnectEnabled);
    }
    server.send(200, "application/json", "{\"enabled\":" + String(saveOnConnectEnabled ? "true" : "false") + "}");
  });

  server.begin();
  Serial.println("Web server started");
  logDebug("Web server started on port 80");
}

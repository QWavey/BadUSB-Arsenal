#include "UpdateManager.h"
#include "FSManager.h"
#include "LEDManager.h"
#include <SD.h>
#include <Update.h>
#include <ArduinoJson.h>

static File g_pkgUploadFile;

// ---- Upload: stream the raw .espkg to the SD card --------------------------
void handleUpdatePackageUpload() {
  HTTPUpload& up = server.upload();
  if (up.status == UPLOAD_FILE_START) {
    // Guard against a concurrent second upload trying to reset state mid-apply.
    if (updateApplying) {
      updateStatus = "Rejected: another update is already being applied";
      return;
    }
    // Refuse if a DuckyScript is running — MSC/SD collision + partial write risk.
    if (scriptRunning) {
      updateStatus = "Rejected: script is running — stop it first";
      return;
    }
    updateApplying = false;
    updateProgress = 0;
    updateStatus = "Receiving package...";
    if (SD.exists(ESPKG_TMP_PATH)) SD.remove(ESPKG_TMP_PATH);
    g_pkgUploadFile = SD.open(ESPKG_TMP_PATH, FILE_WRITE);
    // v4.4: if SD is full/write-protected the open silently fails and every
    // subsequent chunk is dropped, leaving updateStatus stuck on
    // "Package received" while nothing was actually stored.
    if (!g_pkgUploadFile) {
      updateStatus = "Error: cannot open package temp file on SD";
      Serial.println("[UPDATE] SD.open(ESPKG_TMP_PATH, WRITE) failed");
    }
  } else if (up.status == UPLOAD_FILE_WRITE) {
    if (g_pkgUploadFile) g_pkgUploadFile.write(up.buf, up.currentSize);
  } else if (up.status == UPLOAD_FILE_END) {
    if (g_pkgUploadFile) g_pkgUploadFile.close();
    // Only mark "received" if the temp file was actually opened & written.
    if (SD.exists(ESPKG_TMP_PATH)) updateStatus = "Package received";
  }
}

// ---- POST finalizer: arm the apply (runs from loop) ------------------------
void handleUpdatePackagePost() {
  if (!sdCardPresent || !SD.exists(ESPKG_TMP_PATH)) {
    server.send(400, "application/json", "{\"ok\":false,\"error\":\"no package received\"}");
    return;
  }
  // Sanity-check the uploaded blob BEFORE arming the apply: verify magic and
  // that it's at least large enough to hold the header + a plausible manifest.
  File pkg = SD.open(ESPKG_TMP_PATH, FILE_READ);
  if (!pkg) {
    server.send(500, "application/json", "{\"ok\":false,\"error\":\"cannot reopen package\"}");
    return;
  }
  uint8_t hdr[10];
  size_t hdrRead = pkg.read(hdr, 10);
  size_t pkgSize = pkg.size();
  pkg.close();
  if (hdrRead != 10 ||
      hdr[0]!='E' || hdr[1]!='S' || hdr[2]!='P' ||
      hdr[3]!='K' || hdr[4]!='G' || hdr[5]!=0x01) {
    SD.remove(ESPKG_TMP_PATH);
    server.send(400, "application/json", "{\"ok\":false,\"error\":\"not a valid .espkg (bad magic)\"}");
    return;
  }
  uint32_t manLen = (uint32_t)hdr[6] | ((uint32_t)hdr[7]<<8) |
                    ((uint32_t)hdr[8]<<16) | ((uint32_t)hdr[9]<<24);
  if (manLen == 0 || manLen > 8192 || pkgSize < (size_t)(10 + manLen)) {
    SD.remove(ESPKG_TMP_PATH);
    server.send(400, "application/json", "{\"ok\":false,\"error\":\"package truncated or manifest length invalid\"}");
    return;
  }

  updatePackageReady = true;   // applied on the next loop() so progress is pollable
  updateStatus = "Queued";
  updateProgress = 0;
  server.send(200, "application/json", "{\"ok\":true,\"status\":\"applying\",\"packageBytes\":" + String((unsigned long)pkgSize) + "}");
}

// ---- GET /api/update-status ------------------------------------------------
void handleUpdateStatus() {
  DynamicJsonDocument doc(256);
  doc["progress"] = updateProgress;
  doc["status"] = updateStatus;
  doc["applying"] = updateApplying;
  String out; serializeJson(doc, out);
  server.sendHeader("Cache-Control", "no-cache, no-store, must-revalidate");
  server.send(200, "application/json", out);
}

// Copy `len` bytes from the (already-positioned) package into `path` on SD.
// Writes to a temp file and only renames into place on full success, so a
// truncated upload can never leave a half-written /index.html behind.
static bool extractFile(File& src, const String& path, uint32_t len,
                        uint32_t totalBytes, uint32_t& doneBytes) {
  String tmp = path + ".new";
  if (SD.exists(tmp)) SD.remove(tmp);
  File dst = SD.open(tmp, FILE_WRITE);
  if (!dst) return false;
  // v4.16 perf: 4 KB buffer (was 512 B) - cuts SPI round-trips 8x on
  // ~200 KB web assets. `static` keeps it off the small helper's stack.
  static uint8_t buf[4096];
  uint32_t remaining = len;
  while (remaining > 0) {
    size_t chunk = remaining > sizeof(buf) ? sizeof(buf) : remaining;
    int r = src.read(buf, chunk);
    if (r <= 0) { dst.close(); SD.remove(tmp); return false; }
    if (dst.write(buf, r) != (size_t)r) { dst.close(); SD.remove(tmp); return false; }
    remaining -= r;
    doneBytes += r;
    updateProgress = (int)((uint64_t)doneBytes * 100ULL / (totalBytes ? totalBytes : 1));
    // v4.11: removed server.handleClient() call here to prevent reentrancy
    // during the SD extract phase (BUG-9). The progress poll will just see
    // a slightly stale value until the next iteration - much better than
    // corrupting the extract with a concurrent mutating handler.
  }
  dst.close();
  // atomically swap into place
  if (SD.exists(path)) SD.remove(path);
  if (!SD.rename(tmp, path)) { SD.remove(tmp); return false; }
  return true;
}

// ---- Apply the armed package (called from loop) ----------------------------
void processPendingUpdate() {
  if (!updatePackageReady) return;
  updatePackageReady = false;
  updateApplying = true;
  updateProgress = 0;
  updateStatus = "Opening package...";
  setLEDMode(6);   // fast-blink BLUE while the update is being applied
                    // (mode 1 was actually green; users reported it looked wrong)

  File pkg = SD.open(ESPKG_TMP_PATH, FILE_READ);
  if (!pkg) { updateStatus = "Error: cannot open package"; setLEDMode(4); updateApplying = false; return; }

  // magic: 'E','S','P','K','G',0x01
  uint8_t magic[6];
  if (pkg.read(magic, 6) != 6 || magic[0] != 'E' || magic[1] != 'S' || magic[2] != 'P' ||
      magic[3] != 'K' || magic[4] != 'G' || magic[5] != 0x01) {
    updateStatus = "Error: not a valid .espkg"; setLEDMode(4); pkg.close(); updateApplying = false; return;
  }

  // manifest length (uint32 little-endian)
  uint8_t lenb[4];
  if (pkg.read(lenb, 4) != 4) { updateStatus = "Error: truncated header"; setLEDMode(4); pkg.close(); updateApplying = false; return; }
  uint32_t manLen = (uint32_t)lenb[0] | ((uint32_t)lenb[1] << 8) |
                    ((uint32_t)lenb[2] << 16) | ((uint32_t)lenb[3] << 24);
  if (manLen == 0 || manLen > 8192) { updateStatus = "Error: bad manifest size"; setLEDMode(4); pkg.close(); updateApplying = false; return; }

  // manifest JSON (heap — never put multi-KB on the 8KB loop stack)
  char* manBuf = (char*)malloc(manLen + 1);
  if (!manBuf) { updateStatus = "Error: out of memory"; setLEDMode(4); pkg.close(); updateApplying = false; return; }
  if (pkg.read((uint8_t*)manBuf, manLen) != (int)manLen) {
    updateStatus = "Error: truncated manifest"; setLEDMode(4); free(manBuf); pkg.close(); updateApplying = false; return;
  }
  manBuf[manLen] = 0;
  DynamicJsonDocument man(8192);
  DeserializationError jerr = deserializeJson(man, manBuf);
  free(manBuf);
  if (jerr) { updateStatus = "Error: bad manifest JSON"; setLEDMode(4); pkg.close(); updateApplying = false; return; }

  // total bytes (for the progress bar)
  JsonArray sdArr = man["sd"].as<JsonArray>();
  uint64_t totalBytes = 0;
  for (JsonObject f : sdArr) totalBytes += (uint32_t)(f["size"] | 0);
  uint32_t fwSize = man["fw"]["size"] | 0;
  totalBytes += fwSize;
  uint32_t doneBytes = 0;

  // Payloads begin right after the manifest — the cursor is already there.
  // 1) website / SD files FIRST (safe, no reboot)
  for (JsonObject f : sdArr) {
    String path = f["path"].as<String>();
    if (!path.startsWith("/")) path = "/" + path;
    uint32_t sz = f["size"] | 0;
    // v4.11 SECURITY: reject `..` and quarantined system paths in the
    // manifest. Without this, a crafted .espkg with `"path":"/reboot_script.txt"`
    // overwrites files that the boot flow auto-executes -> persistent
    // arbitrary code exec from anyone who can POST /api/update-package.
    // Also block absolute-path escapes.
    bool badPath = (path.indexOf("..") >= 0) ||
                   (path == "/reboot_script.txt") ||
                   (path == "/temp_resume.txt")   ||
                   (path == "/update.espkg");
    if (badPath) {
      updateStatus = "Error: rejected unsafe manifest path " + path;
      Serial.println("[UPDATE] " + updateStatus);
      setLEDMode(4); pkg.close(); updateApplying = false; return;
    }
    updateStatus = "Writing " + path;
    // v4.11: DON'T call server.handleClient() here - reentrancy risk.
    // Any concurrent /api/live-type / /api/save / /api/factory-reset that
    // fires during the extract would corrupt the in-progress state. The
    // status endpoint just sees a slightly stale progress until the next
    // extract iteration; that's fine.
    if (!extractFile(pkg, path, sz, (uint32_t)totalBytes, doneBytes)) {
      updateStatus = "Error writing " + path; setLEDMode(4); pkg.close(); updateApplying = false; return;
    }
  }

  // 2) firmware LAST (OTA — reboots on success)
  if (fwSize > 0) {
    updateStatus = "Flashing firmware...";
    server.handleClient();
    if (!Update.begin(fwSize, U_FLASH)) {
      updateStatus = String("Error: OTA begin (") + Update.errorString() + ")";
      setLEDMode(4); pkg.close(); updateApplying = false; return;
    }
    uint8_t buf[1024];
    uint32_t remaining = fwSize;
    while (remaining > 0) {
      size_t chunk = remaining > sizeof(buf) ? sizeof(buf) : remaining;
      int r = pkg.read(buf, chunk);
      if (r <= 0) { updateStatus = "Error: truncated firmware"; setLEDMode(4); Update.abort(); pkg.close(); updateApplying = false; return; }
      if (Update.write(buf, r) != (size_t)r) { updateStatus = "Error: OTA write failed"; setLEDMode(4); Update.abort(); pkg.close(); updateApplying = false; return; }
      remaining -= r;
      doneBytes += r;
      updateProgress = (int)((uint64_t)doneBytes * 100ULL / (totalBytes ? totalBytes : 1));
      // v4.4: removed the server.handleClient() call from inside the OTA
      // write loop. Update.write() is NOT reentrant — if a concurrent /api/*
      // handler fired here and (say) opened SD.open, the OTA partition could
      // be left half-written and unbootable. The status endpoint just sees a
      // stale progress value until the OTA loop yields; that's fine.
    }
    if (!Update.end(true)) {
      updateStatus = String("Error: OTA end (") + Update.errorString() + ")";
      setLEDMode(4); pkg.close(); updateApplying = false; return;
    }
  }

  pkg.close();
  SD.remove(ESPKG_TMP_PATH);
  updateProgress = 100;
  updateStatus = fwSize > 0 ? "Done — rebooting..." : "Website updated";
  updateApplying = false;
  setLED(0, 0, 255);          // solid blue: update finished successfully
  server.handleClient();   // flush the final status to any poller
  delay(800);
  if (fwSize > 0) ESP.restart();
}

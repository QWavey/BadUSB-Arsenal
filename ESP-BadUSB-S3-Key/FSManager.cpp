#include "FSManager.h"
#include <HTTPClient.h>
#include <WiFi.h>
#include "LEDManager.h"
#include "LogManager.h"
#include <ArduinoJson.h>
#include "esp32-hal-tinyusb.h"    // tud_ready() for the MSC-safe SD check (v4.4)

bool initSDCard() {
  SPI.begin(SD_SCK_PIN, SD_MISO_PIN, SD_MOSI_PIN, SD_CS_PIN);

  // Improved SD card initialization with retry logic.
  // Track mount success inside the loop instead of unconditionally re-calling
  // SD.begin() outside it — that second call is idempotent on SPI SD (returns
  // true because _card is still set) but does an unnecessary re-probe of the
  // SPI bus on every boot on the happy path.
  bool mounted = false;
  for (int i = 0; i < 3; i++) {
    if (SD.begin(SD_CS_PIN)) { mounted = true; break; }
    delay(100);
  }
  if (!mounted) {
    Serial.println("Card Mount Failed");
    return false;
  }

  uint8_t cardType = SD.cardType();
  if (cardType == CARD_NONE) {
    Serial.println("No SD card attached");
    return false;
  }

  // Create necessary directories with error checking
  if (!SD.exists(DIR_LANGUAGES)) {
    if (!SD.mkdir(DIR_LANGUAGES)) {
      Serial.println("Failed to create languages directory");
      return false;
    }
  }
  if (!SD.exists(DIR_SCRIPTS)) {
    if (!SD.mkdir(DIR_SCRIPTS)) {
      Serial.println("Failed to create scripts directory");
      return false;
    }
  }
  if (!SD.exists(DIR_LOGS)) {
    if (!SD.mkdir(DIR_LOGS)) {
      Serial.println("Failed to create logs directory");
    }
  }
  if (!SD.exists(DIR_UPLOADS)) {
    if (!SD.mkdir(DIR_UPLOADS)) {
      Serial.println("Failed to create uploads directory");
    }
  }

  Serial.println("SD Card initialized successfully");
  return true;
}

void checkSDCard() {
  bool cardDetected = false;

  // v4.4 rewrite: the old code wrote and removed `/.sdtest` on every tick.
  // That's 3-4 SPI transactions PER SECOND, wearing the card and racing the
  // MSC callbacks (which do raw sector reads/writes from another task).
  //
  // Priorities now:
  //   1) If a host has mounted the USB drive (tud_ready), NEVER poke the FAT
  //      layer — a filesystem-level op while MSC is mid-transfer corrupts FAT
  //      metadata. Trust the last known state and defer.
  //   2) Fast, no-I/O detection via SD.cardType() — reads a cached SDMMC/SPI
  //      register. Only fall back to a write-probe once every 30 seconds, or
  //      when cardType() is inconclusive.
  static uint32_t lastWriteProbe = 0;
  const uint32_t kProbeIntervalMs = 30000;

  if (tud_ready()) {
    // Host is actively using the drive — don't touch it.
    return;
  }

  // Cheap path: card type register.
  uint8_t ct = SD.cardType();
  if (ct != CARD_NONE && ct != CARD_UNKNOWN) {
    cardDetected = true;
  } else if (millis() - lastWriteProbe > kProbeIntervalMs) {
    lastWriteProbe = millis();
    // Only reach this when cardType() reports NONE/UNKNOWN — try a real write
    // probe up to 3 times, matching the pre-v4.4 behaviour.
    for (int i = 0; i < 3; i++) {
      File testFile = SD.open("/.sdtest", FILE_WRITE);
      if (testFile) {
        testFile.print("t");
        testFile.close();
        SD.remove("/.sdtest");
        cardDetected = true;
        break;
      }
      delay(10);
    }
  } else {
    // Recently probed with a NONE result — trust the last known state.
    cardDetected = sdCardPresent;
  }

  if (cardDetected && !sdCardPresent) {
    Serial.println("SD Card inserted");
    sdCardPresent = true;
    if (!scriptRunning) {
      setLEDMode(0);
      setLED(0, 255, 0);
    }
    loadAvailableLanguages();
    loadAvailableScripts();
    logCommand("SD_CARD", "SD card inserted");
  } else if (!cardDetected && sdCardPresent) {
    Serial.println("SD Card removed - ERROR STATE");
    sdCardPresent = false;
    setLEDMode(2);
    logCommand("SD_CARD", "SD card removed - ERROR");
  } else if (!cardDetected && !sdCardPresent) {
    if (!scriptRunning) {
      ledMode = 2;
    }
  } else if (cardDetected && sdCardPresent) {
    if (!scriptRunning && ledMode != 0) {
      setLEDMode(0);
      setLED(0, 255, 0);
    }
  }
}

void loadAvailableLanguages() {
  if (!sdCardPresent) {
    Serial.println("[FS] Language discovery skipped: SD not present");
    return;
  }

  Serial.println("[FS] Scanning for languages in: " + String(DIR_LANGUAGES));
  File root = SD.open(DIR_LANGUAGES);
  if (!root) {
    Serial.println("[FS] FAILED to open languages directory: " + String(DIR_LANGUAGES));
    logDebug("Discovery: Failed to open " + String(DIR_LANGUAGES));
    return;
  }

  availableLanguages.clear();

  File file = root.openNextFile();
  while (file) {
    if (!file.isDirectory()) {
      String fileName = file.name();
      if (fileName.endsWith(".json")) {
        String langName = fileName.substring(0, fileName.lastIndexOf('.'));
        availableLanguages.push_back(langName);
        Serial.println("[FS] Discovered language: " + langName);
      } else {
        Serial.println("[FS] Ignoring non-JSON file: " + fileName);
      }
    }
    file = root.openNextFile();
  }
  root.close();
  Serial.println("[FS] Discovery complete. Total languages found: " + String(availableLanguages.size()));
}

void loadAvailableScripts() {
  if (!sdCardPresent) return;

  File root = SD.open(DIR_SCRIPTS);
  if (!root) {
    Serial.println("Failed to open scripts directory");
    return;
  }

  availableScripts.clear();

  File file = root.openNextFile();
  while (file) {
    if (!file.isDirectory()) {
      String fileName = file.name();
      if (fileName.endsWith(".txt")) {
        availableScripts.push_back(fileName);
        Serial.println("Found script: " + fileName);
      }
    }
    file = root.openNextFile();
  }
  root.close();
}

bool loadLanguage(String language) {
  if (!sdCardPresent) return false;

  String filePath = String(DIR_LANGUAGES) + "/" + language + ".json";
  File file = SD.open(filePath);

  if (!file) {
    Serial.println("Failed to open language file: " + filePath);
    lastError = "Language file not found: " + language;
    errorCount++;
    return false;
  }

  DynamicJsonDocument doc(16384);
  DeserializationError error = deserializeJson(doc, file);
  file.close();

  if (error) {
    Serial.println("Failed to parse JSON: " + String(error.c_str()));
    lastError = "JSON parse error: " + String(error.c_str());
    errorCount++;
    return false;
  }

  currentKeymap.clear();

  for (JsonPair kv : doc.as<JsonObject>()) {
    String key = kv.key().c_str();
    if (!key.startsWith("comment") && !key.startsWith("_comment")) {
      String value = kv.value().as<String>();
      currentKeymap[key] = value;
    }
  }

  currentLanguage = language;
  Serial.println("Loaded language: " + language);
  return true;
}

String loadScript(String filename) {
  if (!sdCardPresent) return "";

  String filePath = String(DIR_SCRIPTS) + "/" + filename;
  File file = SD.open(filePath);

  if (!file) {
    Serial.println("Failed to open script file: " + filePath);
    lastError = "Script file not found: " + filename;
    errorCount++;
    return "";
  }

  String scriptContent = file.readString();
  file.close();

  Serial.println("Loaded script: " + filename);
  return scriptContent;
}

bool saveScript(String filename, String content) {
  if (!sdCardPresent) return false;

  if (!filename.endsWith(".txt")) {
    filename += ".txt";
  }

  String filePath = String(DIR_SCRIPTS) + "/" + filename;

  if (SD.exists(filePath)) {
    SD.remove(filePath);
  }

  File file = SD.open(filePath, FILE_WRITE);
  if (!file) {
    Serial.println("Failed to create script file: " + filePath);
    lastError = "Failed to save script: " + filename;
    errorCount++;
    return false;
  }

  size_t bytesWritten = file.print(content);
  file.close();

  if (bytesWritten > 0) {
    Serial.println("Script saved: " + filename + " (" + String(bytesWritten) + " bytes)");
    loadAvailableScripts();
    return true;
  } else {
    Serial.println("Failed to write script: " + filename);
    lastError = "Failed to write script: " + filename;
    errorCount++;
    return false;
  }
}

bool deleteScript(String filename) {
  if (!sdCardPresent) return false;

  String filePath = String(DIR_SCRIPTS) + "/" + filename;

  if (SD.exists(filePath)) {
    if (SD.remove(filePath)) {
      Serial.println("Script deleted: " + filename);
      loadAvailableScripts();
      return true;
    }
  }

  Serial.println("Failed to delete script: " + filename);
  lastError = "Failed to delete script: " + filename;
  errorCount++;
  return false;
}

void changeDirectory(String path) {
  if (!sdCardPresent) {
    Serial.println("SD card not present");
    return;
  }

  if (path.startsWith("./")) {
    path = currentDirectory + path.substring(2);
  } else if (path == "..") {
    if (currentDirectory != "/") {
      int lastSlash = currentDirectory.lastIndexOf('/');
      if (lastSlash == 0) {
        currentDirectory = "/";
      } else {
        currentDirectory = currentDirectory.substring(0, lastSlash);
      }
    }
    return;
  } else if (!path.startsWith("/")) {
    if (currentDirectory.endsWith("/")) {
      path = currentDirectory + path;
    } else {
      path = currentDirectory + "/" + path;
    }
  }

  path.replace("//", "/");
  
  if (SD.exists(path)) {
    File dir = SD.open(path);
    if (dir && dir.isDirectory()) {
      currentDirectory = path;
      if (!currentDirectory.endsWith("/") && currentDirectory != "/") {
        currentDirectory += "/";
      }
      Serial.println("Changed directory to: " + currentDirectory);
    } else {
      Serial.println("Not a directory: " + path);
    }
    dir.close();
  } else {
    Serial.println("Directory not found: " + path);
  }
}

void useFile(String filePath) {
  if (!sdCardPresent) {
    Serial.println("SD card not present");
    return;
  }

  if (!filePath.startsWith("/")) {
    filePath = currentDirectory + filePath;
  }

  if (SD.exists(filePath)) {
    selectedFiles.clear();
    selectedFiles.push_back(filePath);
    Serial.println("File ready to use: " + filePath);
    variables["SELECTED_FILE"] = filePath;
  } else {
    Serial.println("File not found: " + filePath);
  }
}

void useFiles(std::vector<String> filePaths) {
  if (!sdCardPresent) {
    Serial.println("SD card not present");
    return;
  }

  selectedFiles.clear();
  for (String filePath : filePaths) {
    if (!filePath.startsWith("/")) {
      filePath = currentDirectory + filePath;
    }

    if (SD.exists(filePath)) {
      selectedFiles.push_back(filePath);
      Serial.println("File ready to use: " + filePath);
    } else {
      Serial.println("File not found: " + filePath);
    }
  }
  
  if (!selectedFiles.empty()) {
    variables["SELECTED_FILES"] = String(selectedFiles.size());
  }
}

void copyFile(String sourcePath, String destPath) {
  if (!sdCardPresent) {
    Serial.println("SD card not present");
    return;
  }

  if (sourcePath == "" && !selectedFiles.empty()) {
    sourcePath = selectedFiles[0];
  }

  if (!sourcePath.startsWith("/")) {
    sourcePath = currentDirectory + sourcePath;
  }
  if (destPath != "" && !destPath.startsWith("/")) {
    destPath = currentDirectory + destPath;
  }

  if (destPath == "") {
    copiedFilePath = sourcePath;
    fileCopied = true;
    fileCut = false;
    Serial.println("File marked for copy: " + sourcePath);
  } else {
    if (copySDFile(sourcePath, destPath)) {
      Serial.println("File copied: " + sourcePath + " -> " + destPath);
    } else {
      Serial.println("Failed to copy file: " + sourcePath);
    }
  }
}

void cutFile(String sourcePath, String destPath) {
  if (!sdCardPresent) {
    Serial.println("SD card not present");
    return;
  }

  if (sourcePath == "" && !selectedFiles.empty()) {
    sourcePath = selectedFiles[0];
  }

  if (!sourcePath.startsWith("/")) {
    sourcePath = currentDirectory + sourcePath;
  }
  if (destPath != "" && !destPath.startsWith("/")) {
    destPath = currentDirectory + destPath;
  }

  if (destPath == "") {
    cutFilePath = sourcePath;
    fileCut = true;
    fileCopied = false;
    Serial.println("File marked for cut: " + sourcePath);
  } else {
    if (moveSDFile(sourcePath, destPath)) {
      Serial.println("File moved: " + sourcePath + " -> " + destPath);
    } else {
      Serial.println("Failed to move file: " + sourcePath);
    }
  }
}

void pasteFile(String destPath) {
  if (!sdCardPresent) {
    Serial.println("SD card not present");
    return;
  }

  if (destPath != "" && !destPath.startsWith("/")) {
    destPath = currentDirectory + destPath;
  }

  if (destPath == "") {
    destPath = currentDirectory;
  }

  if (fileCopied && copiedFilePath != "") {
    String fileName = getFileNameFromPath(copiedFilePath);
    String destFile = destPath;
    if (!destPath.endsWith("/")) {
      destFile += "/";
    }
    destFile += fileName;

    if (copySDFile(copiedFilePath, destFile)) {
      Serial.println("File pasted: " + copiedFilePath + " -> " + destFile);
    } else {
      Serial.println("Failed to paste file: " + copiedFilePath);
    }
  } else if (fileCut && cutFilePath != "") {
    String fileName = getFileNameFromPath(cutFilePath);
    String destFile = destPath;
    if (!destPath.endsWith("/")) {
      destFile += "/";
    }
    destFile += fileName;

    if (moveSDFile(cutFilePath, destFile)) {
      Serial.println("File moved: " + cutFilePath + " -> " + destFile);
      cutFilePath = "";
      fileCut = false;
    } else {
      Serial.println("Failed to move file: " + cutFilePath);
    }
  } else {
    Serial.println("No file to paste");
  }
}

String getFileNameFromPath(String path) {
  int lastSlash = path.lastIndexOf('/');
  if (lastSlash != -1) {
    return path.substring(lastSlash + 1);
  }
  return path;
}

String getParentDirectory(String path) {
  if (path == "/") return "/";
  int lastSlash = path.lastIndexOf('/');
  if (lastSlash == 0) return "/";
  return path.substring(0, lastSlash);
}

bool copySDFile(String sourcePath, String destPath) {
  File sourceFile = SD.open(sourcePath, FILE_READ);
  if (!sourceFile) {
    Serial.println("Failed to open source file: " + sourcePath);
    return false;
  }

  File destTest = SD.open(destPath);
  if (destTest && destTest.isDirectory()) {
    if (!destPath.endsWith("/")) {
      destPath += "/";
    }
    destPath += getFileNameFromPath(sourcePath);
  }
  destTest.close();

  if (SD.exists(destPath)) {
    SD.remove(destPath);
  }

  File destFile = SD.open(destPath, FILE_WRITE);
  if (!destFile) {
    Serial.println("Failed to create destination file: " + destPath);
    sourceFile.close();
    return false;
  }

  uint8_t buffer[512];
  size_t bytesRead;
  bool writeFailed = false;
  while ((bytesRead = sourceFile.read(buffer, sizeof(buffer))) > 0) {
    // v4.4: check destFile.write's return value — SD.write returns the number
    // of bytes actually written; when the card is full or write-protected it
    // returns 0 or a short count. Without this check, moveSDFile deletes the
    // source after a "successful" 0-byte copy → DATA LOSS.
    if (destFile.write(buffer, bytesRead) != bytesRead) {
      Serial.println("[FS] copy: write failed (card full or write-protected?): " + destPath);
      writeFailed = true;
      break;
    }
  }

  sourceFile.close();
  destFile.close();

  if (writeFailed) {
    SD.remove(destPath);   // don't leave a torn/partial file behind
    return false;
  }
  return true;
}

bool moveSDFile(String sourcePath, String destPath) {
  if (copySDFile(sourcePath, destPath)) {
    if (SD.remove(sourcePath)) {
      return true;
    } else {
      // v4.11 FIX: if the source remove fails we now roll back the copy so
      // callers (cutFile / pasteFile) don't end up with two copies AND a
      // "Failed to move" error. Better to fail cleanly.
      Serial.println("Failed to remove source file after copy: " + sourcePath +
                     " - rolling back destination");
      SD.remove(destPath);
      return false;
    }
  }
  return false;
}

bool deleteDirectory(String path) {
  File dir = SD.open(path);
  if (!dir) {
    return false;
  }

  if (!dir.isDirectory()) {
    dir.close();
    return SD.remove(path);
  }

  dir.rewindDirectory();
  File file = dir.openNextFile();
  while (file) {
    // v4.4: ESP32 SD library's file.name() returns the FULL absolute path
    // (e.g. "/scripts/foo.txt"), not just the leaf. The old code did
    //   filepath = path + "/" + file.name()
    // which produced "/scripts//scripts/foo.txt" — SD.remove failed and the
    // whole tree walk aborted, leaving handleDeleteFile with a 500. Strip
    // everything up to and including the last '/' from file.name() first.
    String fname = String(file.name());
    int lastSlash = fname.lastIndexOf('/');
    if (lastSlash >= 0) fname = fname.substring(lastSlash + 1);
    String filepath = path;
    if (!filepath.endsWith("/")) filepath += "/";
    filepath += fname;
    if (file.isDirectory()) {
      if (!deleteDirectory(filepath)) {
        dir.close();
        return false;
      }
    } else {
      if (!SD.remove(filepath)) {
        dir.close();
        return false;
      }
    }
    file = dir.openNextFile();
  }
  dir.close();

  return SD.rmdir(path);
}

bool downloadFileFromURL(String url, String path) {
  if (WiFi.status() != WL_CONNECTED) {
    Serial.println("WiFi not connected - cannot download file");
    return false;
  }

  // Ensure parent directory exists
  String parentDir = getParentDirectory(path);
  if (parentDir != "" && parentDir != "/") {
    ensureDirectoryExists(parentDir);
  }

  HTTPClient http;
  http.begin(url);
  http.setFollowRedirects(HTTPC_STRICT_FOLLOW_REDIRECTS);
  http.setTimeout(10000); // 10s timeout
  
  int httpCode = http.GET();

  // v4.17-post-hunt BUG #6 fix: don't treat 301/302 as success. The old code
  // saved the redirect body (usually an HTML "Moved" page) as if it was the
  // downloaded file. HTTPC_STRICT_FOLLOW_REDIRECTS should handle real
  // redirects internally; anything reaching us as a bare 301/302 means the
  // redirect wasn't followed and we must NOT persist that HTML.
  if (httpCode == HTTP_CODE_OK) {
    int len = http.getSize();
    WiFiClient * stream = http.getStreamPtr();

    File file = SD.open(path, FILE_WRITE);
    if (!file) {
      Serial.println("Failed to open file for writing: " + path);
      http.end();
      return false;
    }

    uint8_t buff[512] = { 0 };
    // v4.4: idle timeout. http.setTimeout(10000) above only covers the request
    // stage — once we're streaming, stream->available() can return 0 forever
    // if the peer stalls without closing, hanging the DuckyScript task and the
    // whole loop() thread indefinitely. Break out after 15 s of no bytes.
    unsigned long lastDataMs = millis();
    const unsigned long IDLE_TIMEOUT_MS = 15000;
    while (http.connected() && (len > 0 || len == -1)) {
      size_t size = stream->available();
      if (size) {
        int c = stream->readBytes(buff, ((size > sizeof(buff)) ? sizeof(buff) : size));
        if (c > 0) {
          file.write(buff, c);
          if (len > 0) len -= c;
          lastDataMs = millis();
        }
      } else if (millis() - lastDataMs > IDLE_TIMEOUT_MS) {
        Serial.println("[HTTP] download idle timeout — aborting");
        break;
      }
      delay(1);
    }

    file.close();
    http.end();
    Serial.println("File downloaded successfully: " + path);
    return true;
  } else {
    Serial.printf("[HTTP] GET... failed, error: %s (code %d)\n", http.errorToString(httpCode).c_str(), httpCode);
    http.end();
    return false;
  }
}

bool ensureDirectoryExists(String path) {
  if (SD.exists(path)) return true;
  
  // Recursively create parent if needed
  String parent = getParentDirectory(path);
  if (parent != "" && parent != "/" && !SD.exists(parent)) {
    ensureDirectoryExists(parent);
  }
  
  Serial.println("Creating directory: " + path);
  return SD.mkdir(path);
}
bool uploadFileToServer(String localPath, String remoteUrl) {
  if (WiFi.status() != WL_CONNECTED) {
    Serial.println("[FS] WiFi not connected - cannot upload file");
    return false;
  }

  if (!SD.exists(localPath)) {
    Serial.println("[FS] Local file not found: " + localPath);
    return false;
  }

  File file = SD.open(localPath, FILE_READ);
  if (!file) {
    Serial.println("[FS] Failed to open local file for reading: " + localPath);
    return false;
  }

  HTTPClient http;
  http.begin(remoteUrl);
  
  // Use a simple POST with the file stream
  int httpCode = http.sendRequest("POST", &file, file.size());

  if (httpCode > 0) {
    Serial.printf("[HTTP] POST... code: %d\n", httpCode);
    String payload = http.getString();
    Serial.println("[HTTP] Response: " + payload);
    file.close();
    http.end();
    return (httpCode == HTTP_CODE_OK || httpCode == HTTP_CODE_CREATED);
  } else {
    Serial.printf("[HTTP] POST... failed, error: %s\n", http.errorToString(httpCode).c_str());
    file.close();
    http.end();
    return false;
  }
}

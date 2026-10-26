#include "WebServerHandlers.h"
#include "FSManager.h"
#include "DuckyInterpreter.h"
#include "WiFiManager.h"
#include "LogManager.h"
#include "LEDManager.h"
#include <ArduinoJson.h>

// Set when the current upload fails; the POST reply reads it.
bool uploadFailed = false;
// Upload in progress: the final destination and the ".part" file the data is
// written to. An aborted upload removes exactly that ".part" file.
static String s_uploadPath;
static String s_uploadTmp;

// Makes a file name safe inside a quoted HTTP header value.
static String headerSafeName(const String& in) {
  String out;
  for (size_t i = 0; i < in.length(); ++i) {
    char c = in[i];
    if (c == '"' || c == '\\' || (unsigned char)c < 0x20 || c == 0x7F) continue;
    out += c;
  }
  return out;
}

// Path checks for client-supplied paths. Returns 0 when the path is usable,
// 400 when it is unsafe, and 403 when it names a Wi-Fi credential file.
// checkRootPath resolves relative names from "/" (the file endpoints do this);
// checkCwdPath resolves them from the current directory (use/copy/cut/paste do).
static int checkRootPath(const String& raw, String& canon) {
  String p = raw.startsWith("/") ? raw : ("/" + raw);
  if (!canonicalSdPath(p, canon)) return 400;
  return isSecretSdPath(canon) ? 403 : 0;
}

static int checkCwdPath(const String& raw, String& canon) {
  String p = raw.startsWith("/") ? raw : (currentDirectory + raw);
  if (!canonicalSdPath(p, canon)) return 400;
  return isSecretSdPath(canon) ? 403 : 0;
}

void handleFileUpload() {
  HTTPUpload& upload = server.upload();

  if (upload.status == UPLOAD_FILE_START) {
    uploadFilename = upload.filename;
    uploadFailed = false;
    s_uploadPath = "";
    s_uploadTmp = "";
    Serial.println("File upload start: " + uploadFilename);

    // v4.11 SECURITY: reject path-traversal in filenames. Without this a
    // client could POST `filename=../reboot_script.txt` and land the file at
    // `/scripts/../reboot_script.txt` == `/reboot_script.txt`, which setup()
    // auto-executes on next boot -> persistent DuckyScript RCE for anyone
    // on the open AP. Enforce leaf-only names (no separators, no control
    // characters, no leading dot).
    if (!isSafeLeafName(uploadFilename) || uploadFilename.indexOf("..") >= 0) {
      Serial.println("[UPLOAD] Rejected unsafe filename: " + uploadFilename);
      // Bug-hunt #1: also CLOSE the previous uploadFile so subsequent
      // UPLOAD_FILE_WRITE chunks — which the ESP32 WebServer library keeps
      // delivering after START — can't write into whichever file was open
      // from a prior (possibly aborted) upload. Without this a malicious
      // upload could tail-append its data to an earlier legitimate file.
      if (uploadFile) uploadFile.close();
      uploadFilename = "";
      uploadFailed = true;
      return;
    }

    String uploadPath;
    if (uploadFilename.endsWith(".txt")) {
      uploadPath = String(DIR_SCRIPTS) + "/" + uploadFilename;
    } else if (uploadFilename.endsWith(".json")) {
      uploadPath = String(DIR_LANGUAGES) + "/" + uploadFilename;
    } else {
      uploadPath = String(DIR_UPLOADS) + "/" + uploadFilename;
    }

    if (uploadFile) {
      uploadFile.close();
    }

    // Write to a ".part" sibling and move it over the real name only when the
    // upload completes (see END). The old file stays until then, and an
    // aborted upload never leaves a partial script or language file behind.
    s_uploadPath = uploadPath;
    s_uploadTmp = uploadPath + ".part";
    if (SD.exists(s_uploadTmp)) SD.remove(s_uploadTmp);

    uploadFile = SD.open(s_uploadTmp, FILE_WRITE);
    if (!uploadFile) {
      Serial.println("Failed to create file: " + s_uploadTmp);
      uploadFailed = true;
      s_uploadPath = "";
      s_uploadTmp = "";
      return;
    }

  } else if (upload.status == UPLOAD_FILE_WRITE) {
    // Bug-hunt #1: gate writes on both an open file AND a non-empty
    // uploadFilename. START clears uploadFilename on reject; without the
    // second guard a WRITE that arrives after a rejected START still
    // trickles into whichever file was open before.
    if (uploadFile && uploadFilename.length() > 0) {
      size_t bytesWritten = uploadFile.write(upload.buf, upload.currentSize);
      if (bytesWritten != upload.currentSize) {
        Serial.println("File write error: " + String(bytesWritten) + " vs " + String(upload.currentSize));
        uploadFailed = true;   // END will discard the short file
      }
    }

  } else if (upload.status == UPLOAD_FILE_END) {
    if (uploadFile && uploadFilename.length() > 0) {
      uploadFile.flush();
      uploadFile.close();
      if (!uploadFailed && s_uploadTmp.length() > 0 && replaceFileOnSD(s_uploadTmp, s_uploadPath)) {
        Serial.println("File upload complete: " + uploadFilename + " size: " + String(upload.totalSize));

        if (uploadFilename.endsWith(".txt")) {
          loadAvailableScripts();
        } else if (uploadFilename.endsWith(".json")) {
          loadAvailableLanguages();
        }
      } else {
        Serial.println("File upload not stored: " + uploadFilename);
        if (s_uploadTmp.length() > 0) SD.remove(s_uploadTmp);
        uploadFailed = true;
      }
    }
    s_uploadPath = "";
    s_uploadTmp = "";

  } else if (upload.status == UPLOAD_FILE_ABORTED) {
    Serial.println("File upload aborted: " + uploadFilename);
    if (uploadFile) uploadFile.close();
    // Remove exactly the partial file this upload created, in its real folder.
    if (s_uploadTmp.length() > 0 && SD.exists(s_uploadTmp)) SD.remove(s_uploadTmp);
    s_uploadPath = "";
    s_uploadTmp = "";
    uploadFailed = true;
  }
}

void handleFileDownload() {
  if (server.hasArg("file")) {
    String filename = server.arg("file");
    // v4.11 SECURITY: canonical path check. Rejects ".." segments, backslashes
    // and control characters. An absolute path gets no free pass, and the
    // Wi-Fi credential files are never served: the saved-credentials file must
    // not leave the device (GET /api/download?file=/wifi_creds.txt).
    String filepath;
    int rc = checkRootPath(filename, filepath);
    if (rc == 400) {
      Serial.println("[DOWNLOAD] Rejected path: " + filename);
      server.send(400, "text/plain", "Bad request");
      return;
    }
    if (rc == 403) {
      Serial.println("[DOWNLOAD] Refused credential file: " + filename);
      server.send(403, "text/plain", "Forbidden");
      return;
    }
    if (filepath == "/") {   // a directory is not a file to download
      server.send(400, "text/plain", "Bad request");
      return;
    }

    if (SD.exists(filepath)) {
      File file = SD.open(filepath);
      if (file) {
        server.sendHeader("Content-Type", "application/octet-stream");
        server.sendHeader("Content-Disposition", "attachment; filename=\"" + headerSafeName(getFileNameFromPath(filepath)) + "\"");
        server.streamFile(file, "application/octet-stream");
        file.close();
        Serial.println("File downloaded: " + filepath);
      } else {
        server.send(500, "text/plain", "Failed to open file");
      }
    } else {
      server.send(404, "text/plain", "File not found: " + filename);
    }
  } else {
    server.send(400, "text/plain", "No file specified");
  }
}

void handleChangeDirectory() {
  if (server.hasArg("path")) {
    String path = server.arg("path");
    // ".." is the one special value (go up). Anything else must be a clean path.
    String checked;
    if (path != ".." && !canonicalSdPath(path, checked)) {
      server.send(400, "text/plain", "Invalid path");
      return;
    }
    changeDirectory(path);
    server.send(200, "application/json", "{\"success\":true,\"currentDirectory\":\"" + jsonEscapeStr(currentDirectory) + "\"}");
  } else {
    server.send(400, "text/plain", "No path specified");
  }
}

void handleGetCurrentDirectory() {
  server.send(200, "application/json", "{\"currentDirectory\":\"" + jsonEscapeStr(currentDirectory) + "\"}");
}

void handleDetectOS() {
  detectOS();
  server.send(200, "application/json", "{\"detectedOS\":\"" + detectedOS + "\"}");
}

void handleUseFile() {
  if (server.hasArg("file")) {
    String filePath = server.arg("file");
    // Resolve the way useFile() does, then check the result, so the path that
    // gets stored is the one that was checked.
    String canon;
    int rc = checkCwdPath(filePath, canon);
    if (rc != 0) {
      server.send(rc, "text/plain", rc == 403 ? "Forbidden" : "Bad request");
      return;
    }
    useFile(canon);
    server.send(200, "application/json", "{\"success\":true,\"message\":\"File ready to use: " + jsonEscapeStr(canon) + "\"}");
  } else {
    server.send(400, "text/plain", "No file specified");
  }
}

void handleCopyFile() {
  if (server.hasArg("source")) {
    String sourcePath = server.arg("source");
    String destPath = server.hasArg("destination") ? server.arg("destination") : "";
    String canon;
    int rc = checkCwdPath(sourcePath, canon);
    if (rc == 0 && destPath.length() > 0) rc = checkCwdPath(destPath, canon);
    if (rc != 0) {
      server.send(rc, "text/plain", rc == 403 ? "Forbidden" : "Bad request");
      return;
    }
    copyFile(sourcePath, destPath);
    server.send(200, "application/json", "{\"success\":true,\"message\":\"File copied: " + jsonEscapeStr(sourcePath) + "\"}");
  } else {
    server.send(400, "text/plain", "No source file specified");
  }
}

void handleCutFile() {
  if (server.hasArg("source")) {
    String sourcePath = server.arg("source");
    String destPath = server.hasArg("destination") ? server.arg("destination") : "";
    String canon;
    int rc = checkCwdPath(sourcePath, canon);
    // Moving the root or a web UI file would break the dashboard.
    if (rc == 0 && sourcePath.length() > 0 && isProtectedSdPath(canon)) rc = 403;
    if (rc == 0 && destPath.length() > 0) rc = checkCwdPath(destPath, canon);
    if (rc != 0) {
      server.send(rc, "text/plain", rc == 403 ? "Forbidden" : "Bad request");
      return;
    }
    cutFile(sourcePath, destPath);
    server.send(200, "application/json", "{\"success\":true,\"message\":\"File cut: " + jsonEscapeStr(sourcePath) + "\"}");
  } else {
    server.send(400, "text/plain", "No source file specified");
  }
}

void handlePasteFile() {
  String destPath = server.hasArg("destination") ? server.arg("destination") : "";
  String canon;
  int rc = (destPath.length() > 0) ? checkCwdPath(destPath, canon) : 0;
  if (rc != 0) {
    server.send(rc, "text/plain", rc == 403 ? "Forbidden" : "Bad request");
    return;
  }
  pasteFile(destPath);
  server.send(200, "application/json", "{\"success\":true,\"message\":\"File pasted\"}");
}

void handleJoinInternet() {
  if (server.hasArg("ssid") && server.hasArg("password")) {
    String ssid = server.arg("ssid");
    String password = server.arg("password");
    joinWiFi(ssid, password);

    // Return success since it's started in the background
    server.send(200, "application/json", "{\"success\":true,\"message\":\"Connecting to WiFi: " + jsonEscapeStr(ssid) + "...\"}");
  } else {
    server.send(400, "text/plain", "Missing SSID or password");
  }
}

void handleLeaveInternet() {
  leaveWiFi();
  server.send(200, "application/json", "{\"success\":true,\"message\":\"Disconnected from WiFi\"}");
}

void handleListFiles() {
  String path = currentDirectory;
  if (server.hasArg("path")) {
    path = server.arg("path");
  }

  // Canonical check: rejects ".." segments, backslashes and control characters,
  // and resolves relative names from the root.
  String canon;
  if (!canonicalSdPath(path.startsWith("/") ? path : ("/" + path), canon)) {
    server.send(400, "text/plain", "Invalid path");
    return;
  }
  path = canon;

  File root = SD.open(path);
  if (!root) {
    server.send(500, "text/plain", "Failed to open directory: " + path);
    return;
  }

  if (!root.isDirectory()) {
    server.send(400, "text/plain", "Not a directory: " + path);
    root.close();
    return;
  }

  String json = "[";
  bool first = true;

  if (path != "/") {
    json += "{";
    json += "\"name\":\"..\",";
    json += "\"size\":0,";
    json += "\"isDirectory\":true,";
    json += "\"path\":\"" + jsonEscapeStr(getParentDirectory(path)) + "\"";
    json += "}";
    first = false;
  }

  File file = root.openNextFile();
  while (file) {
    String name = String(file.name());
    if (name.lastIndexOf('/') >= 0) {
      name = name.substring(name.lastIndexOf('/') + 1);
    }

    // Hide system files: the web UI assets served from SD, transient reboot
    // helpers, dotfiles, the .espkg staging file, and the Wi-Fi credential
    // files. They belong to the device, not the user, and shouldn't appear in
    // the File Manager.
    bool isSystem =
        (path == "/") && (
          name == "index.html"        || name == "style.css"        ||
          name == "script.js"         || name == "update.espkg"     ||
          name == "reboot_script.txt" || name == "temp_resume.txt"  ||
          name.startsWith(".")        || isSecretSdPath("/" + name)
        );
    if (!isSystem) {
      if (!first) json += ",";
      first = false;
      json += "{";
      json += "\"name\":\"" + jsonEscapeStr(name) + "\",";
      json += "\"size\":" + String(file.size()) + ",";
      json += "\"isDirectory\":" + String(file.isDirectory() ? "true" : "false") + ",";
      json += "\"path\":\"" + jsonEscapeStr(path + (path.endsWith("/") ? "" : "/") + name) + "\"";
      json += "}";
    }
    file = root.openNextFile();
  }
  json += "]";

  root.close();
  server.send(200, "application/json", json);
}

void handleDeleteFile() {
  if (server.hasArg("file")) {
    String filename = server.arg("file");
    String filepath;

    // Canonical check (rejects "..", backslashes, control characters). Protected
    // paths are refused: the root (deleting "/" used to wipe the whole card),
    // the web UI files, and the Wi-Fi credential files.
    if (filename.length() == 0 ||
        !canonicalSdPath(filename.startsWith("/") ? filename : ("/" + filename), filepath)) {
      server.send(400, "text/plain", "Invalid filename");
      return;
    }
    if (isProtectedSdPath(filepath)) {
      Serial.println("[DELETE] Refused protected path: " + filepath);
      server.send(403, "text/plain", "Refusing to delete a protected file: " + filepath);
      return;
    }

    if (SD.exists(filepath)) {
      bool success = false;
      File file = SD.open(filepath);
      if (file) {
        if (file.isDirectory()) {
          file.close();
          success = deleteDirectory(filepath);
        } else {
          file.close();
          success = SD.remove(filepath);
        }
      }

      if (success) {
        server.send(200, "text/plain", "Deleted: " + filename);
        Serial.println("Deleted: " + filename);

        if (filename.endsWith(".txt")) {
          loadAvailableScripts();
        } else if (filename.endsWith(".json")) {
          loadAvailableLanguages();
        }
      } else {
        server.send(500, "text/plain", "Failed to delete: " + filename);
      }
    } else {
      server.send(404, "text/plain", "File not found: " + filename);
    }
  } else {
    server.send(400, "text/plain", "No file specified");
  }
}

void handleCreateDirectory() {
  if (server.hasArg("path")) {
    String path = server.arg("path");
    // v4.11 SECURITY: block traversal.
    if (path.indexOf("..") >= 0) {
      server.send(400, "text/plain", "Bad request");
      return;
    }

    if (!path.startsWith("/")) {
      path = currentDirectory + (currentDirectory.endsWith("/") ? "" : "/") + path;
    }

    if (SD.mkdir(path)) {
      server.send(200, "text/plain", "Directory created: " + path);
      Serial.println("Directory created: " + path);
    } else {
      server.send(500, "text/plain", "Failed to create directory");
    }
  } else {
    server.send(400, "text/plain", "No path specified");
  }
}

void handleFileInfo() {
  if (server.hasArg("file")) {
    String filename = server.arg("file");
    // v4.11 SECURITY: canonical path check, so clients can't probe for
    // /wifi_creds.txt etc. via file-info reconnaissance.
    String filepath;
    int rc = checkRootPath(filename, filepath);
    if (rc == 400) {
      server.send(400, "text/plain", "Bad request");
      return;
    }
    if (rc == 403) {
      server.send(403, "text/plain", "Forbidden");
      return;
    }

    if (SD.exists(filepath)) {
      File file = SD.open(filepath);
      if (file) {
        DynamicJsonDocument doc(512);
        doc["name"] = filename;
        doc["size"] = file.size();
        doc["isDirectory"] = file.isDirectory();
        file.close();

        String response;
        serializeJson(doc, response);
        server.send(200, "application/json", response);
      } else {
        server.send(500, "text/plain", "Failed to open file");
      }
    } else {
      server.send(404, "text/plain", "File not found");
    }
  } else {
    server.send(400, "text/plain", "No file specified");
  }
}

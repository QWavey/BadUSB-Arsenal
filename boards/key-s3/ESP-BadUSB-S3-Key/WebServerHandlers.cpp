#include "WebServerHandlers.h"
#include "FSManager.h"
#include "DuckyInterpreter.h"
#include "WiFiManager.h"
#include "LogManager.h"
#include "LEDManager.h"
#include <ArduinoJson.h>

void handleFileUpload() {
  HTTPUpload& upload = server.upload();

  if (upload.status == UPLOAD_FILE_START) {
    uploadFilename = upload.filename;
    Serial.println("File upload start: " + uploadFilename);

    // v4.11 SECURITY: reject path-traversal in filenames. Without this a
    // client could POST `filename=../reboot_script.txt` and land the file at
    // `/scripts/../reboot_script.txt` == `/reboot_script.txt`, which setup()
    // auto-executes on next boot -> persistent DuckyScript RCE for anyone
    // on the open AP. Enforce leaf-only names.
    if (uploadFilename.indexOf('/') >= 0 || uploadFilename.indexOf('\\') >= 0 ||
        uploadFilename.indexOf("..") >= 0 || uploadFilename.length() == 0) {
      Serial.println("[UPLOAD] Rejected unsafe filename: " + uploadFilename);
      uploadFilename = "";
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
    
    if (SD.exists(uploadPath)) {
      SD.remove(uploadPath);
    }
    
    uploadFile = SD.open(uploadPath, FILE_WRITE);
    if (!uploadFile) {
      Serial.println("Failed to create file: " + uploadPath);
      return;
    }
    
  } else if (upload.status == UPLOAD_FILE_WRITE) {
    if (uploadFile) {
      size_t bytesWritten = uploadFile.write(upload.buf, upload.currentSize);
      if (bytesWritten != upload.currentSize) {
        Serial.println("File write error: " + String(bytesWritten) + " vs " + String(upload.currentSize));
      }
    }
    
  } else if (upload.status == UPLOAD_FILE_END) {
    if (uploadFile) {
      uploadFile.flush();
      uploadFile.close();
      Serial.println("File upload complete: " + uploadFilename + " size: " + String(upload.totalSize));
      
      if (uploadFilename.endsWith(".txt")) {
        loadAvailableScripts();
      } else if (uploadFilename.endsWith(".json")) {
        loadAvailableLanguages();
      }
    }
  } else if (upload.status == UPLOAD_FILE_ABORTED) {
    Serial.println("File upload aborted: " + uploadFilename);
    if (uploadFile) {
      uploadFile.close();
      String uploadPath = String(DIR_UPLOADS) + "/" + uploadFilename;
      if (SD.exists(uploadPath)) {
        SD.remove(uploadPath);
      }
    }
  }
}

void handleFileDownload() {
  if (server.hasArg("file")) {
    String filename = server.arg("file");
    // v4.11 SECURITY: block `..` traversal so a client can't exfiltrate
    // arbitrary SD content (e.g. /wifi_creds.txt, /update.espkg) with
    // GET /api/download?file=../wifi_creds.txt on the open AP.
    if (filename.indexOf("..") >= 0) {
      Serial.println("[DOWNLOAD] Rejected traversal: " + filename);
      server.send(400, "text/plain", "Bad request");
      return;
    }
    String filepath = filename;

    if (!filepath.startsWith("/")) {
      filepath = "/" + filepath;
    }

    if (SD.exists(filepath)) {
      File file = SD.open(filepath);
      if (file) {
        server.sendHeader("Content-Type", "application/octet-stream");
        server.sendHeader("Content-Disposition", "attachment; filename=" + filename);
        server.streamFile(file, "application/octet-stream");
        file.close();
        Serial.println("File downloaded: " + filename);
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
    changeDirectory(path);
    server.send(200, "application/json", "{\"success\":true,\"currentDirectory\":\"" + currentDirectory + "\"}");
  } else {
    server.send(400, "text/plain", "No path specified");
  }
}

void handleGetCurrentDirectory() {
  server.send(200, "application/json", "{\"currentDirectory\":\"" + currentDirectory + "\"}");
}

void handleDetectOS() {
  detectOS();
  server.send(200, "application/json", "{\"detectedOS\":\"" + detectedOS + "\"}");
}

void handleUseFile() {
  if (server.hasArg("file")) {
    String filePath = server.arg("file");
    useFile(filePath);
    server.send(200, "application/json", "{\"success\":true,\"message\":\"File ready to use: " + filePath + "\"}");
  } else {
    server.send(400, "text/plain", "No file specified");
  }
}

// v4.16 SECURITY: `..` traversal filter helper shared by copy/cut/paste.
// Same policy as handleDeleteFile: refuse any path containing `..`.
static bool __rejectTraversal(const String& p) {
  return p.indexOf("..") >= 0;
}

void handleCopyFile() {
  if (server.hasArg("source")) {
    String sourcePath = server.arg("source");
    String destPath = server.hasArg("destination") ? server.arg("destination") : "";
    if (__rejectTraversal(sourcePath) || __rejectTraversal(destPath)) {
      server.send(400, "text/plain", "Bad request"); return;
    }
    copyFile(sourcePath, destPath);
    server.send(200, "application/json", "{\"success\":true,\"message\":\"File copied: " + sourcePath + "\"}");
  } else {
    server.send(400, "text/plain", "No source file specified");
  }
}

void handleCutFile() {
  if (server.hasArg("source")) {
    String sourcePath = server.arg("source");
    String destPath = server.hasArg("destination") ? server.arg("destination") : "";
    if (__rejectTraversal(sourcePath) || __rejectTraversal(destPath)) {
      server.send(400, "text/plain", "Bad request"); return;
    }
    cutFile(sourcePath, destPath);
    server.send(200, "application/json", "{\"success\":true,\"message\":\"File cut: " + sourcePath + "\"}");
  } else {
    server.send(400, "text/plain", "No source file specified");
  }
}

void handlePasteFile() {
  String destPath = server.hasArg("destination") ? server.arg("destination") : "";
  if (__rejectTraversal(destPath)) {
    server.send(400, "text/plain", "Bad request"); return;
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
    server.send(200, "application/json", "{\"success\":true,\"message\":\"Connecting to WiFi: " + ssid + "...\"}");
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
    if (path.indexOf("..") >= 0) {
      server.send(400, "text/plain", "Invalid path");
      return;
    }
  }
  
  if (!path.startsWith("/")) path = "/" + path;

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
    json += "\"path\":\"" + getParentDirectory(path) + "\"";
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
    // helpers, dotfiles, and the .espkg staging file. They belong to the
    // device, not the user, and shouldn't appear in the File Manager.
    bool isSystem =
        (path == "/") && (
          name == "index.html"        || name == "style.css"        ||
          name == "script.js"         || name == "update.espkg"     ||
          name == "reboot_script.txt" || name == "temp_resume.txt"  ||
          name.startsWith(".")
        );
    if (!isSystem) {
      if (!first) json += ",";
      first = false;
      json += "{";
      json += "\"name\":\"" + name + "\",";
      json += "\"size\":" + String(file.size()) + ",";
      json += "\"isDirectory\":" + String(file.isDirectory() ? "true" : "false") + ",";
      json += "\"path\":\"" + path + (path.endsWith("/") ? "" : "/") + name + "\"";
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
    
    if (filename.indexOf("..") >= 0 || filename.length() == 0) {
      server.send(400, "text/plain", "Invalid filename");
      return;
    }

    String filepath = filename;
    if (!filepath.startsWith("/")) {
      filepath = "/" + filepath;
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
    // v4.11 SECURITY: block traversal so clients can't probe for
    // /wifi_creds.txt etc. via file-info reconnaissance.
    if (filename.indexOf("..") >= 0) {
      server.send(400, "text/plain", "Bad request");
      return;
    }
    String filepath = filename;

    if (!filepath.startsWith("/")) {
      filepath = "/" + filepath;
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

#include "LogManager.h"

void openLogFile() {
  if (!sdCardPresent || !loggingEnabled) return;

  if (logFileOpen) {
    logFile.close();
  }

  logFile = SD.open(FILE_LOG, FILE_APPEND);
  if (logFile) {
    logFileOpen = true;
    logFile.println(LOG_SESSION_START_MARKER);
    logFile.println("Timestamp: " + String(millis()));
    logFile.println("Device: ESP32-S3 BadUSB");
    logFile.flush();
  }
}

void closeLogFile() {
  if (logFileOpen) {
    logFile.println(LOG_SESSION_END_MARKER);
    logFile.close();
    logFileOpen = false;
  }
}

void logCommand(String type, String command) {
  if (!sdCardPresent || !loggingEnabled || !logFileOpen) return;

  String timestamp = String(millis());
  logFile.println("[" + timestamp + "] [" + type + "] " + command);
  logFile.flush();
}

// Always-on verbose debug log — written to /logs/debug.txt regardless of loggingEnabled
// Rotates at 64KB to avoid filling SD card
void logDebug(String message) {
  if (!sdCardPresent) return;
  // Watch port bug hunt: honour loggingEnabled (matches logCommand). Was
  // firing on every HTTP hit, /api/stats poll, WiFi event — each write did
  // open+read-size+close+open+append+close, contending with MSC RAW SD
  // access and burning SD wear. Silent by default; user can enable in
  // Settings → Logging.
  if (!loggingEnabled) return;

  // Rotate if too large — but only check the size every ~1 min, not on
  // every log line. Saves one stat call per line.
  static unsigned long lastRotateCheck = 0;
  if (millis() - lastRotateCheck > 60000) {
    lastRotateCheck = millis();
    File check = SD.open(FILE_DEBUG);
    if (check && check.size() > 65536) {
      check.close();
      SD.remove(FILE_DEBUG);
      File init = SD.open(FILE_DEBUG, FILE_WRITE);
      if (init) { init.println("[DEBUG LOG ROTATED]"); init.close(); }
    } else if (check) {
      check.close();
    }
  }

  File f = SD.open(FILE_DEBUG, FILE_APPEND);
  if (f) {
    f.println("[" + String(millis()) + "] " + message);
    f.flush();
    f.close();
  }
}

void loadCommandHistory() {
  if (!sdCardPresent) return;

  if (SD.exists(FILE_HISTORY)) {
    File file = SD.open(FILE_HISTORY);
    if (file) {
      commandHistory.clear();
      while (file.available()) {
        String line = file.readStringUntil('\n');
        line.trim();
        if (line.length() > 0) {
          commandHistory.push_back(line);
        }
      }
      file.close();

      if (commandHistory.size() > MAX_HISTORY_SIZE) {
        commandHistory.erase(commandHistory.begin(), commandHistory.begin() + (commandHistory.size() - MAX_HISTORY_SIZE));
      }
    }
  }
}

void saveCommandHistory() {
  if (!sdCardPresent) return;

  if (SD.exists(FILE_HISTORY)) {
    SD.remove(FILE_HISTORY);
  }

  File file = SD.open(FILE_HISTORY, FILE_WRITE);
  if (file) {
    for (String cmd : commandHistory) {
      file.println(cmd);
    }
    file.close();
  }
}

void addToHistory(String command) {
  commandHistory.push_back(command);

  bool trimmed = false;
  if (commandHistory.size() > MAX_HISTORY_SIZE) {
    commandHistory.erase(commandHistory.begin());
    trimmed = true;
  }

  // Bug-hunt round 5 fix #10: quadratic SD wear + latency. saveCommand
  // History() was doing a full delete-and-rewrite of the entire history
  // file on EVERY command — a 500-line DuckyScript triggered 500 O(N)
  // rewrites. Now we APPEND only when the buffer wasn't trimmed
  // (append is O(1) on FAT), and fall back to the full rewrite only when
  // the ring buffer just dropped its oldest entry (then the on-disk copy
  // has to match).
  // Commands can contain secrets, so they are written to the card only while
  // logging is on, the same switch the other log files honour. The in-memory
  // history still works either way.
  if (!sdCardPresent || !loggingEnabled) return;
  if (trimmed) {
    saveCommandHistory();          // full rewrite because head dropped
  } else {
    File file = SD.open(FILE_HISTORY, FILE_APPEND);
    if (file) { file.println(command); file.close(); }
  }
}

void clearErrors() {
  errorCount = 0;
  lastError = "No Errors";
  totalCommandsExecuted = 0; // Optional: Reset command counter too if desired, user said "Clear Error Log"
  Serial.println("[Log] Errors cleared");
  logDebug("Errors cleared by user");
}

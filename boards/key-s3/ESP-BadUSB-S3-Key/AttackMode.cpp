#include "AttackMode.h"
#include "GlobalState.h"
#include "USBManager.h"          // hidDetach() for the pre-reboot USB drop
#include "MSCManager.h"          // v4.8: sub-region translation for SIZE_XX
#include <SD.h>
#include "esp32-hal-tinyusb.h"   // tud_disconnect()

AttackModeConfig currentAttackMode;

// -------------------- helpers --------------------

static uint16_t parseHex16(const String& s) {
  // accepts "046D", "0x046D", "046d"
  const char* c = s.c_str();
  if (s.startsWith("0x") || s.startsWith("0X")) c += 2;
  return (uint16_t) strtol(c, NULL, 16);
}

// SIZE_<n>_<unit>  -> bytes; returns 0 on parse failure
static uint64_t parseSizeToken(const String& tok) {
  if (!tok.startsWith("SIZE_")) return 0;
  // Must have exactly one '_' between number and unit (SIZE_22_GB) — reject
  // decimals / spurious extras like SIZE_2_5_GB.
  int firstU = tok.indexOf('_', 5);
  int lastU  = tok.lastIndexOf('_');
  if (firstU != lastU || firstU <= 5) return 0;
  int u = firstU;
  String num  = tok.substring(5, u);
  String unit = tok.substring(u + 1);
  // Reject anything that isn't purely digits — no signs, no letters, no spaces.
  if (num.length() == 0) return 0;
  for (unsigned i = 0; i < num.length(); i++) {
    if (num[i] < '0' || num[i] > '9') return 0;
  }
  unit.toUpperCase();
  uint64_t n = (uint64_t) strtoull(num.c_str(), NULL, 10);
  if (n == 0) return 0;
  // multiplier overflow guard: reject anything that wraps uint64_t
  const uint64_t MAX = 0xFFFFFFFFFFFFFFFFULL;
  uint64_t mult = 1;
  if      (unit == "KB") mult = 1024ULL;
  else if (unit == "MB") mult = 1024ULL * 1024ULL;
  else if (unit == "GB") mult = 1024ULL * 1024ULL * 1024ULL;
  else if (unit == "TB") mult = 1024ULL * 1024ULL * 1024ULL * 1024ULL;
  else return 0;
  if (n > MAX / mult) return 0;   // would overflow
  return n * mult;
}

static bool tokIs(const String& t, const char* a, const char* b = NULL, const char* c = NULL) {
  return t == a || (b && t == b) || (c && t == c);
}

// -------------------- prefs --------------------

void loadAttackModePrefs() {
  currentAttackMode.hid       = preferences.getBool  ("am_hid",  true);
  currentAttackMode.storage   = preferences.getBool  ("am_msc",  false);
  // Auto-recover: if a previous session left HID off without an explicit
  // "no_hid" intent flag, force it back on. Otherwise the user is silently
  // locked out (typing scripts no-op, no way to notice from the Web UI).
  if (!currentAttackMode.hid && !preferences.getBool("am_no_hid_intent", false)) {
    Serial.println("[ATTACKMODE] Recovering am_hid=false -> true (no explicit NO_HID intent)");
    currentAttackMode.hid = true;
    preferences.putBool("am_hid", true);
  }
  currentAttackMode.vid       = preferences.getUShort("am_vid",  0x303A);
  currentAttackMode.pid       = preferences.getUShort("am_pid",  0x0002);
  currentAttackMode.sizeBytes = preferences.getULong64("am_size", 0);
}

static void saveAttackModePrefs() {
  preferences.putBool   ("am_hid",  currentAttackMode.hid);
  preferences.putBool   ("am_msc",  currentAttackMode.storage);
  // Remember whether HID-off was intentional so loadAttackModePrefs doesn't
  // "auto-recover" a deliberate storage-only setup.
  preferences.putBool   ("am_no_hid_intent", !currentAttackMode.hid);
  preferences.putUShort ("am_vid",  currentAttackMode.vid);
  preferences.putUShort ("am_pid",  currentAttackMode.pid);
  preferences.putULong64("am_size", currentAttackMode.sizeBytes);
}

uint64_t effectiveStorageBytes() {
  uint64_t actual = sdCardPresent ? SD.totalBytes() : 0;
  if (currentAttackMode.sizeBytes == 0) return actual;
  if (actual == 0) return currentAttackMode.sizeBytes;
  return (currentAttackMode.sizeBytes < actual) ? currentAttackMode.sizeBytes
                                                : actual;
}

// -------------------- ATTACKMODE --------------------

bool handleAttackModeLine(const String& line) {
  if (!line.startsWith("ATTACKMODE")) return false;

  // v4.36 bug-hunt HIGH #9: seed cfg from currentAttackMode so tokens ONLY
  // override the fields they actually mention. Prior code reset
  // hid=true, storage=false on every line; `ATTACKMODE VID_XX PID_XX` on a
  // device with storage=true would silently clear storage and trigger a
  // stray composition-change reboot. Now: only tokens actually present
  // change cfg.
  AttackModeConfig cfg = currentAttackMode;
  bool sawStorageToken = false, sawHidToken = false, sawOffToken = false;

  bool sizeGiven = false;
  uint64_t sizeReq = 0;

  // Tokenize on whitespace
  int start = String("ATTACKMODE").length();
  int len = line.length();
  while (start < len) {
    while (start < len && (line[start] == ' ' || line[start] == '\t')) start++;
    if (start >= len) break;
    int end = start;
    while (end < len && line[end] != ' ' && line[end] != '\t') end++;
    String tok = line.substring(start, end);
    start = end;
    tok.toUpperCase();

    if (tok.length() == 0) continue;
    if      (tokIs(tok, "HID"))                       { cfg.hid = true; sawHidToken = true; }
    else if (tokIs(tok, "STORAGE", "MSC", "STORE"))   { cfg.storage = true; sawStorageToken = true; }
    else if (tokIs(tok, "STORAGE_ONLY", "MSC_ONLY"))  { cfg.storage = true; cfg.hid = false; sawStorageToken = true; sawHidToken = true; }
    else if (tokIs(tok, "NO_HID", "NOHID", "HID_OFF")) { cfg.hid = false; sawHidToken = true; }
    else if (tokIs(tok, "OFF"))                       { cfg.storage = false; sawStorageToken = true; sawOffToken = true; }
    // BLANK: tear the device completely off the bus. HID off, MSC off — after
    // the reboot the composite descriptor is empty and USB.begin() is skipped
    // entirely, so Windows / Linux see nothing at all (no HID keyboard, no
    // removable drive). Recovery: run "ATTACKMODE HID" via the web UI's
    // /execute endpoint or the "HID only" button in Settings.
    else if (tokIs(tok, "BLANK", "NONE", "UNMOUNT"))  { cfg.hid = false; cfg.storage = false; sawHidToken = true; sawStorageToken = true; }
    else if (tok.startsWith("VID_")) {
      String v = tok.substring(4);
      if (v.length() == 0) { Serial.println("[ATTACKMODE] VID_ needs hex digits, ignored"); }
      else                 { cfg.vid = parseHex16(v); }
    }
    else if (tok.startsWith("PID_")) {
      String v = tok.substring(4);
      if (v.length() == 0) { Serial.println("[ATTACKMODE] PID_ needs hex digits, ignored"); }
      else                 { cfg.pid = parseHex16(v); }
    }
    else if (tok.startsWith("SIZE_"))                 { sizeReq = parseSizeToken(tok); sizeGiven = true; }
    else {
      Serial.println("[ATTACKMODE] Unknown token: " + tok);
    }
  }

  // Validate SIZE against real SD capacity
  if (sizeGiven) {
    uint64_t actual = sdCardPresent ? SD.totalBytes() : 0;
    if (sizeReq == 0) {
      lastError = "SIZE parse failed in ATTACKMODE";
      errorCount++;
      Serial.println("[ATTACKMODE] " + lastError);
    } else if (actual > 0 && sizeReq > actual) {
      lastError = "SIZE exceeds SD capacity (" + String((unsigned long)(actual / (1024ULL*1024ULL))) + " MB available)";
      errorCount++;
      Serial.println("[ATTACKMODE] " + lastError);
      cfg.sizeBytes = actual;   // clamp
    } else {
      cfg.sizeBytes = sizeReq;
    }
  }

  // Safety net: an empty ATTACKMODE (no HID/STORAGE/OFF/BLANK token) would
  // leave the device with no USB interfaces at all — user probably fat-fingered
  // the line. Keep HID on as a minimum. BLANK is the OPT-IN way to remove
  // everything on purpose (recovery: `ATTACKMODE HID` via the web UI).
  bool anyKeyword = cfg.hid || cfg.storage ||
                    line.indexOf("OFF") >= 0 ||
                    line.indexOf("BLANK") >= 0 ||
                    line.indexOf("NONE")  >= 0 ||
                    line.indexOf("UNMOUNT") >= 0;
  if (!anyKeyword) {
    Serial.println("[ATTACKMODE] no HID/STORAGE/OFF/BLANK token; keeping HID on to avoid bricking");
    cfg.hid = true;
  }

  // Detect composition change vs current state (requires reboot to re-enumerate)
  bool composeChanged = (cfg.hid     != currentAttackMode.hid) ||
                        (cfg.storage != currentAttackMode.storage);
  // v4.33: VID/PID or manufacturer/product change WITHOUT composition change
  // also requires a re-enumerate cycle - the host caches the old descriptor
  // until it sees an unplug. Symptom the user hit: `ATTACKMODE HID VID_05AC
  // PID_021E` inside OS_DETECT changed the identity to Apple, USB attached
  // then detached, then STRING typed nothing because the host hadn't bound
  // the new keyboard driver yet. Do a short unmount/remount + settle
  // instead of a full reboot (much faster).
  bool identityChanged = (cfg.vid != currentAttackMode.vid) ||
                         (cfg.pid != currentAttackMode.pid);

  currentAttackMode = cfg;
  saveAttackModePrefs();

  Serial.printf("[ATTACKMODE] hid=%d storage=%d vid=%04x pid=%04x size=%llu bytes (compose_changed=%d, ident_changed=%d)\n",
                cfg.hid, cfg.storage, cfg.vid, cfg.pid, cfg.sizeBytes, composeChanged, identityChanged);

  // v4.35: BOTH composition change AND identity (VID/PID) change require a
  // REAL REBOOT to take effect - the tinyusb descriptor is built once at
  // USB.begin() from the NVS-persisted VID/PID and can't be swapped live.
  // Attempting tud_disconnect()+tud_connect() only re-cycles the SAME
  // descriptor, so the host sees an unplug/replug of the OLD identity and
  // the intended new identity never appears. Symptom: user's OS_DETECT
  // script does "connect+disconnect sound" then nothing types, because
  // the payload continued running against the WRONG (old) HID identity
  // and the DETECT_OS logic assumed the new Apple keyboard was live.
  //
  // Now: any ATTACKMODE change that alters composition OR VID/PID sets
  // g_composeRebootPending, so the executor persists lines[i+1..] to
  // /temp_resume.txt and reboots. setup() picks up the resume file after
  // the ESP re-enumerates with the NEW identity from NVS.
  if (composeChanged || identityChanged) {
    extern volatile bool g_composeRebootPending;
    Serial.println(composeChanged
        ? "[ATTACKMODE] Composition changed - deferring reboot until executor persists remainder"
        : "[ATTACKMODE] VID/PID identity changed - deferring reboot to rebuild descriptor with new identity");
    g_composeRebootPending = true;
  }
  return true;
}

bool handleSizeLine(const String& line) {
  if (!line.startsWith("SIZE_")) return false;
  uint64_t bytes = parseSizeToken(line);
  if (bytes == 0) {
    lastError = "SIZE parse failed: " + line;
    errorCount++;
    Serial.println("[SIZE] " + lastError);
    return true;
  }
  uint64_t actual = sdCardPresent ? SD.totalBytes() : 0;
  if (actual > 0 && bytes > actual) {
    lastError = "SIZE exceeds SD capacity (" + String((unsigned long)(actual / (1024ULL*1024ULL))) + " MB available)";
    errorCount++;
    Serial.println("[SIZE] " + lastError);
    currentAttackMode.sizeBytes = actual;
  } else {
    currentAttackMode.sizeBytes = bytes;
  }
  preferences.putULong64("am_size", currentAttackMode.sizeBytes);
  Serial.printf("[SIZE] set to %llu bytes\n", currentAttackMode.sizeBytes);

  // v4.8: SIZE now also creates a real MSC sub-region in the SD's free space
  // (same mechanism as BEHAVE_BROKEN) so files in the primary partition stay
  // hidden. If no free space is available, the SIZE cap still applies to the
  // reported capacity but the primary FAT's files remain visible - user must
  // shrink the primary partition manually to get real hiding.
  if (sdCardPresent) {
    uint32_t freeStart = 0, freeSize = 0;
    if (mscFindFreeSpaceAfterLastPartition(freeStart, freeSize)) {
      uint32_t reqSectors = (uint32_t)(currentAttackMode.sizeBytes / 512ULL);
      if (reqSectors == 0) reqSectors = freeSize;
      if (reqSectors > freeSize) reqSectors = freeSize;
      // Zero the first ~4 KB of the sub-region so Windows sees unformatted
      // and offers to format.
      uint8_t zeros[512]; memset(zeros, 0, sizeof(zeros));
      for (uint32_t i = 0; i < 8 && i < reqSectors; i++) {
        SD.writeRAW(zeros, freeStart + i);
      }
      mscSetSubRegion(freeStart, reqSectors);
      Serial.printf("[SIZE] sub-region: LBA %u..%u (%.1f MB) - files in "
                    "primary partition are hidden from host.\n",
                    (unsigned)freeStart, (unsigned)(freeStart + reqSectors - 1),
                    (double)reqSectors * 512.0 / (1024.0 * 1024.0));
    } else {
      Serial.println("[SIZE] no free space after primary partition; capacity "
                     "cap applies but primary FAT files stay visible");
    }
  }
  return true;
}

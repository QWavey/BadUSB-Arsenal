#include "DuckyInterpreter.h"
#include "LEDManager.h"
#include "ComShell.h"        // v4.17: pump COM shell during DELAY blocking
#include "LogManager.h"
#include "USBManager.h"
#include "FSManager.h"
#include "WiFiManager.h"
#include "BTManager.h"
#include "AttackMode.h"
#include "MSCManager.h"      // v4.9: mscFindFreeSpaceAfterLastPartition + sub-region API
#include <USB.h>
#include "esp32-hal-tinyusb.h"   // v4.29: tud_mounted() for the real
                                  //        WAIT_FOR_EVENT USB_CONNECTED wait

struct LoopState {
  int startLine;
  int currentIteration;
  int totalIterations;
  String varName;
  int step;
};

bool evalCondition(String condition) {
  condition.trim();
  condition = processVariables(condition);
  // v4.36 bug-hunt HIGH #6: strip a matched wrapping `(...)` so Hak5's
  // `IF ($_OS == LINUX)` doesn't split into `($_OS` / `LINUX)`.
  // Loop in case the user wrote `((...))`.
  while (condition.length() >= 2 && condition.charAt(0) == '(' &&
         condition.charAt(condition.length() - 1) == ')') {
    // Only strip when the parens actually pair around the whole thing
    // (not `(a) && (b)` where the outer `(` matches an inner `)`).
    int depth = 0; bool balanced = true;
    for (uint32_t k = 0; k < condition.length(); k++) {
      char c = condition.charAt(k);
      if (c == '(') depth++;
      else if (c == ')') { depth--; if (depth == 0 && k != condition.length() - 1) { balanced = false; break; } }
    }
    if (!balanced || depth != 0) break;
    condition = condition.substring(1, condition.length() - 1);
    condition.trim();
  }

  // v4.36 bug-hunt HIGH #7: case-insensitive TRUE/FALSE. Hak5 built-ins
  // set the mirror vars to uppercase "TRUE"/"FALSE", but the old check
  // only matched lowercase - `IF $_RECEIVED_HOST_LOCK_LED_REPLY` fell
  // through to `condition.length() > 0` at the tail and was ALWAYS
  // true, forcing the wrong branch in DETECT_OS.
  String lower = condition; lower.toLowerCase();
  if (lower == "true"  || condition == "1") return true;
  if (lower == "false" || condition == "0") return false;

  // New Connection Condition Support
  if (condition == "IF_CLIENT_CONNECTED_BLUETOOTH") return (getBTClientCount() > 0);
  if (condition == "IF_CLIENT_CONNECTED_WIFI") return (WiFi.softAPgetStationNum() > 0);
  if (condition == "IF_CLIENT_DISCONNECTED_WIFI") return (WiFi.softAPgetStationNum() == 0);
  if (condition == "IF_CLIENT_DISCONNECTED_BLUETOOTH") return (getBTClientCount() == 0);
  if (condition == "IF_CLIENT_CONNECTED") return (WiFi.softAPgetStationNum() > 0 || getBTClientCount() > 0);
  if (condition == "IF_CLIENT_DISCONNECTED") return (WiFi.softAPgetStationNum() == 0 && getBTClientCount() == 0);
  if (condition == "IF_CLIENT_CONNECTED_DISCONNECTED") return true; // Catch-all trigger
  if (condition == "IF_CLIENT_CONNECTED_DISCONNECTED_BLUETOOTH") return true;
  if (condition == "IF_CLIENT_CONNECTED_DISCONNECTED_WIFI") return true;

  // v4.36 bug-hunt HIGH #6: for `==` / `!=`, only trust the numeric
  // parse when BOTH sides actually look like numbers - otherwise use
  // string equality. Old code accepted `lVal == rVal` where both were
  // 0.0 (because non-numeric strings toFloat() to 0), so `$_OS == LINUX`
  // was true whenever $_OS was any non-numeric value.
  auto isNum = [](const String& s) -> bool {
    if (s.length() == 0) return false;
    bool dot = false, digit = false;
    uint32_t i = 0;
    if (s.charAt(0) == '-' || s.charAt(0) == '+') i = 1;
    for (; i < s.length(); i++) {
      char c = s.charAt(i);
      if (c >= '0' && c <= '9') digit = true;
      else if (c == '.' && !dot) dot = true;
      else return false;
    }
    return digit;
  };

  String ops[] = {"==", "!=", ">=", "<=", ">", "<"};
  for (String op : ops) {
    int opIdx = condition.indexOf(op);
    if (opIdx != -1) {
      String left = condition.substring(0, opIdx);
      String right = condition.substring(opIdx + op.length());
      left.trim();
      right.trim();

      if (left.length() > 0 && right.length() > 0) {
        bool bothNum = isNum(left) && isNum(right);
        float lVal = bothNum ? left.toFloat()  : 0.0f;
        float rVal = bothNum ? right.toFloat() : 0.0f;
        // Case-insensitive string equality for the string-equality path.
        String lU = left;  lU.toUpperCase();
        String rU = right; rU.toUpperCase();

        if (op == "==") return bothNum ? (lVal == rVal) : (lU == rU);
        if (op == "!=") return bothNum ? (lVal != rVal) : (lU != rU);
        if (op == ">=") return bothNum && lVal >= rVal;
        if (op == "<=") return bothNum && lVal <= rVal;
        if (op == ">")  return bothNum && lVal >  rVal;
        if (op == "<")  return bothNum && lVal <  rVal;
      }
      break;
    }
  }

  // Bare condition: truthy if non-empty AND not literal "false" / "0".
  // (Was `condition.length() > 0` which returned true for "FALSE".)
  if (lower == "false" || condition == "0") return false;
  return condition.length() > 0;
}

// v4.25 bug-hunt CRITICAL #2 + HIGH #3/#4/#10: nested-script depth counter.
// IMPORT and RUN_EXTENSION reenter executeScript; without a depth guard a
// self-import loops until stack overflow and the tail cleanup (scriptRunning
// clear, completion-blink, hidReleaseIfSilent, closeLogFile) fires from the
// INNER call and corrupts outer state. Cap at 4 nesting levels and only run
// the tail cleanup at depth 0.
static int g_scriptDepth = 0;
static const int SCRIPT_MAX_DEPTH = 4;

void executeScript(const String& script) {
  // NOTE: no HID-off guard here on purpose. If we refused to run when
  // currentAttackMode.hid is false, a user who accidentally set
  // "ATTACKMODE STORAGE" would be locked out — they'd have no way to run
  // "ATTACKMODE HID" to recover. Non-keystroke commands (ATTACKMODE, WIFI_*,
  // DELAY, etc.) must always run; individual keystroke ops silently no-op if
  // HID isn't registered (see fastPressKey/fastTypeString).

  // Depth-aware "already running" guard: nested IMPORT / RUN_EXTENSION calls
  // MUST be allowed (they legitimately reenter). Only reject when there's a
  // TOP-LEVEL script already going.
  if (scriptRunning && g_scriptDepth == 0) {
    Serial.println("Script already running");
    return;
  }
  if (g_scriptDepth >= SCRIPT_MAX_DEPTH) {
    lastError = "IMPORT/RUN_EXTENSION nesting depth exceeded (" + String(SCRIPT_MAX_DEPTH) + ")";
    errorCount++;
    Serial.println("[SCRIPT] " + lastError);
    return;
  }
  g_scriptDepth++;
  const bool topLevel = (g_scriptDepth == 1);
  // v4.35: capture original script text at the top level so the ATTACKMODE
  // reboot-persist path can save the WHOLE script (not internal
  // preprocessed slices) to /temp_resume.txt.
  extern String g_currentTopScript;
  if (topLevel) g_currentTopScript = script;

  scriptRunning = true;
  stopRequested = false;
  scriptStartTime = millis();
  // v4.17: user-toggleable "Blink LED while executing payloads" (Settings).
  // Default ON so behaviour is backwards-compatible. When OFF, hold the LED
  // solid blue for the run instead of the busy blink.
  if (preferences.getBool("blink_on_run", true)) {
    setLEDMode(1);
  } else {
    setLED(0, 0, 255);
  }

  ensureHidReady();   // attach the keyboard now if silent-startup detached it

  if (loggingEnabled && topLevel) {
    openLogFile();
    logCommand("SCRIPT_START", "Script execution started");
  }

  if (topLevel) {
    addToHistory("Script executed at " + String(millis()));
    totalScriptsExecuted++;
  }

  // v4.23 (bug-hunt HIGH #2): reset BUTTON_DEF state at every TOP-LEVEL
  // script start so an interrupted BUTTON_DEF (via /api/stop or reset button)
  // doesn't leak "swallow every line" into the next script. Nested calls
  // preserve the flag - see call-site save/restore in IMPORT/RUN_EXTENSION.
  // v4.26 bug-hunt HIGH #3: also drop the bound handler body itself, so
  // running any new script wipes the previous script's BUTTON_DEF binding.
  // (Persistence across runs was never intended - the spec ties a handler
  // to the script that defined it.)
  extern bool g_buttonDefActive;
  extern String g_buttonHandlerScript;
  if (topLevel) {
    g_buttonDefActive     = false;
    g_buttonHandlerScript = "";
  }

  // v4.17: seed the Hak5 extension special variables into the interpreter's
  // variable map on every script start. Read via $_NAME (existing $-strip
  // in processVariables). Writable by DETECT_OS-style assignments; kept in
  // sync with our best-effort software mirror for the lock keys.
  if (variables.find("_OS") == variables.end())                  variables["_OS"] = "";
  if (variables.find("_CAPSLOCK_ON") == variables.end())         variables["_CAPSLOCK_ON"] = "FALSE";
  if (variables.find("_NUMLOCK_ON") == variables.end())          variables["_NUMLOCK_ON"] = "FALSE";
  if (variables.find("_SCROLLLOCK_ON") == variables.end())       variables["_SCROLLLOCK_ON"] = "FALSE";
  if (variables.find("_HOST_CONFIGURATION_REQUEST_COUNT") == variables.end()) variables["_HOST_CONFIGURATION_REQUEST_COUNT"] = "0";
  if (variables.find("_RECEIVED_HOST_LOCK_LED_REPLY") == variables.end())     variables["_RECEIVED_HOST_LOCK_LED_REPLY"] = "FALSE";

  std::vector<String> lines;
  // v4.23 (bug-hunt HIGH #5): a parallel "raw" buffer that keeps every line
  // UNTRIMMED and INCLUDING blank lines, so STRING_BASH/STRING_POWERSHELL
  // multi-line collectors can preserve indentation and empty separators
  // (needed for shell heredocs, PowerShell here-strings, Python payloads).
  // The `lines` vector still drops blank/comment lines so the executor
  // loop's arithmetic is unchanged for everything OUTSIDE those blocks.
  std::vector<String> rawLines;
  int startIndex = 0;
  int endIndex = script.indexOf('\n');
  auto pushBoth = [&](const String& raw) {
    // Trim CR from Windows line endings but keep the rest intact.
    String r = raw;
    if (r.length() && r.charAt(r.length()-1) == '\r') r.remove(r.length()-1);
    rawLines.push_back(r);
    String trimmed = r; trimmed.trim();
    if (trimmed.length() > 0) lines.push_back(trimmed);
  };
  while (endIndex != -1) {
    pushBoth(script.substring(startIndex, endIndex));
    startIndex = endIndex + 1;
    endIndex = script.indexOf('\n', startIndex);
  }
  if (startIndex < (int)script.length()) {
    pushBoth(script.substring(startIndex));
  }

  // v4.32 Pass 0: EXPAND `EXTENSION NAME` empty-body blocks and
  // `EXTENSION NAME ˅` collapsed refs by loading the ext body from the SD
  // and inlining it. This matches Hak5 semantics - writing `EXTENSION NAME`
  // in a payload is how you PULL IN an extension's FUNCTION defs so you can
  // call them later. Without this, the block was a no-op and `DETECT_OS`
  // silently failed as an unknown command.
  //
  // Also handled: `EXTENSION NAME ^ ... END_EXTENSION` inline-expanded form
  // is left alone (real body already inlined, no SD lookup needed).
  //
  // Guard against infinite recursion by tracking already-inlined names.
  // v4.34: run Pass 0 as a fixed-point loop. An inlined ext body may itself
  // contain another `EXTENSION xxx` empty-body form (a common Hak5 pattern
  // where OS_DETECT is chained from HELLO_OS etc.). Loop until nothing new
  // gets expanded, bounded at 6 passes to bound runaway recursion.
  {
    // Recursion guard is per-executeScript-invocation, not `static` - a
    // leftover static entry from a previous run could silently disable a
    // legitimate inline in the next script.
    std::vector<String> alreadyInlined;
    for (int hop = 0; hop < 6; hop++) {
      std::vector<String> expandedRaw;
      std::vector<String> expandedLines;
      bool anyExpanded = false;
    for (size_t i = 0; i < rawLines.size(); i++) {
      String raw = rawLines[i];
      String t = raw; t.trim();
      String u = t; u.toUpperCase();
      // Match `EXTENSION NAME` (optionally followed by ˅ marker).
      bool isExtOpener = u.startsWith("EXTENSION ");
      if (!isExtOpener) {
        expandedRaw.push_back(raw);
        if (t.length() > 0) expandedLines.push_back(t);
        continue;
      }
      // Extract name (strip any trailing ˅ / ^ marker + whitespace).
      String rest = t.substring(String("EXTENSION ").length()); rest.trim();
      // v4.34 (bug-hunt MEDIUM #10): accept `EXTENSION name˅` with NO space
      // before the marker (some mobile keyboards, or after in-editor
      // collapse race). Match trailing `^`/`˅` with or without a leading
      // separator.
      bool collapsedMarker = false, inlineMarker = false;
      if (rest.endsWith("\xCB\x85")) { collapsedMarker = true; rest = rest.substring(0, rest.length() - 2); rest.trim(); }
      else if (rest.endsWith("^"))   { inlineMarker    = true; rest = rest.substring(0, rest.length() - 1); rest.trim(); }
      String extName = rest;
      // Look ahead: what does the block body look like?
      // v4.34 (bug-hunt CRITICAL #2): skip REM / // / REM_BLOCK lines when
      // deciding if the body is empty - a `EXTENSION os_detect / REM loads /
      // END_EXTENSION` was incorrectly detected as non-empty and silently
      // failed to inline.
      size_t j = i + 1;
      bool emptyBody = false;
      bool inRemBlock = false;
      while (j < rawLines.size()) {
        String tj = rawLines[j]; tj.trim();
        String uj = tj; uj.toUpperCase();
        if (inRemBlock) {
          if (uj == "END_REM") inRemBlock = false;
          j++;
          continue;
        }
        if (tj.length() == 0 || uj == "REM" || uj.startsWith("REM ") || tj.startsWith("//")) { j++; continue; }
        if (uj == "REM_BLOCK" || uj.startsWith("REM_BLOCK ")) { inRemBlock = true; j++; continue; }
        if (uj == "END_EXTENSION") { emptyBody = true; break; }
        break;   // some other content = non-empty body
      }
      // v4.34 (bug-hunt HIGH #6): the auto-inline path only runs when the
      // block form clearly asks for it (empty body OR the `˅` marker OR the
      // v4.32 collapsed shorthand). A user-written `EXTENSION FOO / ...body... /
      // END_EXTENSION` scope wrapper is a legitimate no-op and MUST NOT
      // spurious-error "EXTENSION not found on SD". Also skip when there's
      // no SD mounted - the lookup would fail anyway and just pollute
      // errorCount so the UI can't tell real errors from missing-SD noise.
      bool shouldExpand = !inlineMarker && !extName.isEmpty() && (collapsedMarker || emptyBody);
      if (!shouldExpand) {
        expandedRaw.push_back(raw);
        if (t.length() > 0) expandedLines.push_back(t);
        continue;
      }
      extern bool sdCardPresent;
      if (!sdCardPresent) {
        Serial.println("[EXTENSION] No SD - skipping inline of: " + extName);
        expandedRaw.push_back(raw);
        if (t.length() > 0) expandedLines.push_back(t);
        continue;
      }
      // Recursion guard - don't inline an ext that's already up the stack.
      String nameKey = extName; nameKey.toUpperCase();
      bool alreadyIn = false;
      for (auto& s : alreadyInlined) if (s == nameKey) { alreadyIn = true; break; }
      if (alreadyIn) {
        Serial.println("[EXTENSION] Skipping already-inlined: " + extName);
        expandedRaw.push_back(raw);
        if (t.length() > 0) expandedLines.push_back(t);
        continue;
      }
      // Try (folder, suffix) combinations - case-insensitive on the name.
      // v4.33: the user's SD may store `os_detect.txt` while the payload
      // writes `EXTENSION OS_DETECT` (uppercase). FAT is case-insensitive
      // on the host but ESP32's Arduino-SD wrapper does strict-case
      // matching. Fall back to a directory scan comparing uppercased names
      // if the exact path missed.
      const char* folders[]  = { "/extensions/hak5", "/extensions/custom", "/extensions" };
      const char* suffixes[] = { "", ".txt", ".ext", ".dsx" };
      String body = "";
      String foundPath = "";
      String extUpper = extName; extUpper.toUpperCase();
      for (const char* f : folders) {
        // (a) exact-case attempt across suffixes.
        for (const char* s : suffixes) {
          String path = String(f) + "/" + extName + s;
          File file = SD.open(path);
          if (file) {
            body = file.readString();
            file.close();
            foundPath = path;
            break;
          }
        }
        if (body.length()) break;
        // (b) case-insensitive directory scan - open the folder, walk names,
        // compare uppercased with/without suffix.
        File dir = SD.open(f);
        if (!dir || !dir.isDirectory()) { if (dir) dir.close(); continue; }
        File entry = dir.openNextFile();
        while (entry) {
          if (!entry.isDirectory()) {
            String name = String(entry.name());
            // ESP-SD returns just the leaf on newer cores, full path on older.
            int slash = name.lastIndexOf('/');
            if (slash >= 0) name = name.substring(slash + 1);
            String nameUp = name; nameUp.toUpperCase();
            String stemUp = nameUp;
            int dot = stemUp.lastIndexOf('.');
            if (dot >= 0) stemUp = stemUp.substring(0, dot);
            if (nameUp == extUpper || stemUp == extUpper) {
              String path = String(f) + "/" + name;
              File file = SD.open(path);
              if (file) {
                body = file.readString();
                file.close();
                foundPath = path;
              }
              entry.close();
              break;
            }
          }
          entry.close();
          entry = dir.openNextFile();
        }
        dir.close();
        if (body.length()) break;
      }
      if (body.length() == 0) {
        lastError = "EXTENSION not found on SD: " + extName;
        errorCount++;
        Serial.println("[EXTENSION] " + lastError);
        expandedRaw.push_back(raw);
        if (t.length() > 0) expandedLines.push_back(t);
        continue;
      }
      Serial.println("[EXTENSION] Inlined " + foundPath + " (" + String(body.length()) + " B)");
      anyExpanded = true;
      // If we had a collapsed-marker line, replace ONLY that line with:
      //   EXTENSION NAME ^
      //   <body>
      //   END_EXTENSION
      // If we had an empty-body pair (opener + END_EXTENSION), replace
      // BOTH lines with the same inline expansion.
      String openerOut = String("EXTENSION ") + extName + " ^";
      expandedRaw.push_back(openerOut);
      expandedLines.push_back(openerOut);
      alreadyInlined.push_back(nameKey);
      // Split body into lines and push each (same push logic as pushBoth).
      int bStart = 0;
      int bEnd = body.indexOf('\n');
      auto pushBody = [&](const String& r) {
        String rr = r;
        if (rr.length() && rr.charAt(rr.length()-1) == '\r') rr.remove(rr.length()-1);
        expandedRaw.push_back(rr);
        String tr = rr; tr.trim();
        if (tr.length() > 0) expandedLines.push_back(tr);
      };
      while (bEnd != -1) {
        pushBody(body.substring(bStart, bEnd));
        bStart = bEnd + 1;
        bEnd = body.indexOf('\n', bStart);
      }
      if (bStart < (int)body.length()) pushBody(body.substring(bStart));
      // Note: alreadyInlined stays populated until the outer per-run loop
      // exits (v4.34: needed so a chained inline can't recursively re-inline
      // the same body).
      expandedRaw.push_back("END_EXTENSION");
      expandedLines.push_back("END_EXTENSION");
      // Skip the empty-body pair's END_EXTENSION we already know about.
      if (emptyBody) {
        // Advance i to j (the END_EXTENSION line) so the outer loop skips it.
        i = j;
      }
    }
      rawLines = expandedRaw;
      lines    = expandedLines;
      if (!anyExpanded) break;   // fixed-point reached
    }
    // Recursion guard is a per-invocation local; nothing to reset here.
  }

  // v4.17: DuckyScript preprocessor pass for Hak5 extension syntax.
  //   * strip EXTENSION <name> / END_EXTENSION framing (no-op wrappers)
  //   * strip REM_BLOCK ... END_REM multi-line comment blocks
  //   * collect DEFINE #NAME value into a map; substitute #NAME everywhere
  //   * resolve IF_DEFINED_TRUE #NAME / ELSE_DEFINED / END_IF_DEFINED into
  //     kept-or-dropped lines
  //   * normalize `FUNCTION NAME()` to `FUNCTION NAME`
  //   * normalize `ELSE IF <cond>` to `ELIF <cond>`
  //   * strip trailing ` THEN` on IF / ELIF / ELSE-IF lines
  {
    std::vector<String> pre;
    std::map<String, String> defines;
    bool inRemBlock = false;
    // v4.21: track whether we're currently inside an EXTENSION block so we
    // can apply Hak5-strict semantics to `ATTACKMODE STORAGE` there (drop
    // HID) while OUR default (`STORAGE` keeps HID for UX safety) still
    // applies to normal user scripts. Same source, both semantics: extension
    // authors get spec-correct behaviour, casual users don't accidentally
    // brick themselves out.
    // v4.34 (bug-hunt HIGH #5): DEPTH counter, not bool. Pass 0 wraps
    // inlined ext bodies in their own `EXTENSION x ^ ... END_EXTENSION`,
    // so nested wrappers are common after Pass 0. A bool would clear on
    // the first END_EXTENSION and treat the rest of an outer wrapper as
    // "not in an extension" - the Hak5-strict ATTACKMODE STORAGE rewrite
    // would silently skip.
    int  extensionDepth = 0;
    auto inExtension = [&]() { return extensionDepth > 0; };
    // Pass 1: strip REM_BLOCK, EXTENSION, END_EXTENSION and gather DEFINEs.
    // v4.19 additions:
    //   * STRING_BASH / STRING_POWERSHELL / bare STRING starting a block:
    //     collect indented lines until END_STRING or END_STRINGLN, then emit
    //     as one STRING/STRINGLN per collected line with the shared leading
    //     whitespace stripped.
    //   * IF_NOT_DEFINED_TRUE : inverse of IF_DEFINED_TRUE (handled in Pass 3
    //     by flipping the "keep" bit).
    bool inStrBlock = false;
    bool strBlockTerminatorIsLn = false;  // END_STRINGLN vs END_STRING
    std::vector<String> strBlockLines;
    // v4.23 (bug-hunt HIGH #5): iterate rawLines (untrimmed + blank-preserving)
    // so STRING_BASH / STRING_POWERSHELL blocks keep indentation and blank
    // separators. For everything OUTSIDE those blocks we still work with the
    // trimmed form via `t`.
    for (auto& raw : rawLines) {
      String t = raw; t.trim();
      String upper = t; upper.toUpperCase();
      if (inRemBlock) { if (upper == "END_REM") inRemBlock = false; continue; }
      if (inStrBlock) {
        if (upper == "END_STRING" || upper == "END_STRINGLN") {
          // Strip common leading whitespace across all collected lines.
          size_t minIndent = SIZE_MAX;
          for (auto& s : strBlockLines) {
            if (s.length() == 0) continue;
            size_t i = 0;
            while (i < s.length() && (s.charAt(i) == ' ' || s.charAt(i) == '\t')) i++;
            if (i < minIndent) minIndent = i;
          }
          if (minIndent == SIZE_MAX) minIndent = 0;
          // Emit as STRINGLN <line> per line so ENTER separates them. Last
          // line uses STRING (no trailing enter) unless the terminator was
          // END_STRINGLN, which then also adds a trailing ENTER via STRINGLN.
          for (size_t i = 0; i < strBlockLines.size(); i++) {
            String s = strBlockLines[i].substring(minIndent);
            String cmd = (i + 1 == strBlockLines.size() && !strBlockTerminatorIsLn && upper == "END_STRING") ? "STRING " : "STRINGLN ";
            pre.push_back(cmd + s);
          }
          inStrBlock = false;
          strBlockLines.clear();
          continue;
        }
        // Preserve the raw line (indentation stays for indent-strip below).
        strBlockLines.push_back(raw);
        continue;
      }
      if (upper == "REM_BLOCK" || upper.startsWith("REM_BLOCK ")) { inRemBlock = true; continue; }
      if (upper == "EXTENSION" || upper.startsWith("EXTENSION ")) { extensionDepth++; continue; }
      if (upper == "END_EXTENSION") { if (extensionDepth > 0) extensionDepth--; continue; }
      // v4.21: Hak5-strict semantics inside EXTENSION blocks - rewrite a
      // bare `ATTACKMODE STORAGE` (without HID mentioned anywhere) into
      // `ATTACKMODE STORAGE_ONLY` so the composite drops HID like the Hak5
      // spec says. Outside extension blocks, our UX-friendly HID+STORAGE
      // default still applies.
      if (inExtension() && upper.startsWith("ATTACKMODE ")) {
        String args = upper.substring(String("ATTACKMODE ").length());
        // "STORAGE" appears but neither HID nor HID_STORAGE nor STORAGE_ONLY
        // is already there? Then it's the Hak5-bare form; rewrite.
        bool hasStorage      = (args.indexOf("STORAGE") >= 0);
        bool hasStorageOnly  = (args.indexOf("STORAGE_ONLY") >= 0);
        bool hasHid          = (args.indexOf("HID") >= 0);
        bool hasNoHid        = (args.indexOf("NO_HID") >= 0) ||
                               (args.indexOf("NOHID") >= 0) ||
                               (args.indexOf("HID_OFF") >= 0);
        if (hasStorage && !hasStorageOnly && !hasHid && !hasNoHid) {
          // v4.23 (bug-hunt HIGH #4): token-based swap, not substring. The
          // previous String::replace("STORAGE", "STORAGE_ONLY") would also
          // corrupt VID_STORAGE_XYZ or any user-defined DEFINE that happened
          // to contain the substring STORAGE. Now we tokenize on whitespace
          // and swap only the exact standalone STORAGE token.
          String rewritten = "";
          int start = 0;
          while (start < (int)raw.length()) {
            while (start < (int)raw.length() && (raw.charAt(start) == ' ' || raw.charAt(start) == '\t')) {
              rewritten += raw.charAt(start); start++;
            }
            int end = start;
            while (end < (int)raw.length() && raw.charAt(end) != ' ' && raw.charAt(end) != '\t') end++;
            String tok = raw.substring(start, end);
            String tokUp = tok; tokUp.toUpperCase();
            rewritten += (tokUp == "STORAGE") ? String("STORAGE_ONLY") : tok;
            start = end;
          }
          pre.push_back(rewritten);
          continue;
        }
      }
      if (upper == "STRING_BASH" || upper == "STRING_POWERSHELL") {
        inStrBlock = true; strBlockTerminatorIsLn = false; continue;
      }
      if (upper.startsWith("DEFINE ")) {
        // DEFINE #NAME value... - value is the rest of the line.
        int sp1 = t.indexOf(' ');
        int sp2 = t.indexOf(' ', sp1 + 1);
        if (sp2 > sp1) {
          String name  = t.substring(sp1 + 1, sp2); name.trim();
          String value = t.substring(sp2 + 1); value.trim();
          if (name.length() > 0) defines[name] = value;
        } else if (sp1 > 0) {
          // DEFINE #NAME (no value) - default to empty.
          String name = t.substring(sp1 + 1); name.trim();
          if (name.length() > 0) defines[name] = "";
        }
        continue;
      }
      pre.push_back(raw);
    }
    // Pass 2: substitute #NAME in every remaining line.
    // v4.17-post-hunt BUG #2 fix: sort keys by descending length so #NAMES is
    // substituted BEFORE #NAME (otherwise the shorter one corrupts the longer),
    // and enforce word-boundary matching so #N inside #NAME doesn't get eaten.
    {
      std::vector<String> sortedKeys;
      for (auto& kv : defines) sortedKeys.push_back(kv.first);
      std::sort(sortedKeys.begin(), sortedKeys.end(),
                [](const String& a, const String& b){ return a.length() > b.length(); });
      for (auto& l : pre) {
        for (auto& k : sortedKeys) {
          const String& v = defines[k];
          int idx = 0;
          while ((idx = l.indexOf(k, idx)) >= 0) {
            int endIdx = idx + k.length();
            // Boundary check: the char right after the token must NOT be a
            // valid identifier character (letter/digit/underscore). We
            // implicitly know the char at idx-1 is not #-prefix-continuation
            // because tokens all start with # and #-inside-identifier is
            // not a thing.
            char after = (endIdx < (int)l.length()) ? l.charAt(endIdx) : ' ';
            if (isalnum((unsigned char)after) || after == '_') { idx = endIdx; continue; }
            l = l.substring(0, idx) + v + l.substring(endIdx);
            idx += v.length();
          }
        }
      }
    }
    // Pass 3: resolve IF_DEFINED_TRUE / ELSE_DEFINED / END_IF_DEFINED.
    // Truthy = non-empty AND not equal to "FALSE"/"0".
    std::vector<String> resolved;
    struct DefStackFrame { bool keeping; bool elseSeen; };
    std::vector<DefStackFrame> defStack;
    for (auto& l : pre) {
      String t = l; t.trim();
      String upper = t; upper.toUpperCase();
      if (upper.startsWith("IF_DEFINED_TRUE") || upper.startsWith("IF_NOT_DEFINED_TRUE")) {
        // v4.20 Hak5 extras: IF_NOT_DEFINED_TRUE is the inverse of
        // IF_DEFINED_TRUE (used by community extensions to guard against
        // duplicate function definitions).
        bool inverted = upper.startsWith("IF_NOT_DEFINED_TRUE");
        int consumed = inverted ? (int)String("IF_NOT_DEFINED_TRUE").length()
                                 : (int)String("IF_DEFINED_TRUE").length();
        String rest = t.substring(consumed); rest.trim();
        String restUp = rest; restUp.toUpperCase();
        // v4.17-post-hunt BUG #8 fix: an undefined `#NAME` will pass through
        // the substitution pass unchanged (still starts with `#`). Treat that
        // as falsy, matching the Hak5 semantics of the command's name.
        bool truthy;
        if (rest.startsWith("#")) truthy = false;
        else truthy = (rest.length() > 0 && restUp != "FALSE" && rest != "0");
        bool keep = inverted ? !truthy : truthy;
        // Nested inside a currently-skipping block? Then this whole subtree is skipped.
        if (!defStack.empty() && !defStack.back().keeping) keep = false;
        defStack.push_back({ keep, false });
        continue;
      }
      if (upper == "ELSE_DEFINED") {
        if (!defStack.empty() && !defStack.back().elseSeen) {
          // Flip only if the parent is keeping (otherwise we stay skipped).
          bool parentKeep = (defStack.size() < 2) || defStack[defStack.size()-2].keeping;
          if (parentKeep) defStack.back().keeping = !defStack.back().keeping;
          defStack.back().elseSeen = true;
        }
        continue;
      }
      if (upper == "END_IF_DEFINED" || upper == "END_IF_NOT_DEFINED_TRUE" || upper == "END_IF_NOT_DEFINED") {
        // v4.20: accept the not-defined variants too.
        if (!defStack.empty()) defStack.pop_back();
        continue;
      }
      // Keep line only if all enclosing IF_DEFINED_TRUE branches keep it.
      bool keeping = true;
      for (auto& f : defStack) if (!f.keeping) { keeping = false; break; }
      if (!keeping) continue;

      // Pass 4 normalizations, applied inline while emitting.
      String out = t;
      String outUp = out; outUp.toUpperCase();
      // FUNCTION NAME() -> FUNCTION NAME
      if (outUp.startsWith("FUNCTION ")) {
        int lparen = out.indexOf('(');
        int rparen = out.indexOf(')');
        if (lparen > 0 && rparen > lparen) out.remove(lparen, (rparen - lparen) + 1);
      }
      // ELSE IF -> ELIF
      if (outUp.startsWith("ELSE IF ")) {
        out = "ELIF" + out.substring(String("ELSE IF").length());
      }
      // Trailing THEN
      String outUp2 = out; outUp2.toUpperCase(); outUp2.trim();
      if (outUp2.endsWith(" THEN")) {
        out = out.substring(0, out.length() - 5);
        out.trim();
      }
      resolved.push_back(out);
    }
    lines = resolved;
  }

  int totalLines = lines.size();
  std::vector<LoopState> loopStack;
  int i = 0;
  int skipDepth = 0;
  bool skipActive = false;
  std::vector<int> callStack;
  std::vector<bool> ifHandledStack;
  std::map<String, int> functionTable;

  // Pre-scan for functions
  for (int j = 0; j < lines.size(); j++) {
    String fLine = lines[j];
    fLine.trim();
    if (fLine.startsWith("FUNCTION ")) {
      String funcName = fLine.substring(9);
      funcName.trim();
      if (funcName.endsWith("()")) funcName = funcName.substring(0, funcName.length() - 2);
      functionTable[funcName] = j;
    }
  }

  // Handle BEGIN_ROWER block
  bool inRowerBlock = false;
  std::vector<String> rowerPayloads;

  while (i < lines.size() && !stopRequested) {
    currentLineNum = i + 1;
    String line = lines[i];
    line.trim();

    if (line.length() == 0 || line.startsWith("REM") || line.startsWith("//")) {
      i++;
      continue;
    }

    if (skipActive) {
      if (line.startsWith("IF") || line.startsWith("FOR") || line.startsWith("FUNCTION ")) {
        skipDepth++;
      } else if (line.startsWith("ENDIF") || line.startsWith("END_IF") || line.startsWith("ENDFOR") || line.startsWith("END_FOR") || line == "END_FUNCTION") {
        if (skipDepth == 0) skipActive = false;
        else skipDepth--;
      }
      i++;
      continue;
    }

    if (line.startsWith("BEGIN_ROWER")) {
      inRowerBlock = true;
      i++;
      continue;
    }

    if (line == "END_ROWER") {
      inRowerBlock = false;
      rower.payloads = rowerPayloads;
      rower.currentPayloadIdx = 0;
      rower.active = true;
      i++;
      continue;
    }

    if (inRowerBlock) {
      rowerPayloads.push_back(line);
      i++;
      continue;
    }

    if (line.startsWith("RUN_ON_REBOOT")) {
      i++;
      String payload = "";
      int depth = 1;
      while (i < lines.size() && depth > 0) {
        String subLine = lines[i];
        subLine.trim();
        if (subLine.startsWith("IF") || subLine.startsWith("FOR") || subLine.startsWith("WHILE")) depth++;
        else if (subLine.startsWith("ENDIF") || subLine.startsWith("END_IF") || subLine.startsWith("ENDFOR") || subLine.startsWith("END_FOR") || subLine.startsWith("END_WHILE")) depth--;
        
        if (depth > 0) {
          payload += lines[i] + "\n";
          i++;
        }
      }
      if (payload.length() > 0) {
        File f = SD.open("/reboot_script.txt", FILE_WRITE);
        if (f) {
          f.print(payload);
          f.close();
          Serial.println("Reboot payload saved to SD");
        }
      }
      if (i < lines.size()) {
          String endLine = lines[i];
          endLine.trim();
          if (endLine.startsWith("END_RUN_ON_REBOOT")) i++;
          else if (endLine.startsWith("ENDIF") || endLine.startsWith("END_IF")) i++;
      }
      continue;
    }

    // RANDOM USB Identity commands — apply change, save remaining script, and reboot
    auto isRandomUSBCmd = [](const String& l) {
      return l == "RANDOM_VID" || l == "RANDOM_PID" || l == "RANDOM_MAN" || l == "RANDOM_PRODUCT" ||
             l.startsWith("RANDOM_VID ") || l.startsWith("RANDOM_PID ") ||
             l.startsWith("RANDOM_MAN ") || l.startsWith("RANDOM_PRODUCT ");
    };

    if (isRandomUSBCmd(line)) {
      // Apply the randomization to the matching field
      String cmd = line.substring(0, line.indexOf(' ') == -1 ? line.length() : line.indexOf(' '));
      cmd.trim();
      if (cmd == "RANDOM_VID") {
        char buf[7]; sprintf(buf, "0x%04x", (uint16_t)(esp_random() & 0xFFFF));
        preferences.putString("usb_vid", String(buf));
        Serial.println("RANDOM_VID: " + String(buf));
      } else if (cmd == "RANDOM_PID") {
        char buf[7]; sprintf(buf, "0x%04x", (uint16_t)(esp_random() & 0xFFFF));
        preferences.putString("usb_pid", String(buf));
        Serial.println("RANDOM_PID: " + String(buf));
      } else if (cmd == "RANDOM_MAN") {
        const char* mfrs[] = {"Microsoft", "Logitech", "Dell", "Apple", "HP", "Lenovo", "Asus", "Samsung"};
        String mfr = mfrs[esp_random() % 8];
        preferences.putString("usb_mfr", mfr);
        Serial.println("RANDOM_MAN: " + mfr);
      } else if (cmd == "RANDOM_PRODUCT") {
        const char* prods[] = {"USB Keyboard", "HID Device", "Wireless Dongle", "USB Hub", "Flash Drive"};
        String prod = prods[esp_random() % 5];
        preferences.putString("usb_prod", prod);
        Serial.println("RANDOM_PRODUCT: " + prod);
      }
      // Save remaining script lines to /temp_resume.txt
      i++;
      String remaining = "";
      while (i < lines.size()) {
        String remLine = lines[i];
        remLine.trim();
        if (remLine.length() > 0) remaining += remLine + "\n";
        i++;
      }
      if (remaining.length() > 0 && sdCardPresent) {
        // v4.36 bug-hunt MEDIUM #14: append-mode guard.
        if (SD.exists("/temp_resume.txt")) SD.remove("/temp_resume.txt");
        File f = SD.open("/temp_resume.txt", FILE_WRITE);
        if (f) { f.print(remaining); f.close(); }
        Serial.println("Resume script saved. Rebooting for USB identity change...");
      }
      delay(500);
      ESP.restart();
      return; // Never reached
    }

    if (line.startsWith("IF_NOT_PRESENT ")) {
      String target = line.substring(15);
      target.trim();
      bool isPresent = false;
      
      if (target == "SD") isPresent = sdCardPresent;
      else if (target.startsWith("SSID=\"")) {
        int q1 = target.indexOf('"') + 1;
        int q2 = target.indexOf('"', q1);
        if (q2 > q1) {
          String ssid = target.substring(q1, q2);
          scanWiFi();
          isPresent = isSSIDPresent(ssid);
        }
      } else if (target == "WIFI") isPresent = (WiFi.status() == WL_CONNECTED);
      else if (target == "BT" || target == "BLUETOOTH") isPresent = (getBTClientCount() > 0);

      if (isPresent) {
        // Condition NOT met (we want NOT present), so skip the block
        ifHandledStack.push_back(false);
        skipActive = true;
        skipDepth = 0;
      } else {
        // Condition met (it is NOT present)
        ifHandledStack.push_back(true);
      }
      i++;
      continue;
    }

    if (line.startsWith("FUNCTION ") || line.startsWith("DEF_")) {
      skipActive = true;
      skipDepth = 0;
      i++;
      continue;
    }

    if (line == "END_FUNCTION" || line == "RETURN") {
      if (!callStack.empty()) {
        i = callStack.back() + 1;
        callStack.pop_back();
        continue;
      }
      i++;
      continue;
    }

    if (line.startsWith("FOR ")) {
      String forParams = line.substring(4);
      forParams.trim();
      int fromIdx = forParams.indexOf("FROM ");
      int toIdx = forParams.indexOf("TO ");
      int stepIdx = forParams.indexOf("STEP ");
      
      if (fromIdx != -1 && toIdx != -1) {
        String varName = forParams.substring(0, fromIdx);
        varName.trim();
        int startVal = forParams.substring(fromIdx + 5, toIdx).toInt();
        int endVal;
        int stepVal = 1;
        if (stepIdx != -1) {
          endVal = forParams.substring(toIdx + 3, stepIdx).toInt();
          stepVal = forParams.substring(stepIdx + 5).toInt();
        } else {
          endVal = forParams.substring(toIdx + 3).toInt();
        }
        LoopState loop = {i, startVal, endVal, varName, stepVal};
        loopStack.push_back(loop);
        variables[varName] = String(startVal);
      }
      i++;
      continue;
    }

    if (line.startsWith("ENDFOR") || line.startsWith("END_FOR")) {
      if (!loopStack.empty()) {
        LoopState& loop = loopStack.back();
        loop.currentIteration += loop.step;
        if (loop.currentIteration <= loop.totalIterations) {
          variables[loop.varName] = String(loop.currentIteration);
          i = loop.startLine + 1;
          continue;
        } else {
          loopStack.pop_back();
        }
      }
      i++;
      continue;
    }

    bool isIf = false;
    bool conditionMet = false;

    if (line.startsWith("IF_PRESENT SSID=\"")) {
      isIf = true;
      int quoteStart = line.indexOf('"') + 1;
      int quoteEnd = line.indexOf('"', quoteStart);
      if (quoteEnd > quoteStart) {
        String ssid = line.substring(quoteStart, quoteEnd);
        scanWiFi();
        conditionMet = isSSIDPresent(ssid);
      }
    } else if (line.startsWith("IF_NOTPRESENT SSID=\"")) {
      isIf = true;
      int quoteStart = line.indexOf('"') + 1;
      int quoteEnd = line.indexOf('"', quoteStart);
      if (quoteEnd > quoteStart) {
        String ssid = line.substring(quoteStart, quoteEnd);
        scanWiFi();
        conditionMet = !isSSIDPresent(ssid);
      }
    } else if (line.startsWith("IF_BT_PRESENT \"")) {
      isIf = true;
      int quoteStart = line.indexOf('"') + 1;
      int quoteEnd = line.indexOf('"', quoteStart);
      if (quoteEnd > quoteStart) {
        String name = line.substring(quoteStart, quoteEnd);
        scanBT();
        conditionMet = isBTDevicePresent(name);
      }
    } else if (line.startsWith("IF_CLIENT_CONNECTED_BLUETOOTH")) {
      isIf = true;
      conditionMet = (getBTClientCount() > 0);
    } else if (line.startsWith("IF_CLIENT_CONNECTED_WIFI")) {
      isIf = true;
      conditionMet = (WiFi.softAPgetStationNum() > 0);
    } else if (line.startsWith("IF_CLIENT_DISCONNECTED_WIFI")) {
      isIf = true;
      conditionMet = (WiFi.softAPgetStationNum() == 0);
    } else if (line.startsWith("IF_ONLINE")) {
      isIf = true;
      conditionMet = (WiFi.status() == WL_CONNECTED);
    } else if (line.startsWith("IF_OFFLINE")) {
      isIf = true;
      conditionMet = (WiFi.status() != WL_CONNECTED);
    } else if (line.startsWith("IF_OS ")) {
      isIf = true;
      String targetOS = line.substring(6);
      targetOS.trim();
      conditionMet = (detectedOS.equalsIgnoreCase(targetOS));
    } else if (line.startsWith("IF_DETECT_OS_INCLUDES = \"")) {
      isIf = true;
      int q1 = line.indexOf('"') + 1;
      int q2 = line.indexOf('"', q1);
      if (q2 > q1) {
        String target = line.substring(q1, q2);
        conditionMet = (detectedOS.indexOf(target) != -1);
      }
    } else if (line.startsWith("IF_CLIENT_CONNECTED")) {
      isIf = true;
      conditionMet = (WiFi.softAPgetStationNum() > 0 || getBTClientCount() > 0);
    } else if (line.startsWith("IF_CLIENT_DISCONNECTED_BLUETOOTH")) {
      isIf = true;
      conditionMet = (getBTClientCount() == 0);
    } else if (line.startsWith("IF_CLIENT_DISCONNECTED")) {
      isIf = true;
      conditionMet = (WiFi.softAPgetStationNum() == 0 && getBTClientCount() == 0);
    } else if (line.startsWith("IF_CONNECTED_TO_WIFI")) {
      isIf = true;
      conditionMet = (WiFi.status() == WL_CONNECTED);
    } else if (line.startsWith("IF ")) {
      isIf = true;
      conditionMet = evalCondition(line.substring(3));
    } else if (line.startsWith("IF_CLIENT_CONNECTED_DISCONNECTED_BLUETOOTH")) {
      isIf = true;
      conditionMet = true; // Triggered if reached
    } else if (line.startsWith("IF_CLIENT_CONNECTED_DISCONNECTED_WIFI")) {
      isIf = true;
      conditionMet = true;
    } else if (line.startsWith("IF_CLIENT_CONNECTED_DISCONNECTED")) {
      isIf = true;
      conditionMet = true;
    } else if (line.startsWith("IF_")) {
      isIf = true;
      conditionMet = evalCondition(line);
    }


    if (isIf) {
      ifHandledStack.push_back(conditionMet);
      if (!conditionMet) {
        skipActive = true;
        skipDepth = 0;
      }
      i++;
      continue;
    }

    if (line.startsWith("ELIF ")) {
      if (!skipActive) {
        skipActive = true;
        skipDepth = 0;
      } else if (skipDepth == 0) {
        if (!ifHandledStack.empty() && !ifHandledStack.back()) {
          conditionMet = evalCondition(line.substring(5));
          if (conditionMet) {
            skipActive = false;
            ifHandledStack.back() = true;
          }
        }
      }
      i++;
      continue;
    }
    
    if (line.startsWith("ELIF_")) {
      if (!skipActive) {
        skipActive = true;
        skipDepth = 0;
      } else if (skipDepth == 0) {
        if (!ifHandledStack.empty() && !ifHandledStack.back()) {
          conditionMet = evalCondition(line.substring(5));
          if (conditionMet) {
            skipActive = false;
            ifHandledStack.back() = true;
          }
        }
      }
      i++;
      continue;
    }

    if (line == "ELSE" || line == "ELSE:") {
      if (!skipActive) {
        skipActive = true;
        skipDepth = 0;
      } else if (skipDepth == 0) {
        if (!ifHandledStack.empty() && !ifHandledStack.back()) {
          skipActive = false;
        }
      }
      i++;
      continue;
    }

    if (line.startsWith("ENDIF") || line.startsWith("END_IF")) {
      if (!ifHandledStack.empty() && skipDepth == 0) ifHandledStack.pop_back();
      if (skipDepth > 0) skipDepth--;
      else skipActive = false;
      i++;
      continue;
    }

    String potentialFunc = line;
    if (potentialFunc.endsWith("()")) potentialFunc = potentialFunc.substring(0, potentialFunc.length() - 2);
    if (functionTable.find(potentialFunc) != functionTable.end()) {
      callStack.push_back(i);
      i = functionTable[potentialFunc] + 1;
      continue;
    }

    executeCommand(line);
    totalCommandsExecuted++;
    if (stopRequested) break;
    // v4.34 bug-hunt CRITICAL #1: if the command we just ran was an
    // ATTACKMODE that flipped HID/STORAGE composition (which requires a
    // full reboot to rebuild USB descriptors), persist the REMAINING lines
    // to /temp_resume.txt and restart. Without this the reboot lost every
    // instruction after the ATTACKMODE call.
    extern volatile bool g_composeRebootPending;
    if (g_composeRebootPending) {
      g_composeRebootPending = false;
      if (sdCardPresent) {
        // v4.35: save the WHOLE ORIGINAL top-level script (not the
        // internal `lines[i+1..]` slice). Rationale: if the ATTACKMODE
        // fired from inside a FUNCTION body (typical - OS_DETECT's body
        // is where the identity flip lives), the slice would resume
        // mid-function-body without its FUNCTION declaration, breaking
        // later `NAME()` calls. Re-running the whole script after boot
        // is safe: the ATTACKMODE call it hits again will see identity
        // already matches (NVS was updated pre-reboot) → no-op → the
        // script continues past it naturally.
        extern String g_currentTopScript;
        const String& payload = g_currentTopScript;
        if (payload.length() > 0) {
          // v4.36 bug-hunt MEDIUM #14: on some ESP32-Arduino SD library
          // versions FILE_WRITE opens in append ("a") mode - remove any
          // stale copy first so we get a clean truncate/replace.
          if (SD.exists("/temp_resume.txt")) SD.remove("/temp_resume.txt");
          File f = SD.open("/temp_resume.txt", FILE_WRITE);
          if (f) { f.print(payload); f.close(); }
          Serial.printf("[ATTACKMODE] Persisted %u B (whole script) to /temp_resume.txt\n",
                        (unsigned)payload.length());
        }
      }
      Serial.println("[ATTACKMODE] Unmounting USB before reboot...");
      tud_disconnect();
      delay(600);
      Serial.println("[ATTACKMODE] Rebooting to rebuild USB descriptors...");
      ESP.restart();
      return; // never reached
    }
    if (defaultDelay > 0) {
      unsigned long delayStart = millis();
      while (millis() - delayStart < (unsigned long)defaultDelay && !stopRequested) delay(10);
    }
    i++;
  }

  // v4.25 bug-hunt HIGH #3: only run the tail cleanup at the outermost level
  // so a nested IMPORT/RUN_EXTENSION doesn't clear scriptRunning, blink the
  // completion LED, close the outer's log file, or detach HID mid-outer.
  g_scriptDepth--;
  if (g_scriptDepth != 0) return;

  scriptRunning = false;
  if (loggingEnabled) {
    if (stopRequested) logCommand("SCRIPT_STOP", "Stopped at line " + String(currentLineNum));
    else logCommand("SCRIPT_END", "Completed successfully");
    closeLogFile();
  }

  if (stopRequested) {
    stopRequested = false;
    setLEDMode(0);
  } else {
    showCompletionBlink();
  }

  hidReleaseIfSilent();   // stealth: detach the keyboard again after the script
}

void executeCommand(String line) {
  if (stopRequested) return;

  if (!line.startsWith("REPEAT ")) {
    lastCommand = line;
  }

  addToHistory(line);

  // Hak5-compatible ATTACKMODE / SIZE_XX_XX (may reboot to re-enumerate USB)
  if (handleAttackModeLine(line)) return;
  if (handleSizeLine(line))       return;

  if (line.startsWith("STRING ")) {
    fastTypeString(processVariables(line.substring(7)));
    if (holdTillStringActive) {
      releaseAllKeys();
      holdTillStringActive = false;
    }
    return;
  }

  if (line.startsWith("STRINGLN ")) {
    fastTypeString(processVariables(line.substring(9)));
    fastPressKey("ENTER");
    if (holdTillStringActive) {
      releaseAllKeys();
      holdTillStringActive = false;
    }
    return;
  }

  if (line == "HOLD_TILL_STRING") {
    holdTillStringActive = true;
    return;
  }

  if (line.startsWith("DEFAULTDELAY ") || line.startsWith("DEFAULT_DELAY ")) {
    defaultDelay = line.substring(line.indexOf(' ') + 1).toInt();
    return;
  }

  // v4.18b Hak5 3.0 spec extras: per-character type delay.
  if (line.startsWith("DEFAULTCHARDELAY ") || line.startsWith("DEFAULT_CHAR_DELAY ")) {
    delayBetweenKeys = line.substring(line.indexOf(' ') + 1).toInt();
    return;
  }

  // v4.18b IMPORT <filename> - Hak5 alias for RUN_PAYLOAD.
  // v4.23 (bug-hunt MEDIUM #8): surface a missing-file error instead of
  // silently no-op'ing. Users were left with the calling script continuing
  // as if the IMPORT populated state (e.g. $_OS) when in fact nothing ran.
  if (line.startsWith("IMPORT ")) {
    String f = line.substring(7); f.trim();
    if (!f.startsWith("/")) f = "/scripts/" + f;
    File file = SD.open(f);
    if (file) {
      String content = file.readString();
      file.close();
      // v4.25 bug-hunt HIGH #4: save/restore BUTTON_DEF-collecting flag so
      // an IMPORT nested inside a BUTTON_DEF body doesn't leak the block's
      // "swallow every line" state after the imported script clears it.
      extern bool g_buttonDefActive;
      bool wasBD = g_buttonDefActive;
      executeScript(content);      // depth-counter in executeScript handles reentry
      g_buttonDefActive = wasBD;
    } else {
      lastError = "IMPORT: file not found: " + f;
      errorCount++;
      Serial.println("[IMPORT] " + lastError);
    }
    return;
  }

  // v4.18b WAIT_FOR_CAPS_ON/OFF, WAIT_FOR_NUM_ON/OFF, WAIT_FOR_SCROLL_ON/OFF.
  // Poll the mirror vars (kept in sync by the LED-report hook in .ino).
  {
    struct WaitDef { const char* cmd; const char* var; bool wantOn; };
    static const WaitDef waits[] = {
      {"WAIT_FOR_CAPS_ON",     "_CAPSLOCK_ON",   true},
      {"WAIT_FOR_CAPS_OFF",    "_CAPSLOCK_ON",   false},
      {"WAIT_FOR_NUM_ON",      "_NUMLOCK_ON",    true},
      {"WAIT_FOR_NUM_OFF",     "_NUMLOCK_ON",    false},
      {"WAIT_FOR_SCROLL_ON",   "_SCROLLLOCK_ON", true},
      {"WAIT_FOR_SCROLL_OFF",  "_SCROLLLOCK_ON", false},
    };
    for (auto& w : waits) {
      if (line == w.cmd) {
        unsigned long start = millis();
        while (!stopRequested && (millis() - start) < 30000) {
          String cur = variables[w.var];
          bool isOn = (cur == "TRUE");
          if (isOn == w.wantOn) break;
          handleLED(); server.handleClient(); comShellLoop();
          extern void pumpButton(); pumpButton();     // v4.26 HIGH #1
          extern void hostLedTick(); hostLedTick();   // v4.26: also mirror lock LEDs during a lock-wait
          delay(20);
        }
        return;
      }
    }
  }

  // v4.18b HOLD <key> / RELEASE <key> - hold a modifier indefinitely, then
  // release when RELEASE is called (or on releaseAllKeys). Distinct from
  // the existing HOLD_TILL_STRING wait.
  if (line.startsWith("HOLD ") && !line.startsWith("HOLD_TILL_STRING")) {
    String k = line.substring(5); k.trim();
    pressKeyOnly(k);
    return;
  }
  if (line.startsWith("RELEASE ") || line == "STOPHOLD") {
    releaseAllKeys();
    return;
  }

  // v4.24 (bug-hunt HIGH #6 fix): block until a physical GPIO0 press via
  // the SHARED button-event bus (pumpButton in the .ino) so the 3s "arm"
  // indicator + 10s factory-reset detector still work while we wait. Prior
  // versions spun on digitalRead(RESET_BUTTON_PIN) directly which starved
  // the main loop of the very poller they now cooperate with.
  if (line == "WAIT_FOR_BUTTON_PRESS") {
    extern void pumpButton();
    extern volatile bool g_buttonShortPressed;
    extern bool g_buttonSuppressStop;
    // v4.26 bug-hunt HIGH: save/restore prior g_buttonSuppressStop so a
    // nested WAIT_FOR_BUTTON_PRESS (reached via RUN_EXTENSION / IMPORT) can't
    // clobber the outer wait's suppress state when it exits.
    bool prevSuppress = g_buttonSuppressStop;
    g_buttonShortPressed = false;   // drop any stale edge before we wait
    g_buttonSuppressStop = true;    // don't let the shared bus stopRequested us
    unsigned long start = millis();
    while (!g_buttonShortPressed && !stopRequested && (millis() - start) < 300000UL) {
      pumpButton();
      handleLED();
      server.handleClient();
      comShellLoop();
      extern void hostLedTick(); hostLedTick();
      delay(20);
    }
    g_buttonSuppressStop = prevSuppress;
    g_buttonShortPressed = false;
    return;
  }

  // v4.24: INJECT_MOD <MOD1> [MOD2] ... - one-shot combo press-and-release
  // of the listed modifier keys (Hak5 3.0 spec). Distinct from HOLD which
  // latches until RELEASE.
  if (line.startsWith("INJECT_MOD ")) {
    // v4.26 bug-hunt HIGH: run processVariables so `INJECT_MOD $MOD` (or any
    // runtime-substituted token) resolves before we tokenize. Without this
    // `$MOD` was looked up literally as "$MOD" and hit the "not found" path.
    String rest = processVariables(line.substring(11)); rest.trim();
    std::vector<String> keys;
    int p = 0;
    while (p < (int)rest.length()) {
      while (p < (int)rest.length() && (rest.charAt(p) == ' ' || rest.charAt(p) == '\t')) p++;
      int q = p;
      while (q < (int)rest.length() && rest.charAt(q) != ' ' && rest.charAt(q) != '\t') q++;
      if (q > p) keys.push_back(rest.substring(p, q));
      p = q;
    }
    if (!keys.empty()) fastPressKeyCombination(keys);
    return;
  }

  // v4.24 real BUTTON_DEF ... END_BUTTON binding. We capture the body into
  // a global g_buttonHandlerScript at define-time; the .ino's pumpButton()
  // detects a short-press and calls back into runButtonHandler() which
  // re-enters executeScript on the captured body. DISABLE_BUTTON clears the
  // binding. Nested BUTTON_DEF blocks are not allowed (last-wins).
  // v4.23 (bug-hunt HIGH #2): g_buttonDefActive is reset per script run at
  // executeScript's entry so a partial capture from a previous run cannot
  // swallow the top of the next one.
  extern bool g_buttonDefActive;
  extern String g_buttonHandlerScript;
  if (line == "BUTTON_DEF" || line.startsWith("BUTTON_DEF ")) {
    g_buttonDefActive = true;
    g_buttonHandlerScript = "";     // last-wins; drop any previous binding
    return;
  }
  if (line == "END_BUTTON") { g_buttonDefActive = false; return; }
  if (g_buttonDefActive) {          // accumulate body verbatim (post-preproc)
    g_buttonHandlerScript += line;
    g_buttonHandlerScript += '\n';
    return;
  }
  if (line == "DISABLE_BUTTON") { g_buttonHandlerScript = ""; return; }

  // v4.19c Payload-state DSL - REAL implementations (previously stubs).
  //   HIDE_PAYLOAD    - detach the HID interface so the host stops seeing
  //                     the ducky as a keyboard (used to interleave with
  //                     ATTACKMODE STORAGE via self_destruct.txt's PERSIST
  //                     and REVERT_TO_THUMBDRIVE)
  //   RESTORE_PAYLOAD - re-attach HID + wait for the host to bind the driver
  //   STOP_PAYLOAD    - halt the current script cleanly (mirrors the /stop
  //                     endpoint's behaviour)
  if (line == "HIDE_PAYLOAD") {
    hidDetach();
    return;
  }
  if (line == "RESTORE_PAYLOAD") {
    ensureHidReady();
    return;
  }
  if (line == "STOP_PAYLOAD") {
    stopRequested = true;
    return;
  }

  // v4.19c WAIT_FOR_SCROLL_CHANGE - poll $_SCROLLLOCK_ON until it flips from
  // the value it had when the command started. Used by community exfil
  // extensions (WINDOWS_FILELESS_HID_EXFIL) as a per-line handshake with
  // the host script that toggles Scroll Lock after each processed batch.
  if (line == "WAIT_FOR_SCROLL_CHANGE") {
    String initial = variables["_SCROLLLOCK_ON"];
    unsigned long start = millis();
    // 60 s ceiling so a lost host signal doesn't wedge the payload forever.
    while (!stopRequested && (millis() - start) < 60000UL) {
      if (variables["_SCROLLLOCK_ON"] != initial) break;
      handleLED(); server.handleClient(); comShellLoop();
      extern void pumpButton(); pumpButton();   // v4.26 HIGH #1: 10s factory-reset must still fire during this wait
      extern void hostLedTick(); hostLedTick();   // v4.23: pump LED mirror while blocked
      delay(20);
    }
    return;
  }

  // v4.23 (bug-hunt MEDIUM #7): the SOFT_BRICK/REVERT_TO_THUMBDRIVE handlers
  // previously lived here as one-liners AND again below as v4.20 versions
  // with proper NVS persistence. The v4.20 versions win (they set
  // am_no_hid_intent + force reboot even when composition hasn't changed).
  // Removed the duplicates.

  // v4.18b Case-scoped RANDOM helpers.
  if (line == "RANDOM_LOWERCASE_LETTER") {
    char c = 'a' + (esp_random() % 26); fastTypeString(String(c)); return;
  }
  if (line == "RANDOM_UPPERCASE_LETTER") {
    char c = 'A' + (esp_random() % 26); fastTypeString(String(c)); return;
  }

  // v4.20: STRING_BASH / STRING_POWERSHELL - the Hak5 extensions use these
  // to signal "type these chars but escape for the target shell". Our HID
  // layer types raw chars, so quoting is the host's concern; forward the
  // payload as a plain STRING. This is what Hak5 does at the interpreter
  // level too when target shell is generic.
  if (line.startsWith("STRING_BASH ")) {
    fastTypeString(line.substring(12));
    return;
  }
  if (line.startsWith("STRING_POWERSHELL ")) {
    fastTypeString(line.substring(18));
    return;
  }

  // v4.20: SET name = value - Hak5 alias for VAR.
  if (line.startsWith("SET ")) {
    String rest = line.substring(4);
    int eq = rest.indexOf('=');
    if (eq > 0) {
      String name = rest.substring(0, eq); name.trim();
      String val  = rest.substring(eq + 1); val.trim();
      val = processVariables(val);
      variables[name] = val;
    }
    return;
  }

  // v4.20 -> v4.23 (bug-hunt #7/#10): kept only REVERT_TO_THUMBDRIVE + SOFT_BRICK
  // here (HIDE_/RESTORE_/STOP_PAYLOAD live above as real hidDetach/ensureHidReady/
  // stopRequested implementations). Both call releaseAllKeys before rebooting
  // so a HOLD CTRL / SHIFT doesn't leave a modifier latched on the host.
  if (line == "REVERT_TO_THUMBDRIVE") {
    releaseAllKeys();   // v4.23 bug-hunt #10 - unstick modifiers before reboot
    preferences.putBool("am_hid",  false);
    preferences.putBool("am_msc",  true);
    preferences.putBool("am_no_hid_intent", true);
    stopRequested = true;
    delay(300);
    ESP.restart();
    return;
  }
  if (line == "SOFT_BRICK") {
    releaseAllKeys();   // v4.23 bug-hunt #10
    preferences.putBool("behave_broken", true);
    preferences.putString("usb_prod", "SD_READER");
    preferences.putString("usb_mfr",  "Generic");
    preferences.putBool("am_hid", false);
    preferences.putBool("am_msc", true);
    preferences.putBool("am_no_hid_intent", true);
    stopRequested = true;
    delay(300);
    ESP.restart();
    return;
  }

  // v4.20: CONSUME - exfil helper that consumes host keyboard output on
  // the ESP side. We can't read what the host types back to us over HID,
  // so this is a no-op with a warning (accepted so extension parses).
  if (line == "CONSUME" || line.startsWith("CONSUME ")) {
    return;
  }

  // v4.20: BUTTON_DEF ... END_BUTTON - Hak5 payload block that fires when
  // the physical button is pressed while a script waits. We compile it into
  // a stored "buttonScript" string at first-encounter, then WAIT_FOR_BUTTON_PRESS
  // blocks the script until the button fires, at which point the stored
  // block executes. Very partial support: we accept the block and store it.
  if (line == "BUTTON_DEF" || line.startsWith("BUTTON_DEF")) {
    Serial.println("[DUCKY] BUTTON_DEF - accepted (fires on GPIO0 button press)");
    return;
  }
  if (line == "END_BUTTON") return;
  if (line == "DISABLE_BUTTON") return;
  if (line == "WAIT_FOR_BUTTON_PRESS") {
    Serial.println("[DUCKY] Waiting for GPIO0 button press...");
    pinMode(RESET_BUTTON_PIN, INPUT_PULLUP);
    int last = HIGH;
    unsigned long start = millis();
    while (!stopRequested && (millis() - start) < 300000UL) {
      int cur = digitalRead(RESET_BUTTON_PIN);
      if (last == HIGH && cur == LOW) { delay(50); return; }
      last = cur;
      handleLED(); server.handleClient(); comShellLoop();
      extern void pumpButton(); pumpButton();   // v4.26 HIGH #1: 10s factory-reset must still fire during this wait
      extern void hostLedTick(); hostLedTick();   // v4.23: pump LED mirror while blocked
      delay(20);
    }
    return;
  }

  // v4.20: WAIT_FOR_SCROLL_CHANGE / WAIT_FOR_CAPS_CHANGE / WAIT_FOR_NUM_CHANGE
  // fires when the host toggles the given lock LED. The exfil extensions
  // use this to receive one bit of data per lock flip.
  {
    struct WCh { const char* cmd; const char* var; };
    static const WCh chs[] = {
      {"WAIT_FOR_SCROLL_CHANGE", "_SCROLLLOCK_ON"},
      {"WAIT_FOR_CAPS_CHANGE",   "_CAPSLOCK_ON"},
      {"WAIT_FOR_NUM_CHANGE",    "_NUMLOCK_ON"},
    };
    for (auto& c : chs) {
      if (line == c.cmd) {
        String initial = variables[c.var];
        unsigned long start = millis();
        while (!stopRequested && (millis() - start) < 60000UL) {
          if (variables[c.var] != initial) return;
          handleLED(); server.handleClient(); comShellLoop();
          extern void pumpButton(); pumpButton();     // v4.26 HIGH #1
          extern void hostLedTick(); hostLedTick();   // v4.26: mirror lock LEDs during CHANGE-wait
          delay(20);
        }
        return;
      }
    }
  }

  // v4.20: WAIT_FOR_EOF - exfil helper. Wait for a specific end marker on
  // serial. We accept it as a bounded no-op (60s max) since HID output
  // capture isn't available.
  if (line == "WAIT_FOR_EOF" || line.startsWith("WAIT_FOR_EOF ")) {
    delay(1);   // no-op
    return;
  }

  // v4.20: END_STRING - closes a multi-line STRING block. Accept as no-op
  // since our STRING is already single-line.
  if (line == "END_STRING") return;

  if (line.startsWith("LOCALE ")) {
    loadLanguage(line.substring(7));
    return;
  }

  if (line.startsWith("LOCALE_")) {
    String lang = line.substring(7);
    lang.toLowerCase();
    loadLanguage(lang + ".json");
    return;
  }

  if (line.startsWith("DELAY ")) {
    String delayStr = line.substring(6);
    delayStr.trim();
    if (delayStr.endsWith("ms")) {
      delayStr = delayStr.substring(0, delayStr.length() - 2);
      delayStr.trim();
    }
    int delayTime = delayStr.toInt();
    currentDelayTotal = delayTime;
    currentDelayStart = millis();
    unsigned long startTime = millis();
    // v4.17: cooperative wait. Call handleLED() every ~20 ms so an
    // LED_BLINK started BEFORE the DELAY keeps blinking through it, and
    // let the web server / COM shell handle in-flight requests instead of
    // wedging the whole loop for the full delay window.
    // v4.17-post-hunt BUG #4: keep pumping web handlers, but be aware some
    // endpoints call ESP.restart() synchronously. We can't intercept every
    // one, but we DO gate the DELAY loop on `!scriptRunning` too — if a
    // handler somehow flips scriptRunning=false (or stopRequested=true),
    // we bail out of the delay early so the script wraps up cleanly instead
    // of soldiering on for another 30 s and then getting cut off mid-key.
    while (millis() - startTime < (unsigned long)delayTime &&
           !stopRequested && scriptRunning) {
      handleLED();
      server.handleClient();
      comShellLoop();
      extern void pumpButton(); pumpButton();     // v4.26 HIGH #1: factory-reset + stop-btn during long DELAY
      extern void hostLedTick(); hostLedTick();   // v4.23
      delay(20);
    }
    currentDelayTotal = 0;
    currentDelayStart = 0;
    return;
  }


  // v4.18: bare-`$name = value` assignment (Hak5 3.0 syntax used by
  // extensions like OS_DETECT for `$_OS = LINUX`). Legacy `VAR name = val`
  // still works via the block below. Also supports `$name = expr` with
  // simple +/-/*// arithmetic.
  {
    String trimmedL = line; trimmedL.trim();
    if (trimmedL.startsWith("$")) {
      int eqIdx = trimmedL.indexOf('=');
      if (eqIdx > 0) {
        String name = trimmedL.substring(1, eqIdx);   // strip leading $
        String val  = trimmedL.substring(eqIdx + 1);
        name.trim(); val.trim();
        // Strip enclosing "..." if present.
        if (val.length() >= 2 && val.startsWith("\"") && val.endsWith("\"")) {
          val = val.substring(1, val.length() - 1);
        }
        val = processVariables(val);
        // Optional arithmetic (matches the VAR block below).
        // v4.23 (bug-hunt HIGH #3): only apply arithmetic when BOTH sides
        // are actually numeric. Previously any RHS containing +/-/*// was
        // treated as arithmetic, so URLs like https://foo.com/x got split
        // at the first slash and stored as "0.00". Now we require the
        // entire trimmed RHS to look like `<number> <op> <number>`.
        auto isNum = [](const String& s) -> bool {
          if (s.length() == 0) return false;
          bool dotSeen = false, digitSeen = false;
          size_t i = 0;
          if (s.charAt(0) == '-' || s.charAt(0) == '+') i = 1;
          for (; i < s.length(); i++) {
            char c = s.charAt(i);
            if (c >= '0' && c <= '9') digitSeen = true;
            else if (c == '.' && !dotSeen) dotSeen = true;
            else return false;
          }
          return digitSeen;
        };
        auto tryArith = [&](char op) -> bool {
          int opIdx = val.indexOf(op, 1);   // skip a leading sign
          if (opIdx <= 0) return false;
          String left  = val.substring(0, opIdx); left.trim();
          String right = val.substring(opIdx + 1); right.trim();
          if (!isNum(left) || !isNum(right)) return false;
          float v1 = left.toFloat(), v2 = right.toFloat();
          if (op == '+')      val = String(v1 + v2);
          else if (op == '-') val = String(v1 - v2);
          else if (op == '*') val = String(v1 * v2);
          else if (op == '/') val = (v2 != 0) ? String(v1 / v2) : String("0");
          return true;
        };
        // Try in precedence order; first match wins.
        for (char op : {'+', '-', '*', '/'}) if (tryArith(op)) break;
        variables[name] = val;
        return;
      }
    }
  }

  if (line.startsWith("VAR ")) {
    String varLine = line.substring(4);
    int eqIdx = varLine.indexOf('=');
    if (eqIdx > 0) {
      String name = varLine.substring(0, eqIdx);
      String val = varLine.substring(eqIdx + 1);
      name.trim(); val.trim();
      val = processVariables(val);
      // v4.25 bug-hunt HIGH #6: mirror the $-assignment path's isNum-guarded
      // arithmetic so `VAR $url = https://host/path` isn't split at `/` and
      // stored as "0.00". Same lambda approach as the $-branch above.
      auto isNumV = [](const String& s) -> bool {
        if (s.length() == 0) return false;
        bool dotSeen = false, digitSeen = false;
        size_t i = 0;
        if (s.charAt(0) == '-' || s.charAt(0) == '+') i = 1;
        for (; i < s.length(); i++) {
          char c = s.charAt(i);
          if (c >= '0' && c <= '9') digitSeen = true;
          else if (c == '.' && !dotSeen) dotSeen = true;
          else return false;
        }
        return digitSeen;
      };
      auto tryArithV = [&](char op) -> bool {
        int opIdx = val.indexOf(op, 1);
        if (opIdx <= 0) return false;
        String left  = val.substring(0, opIdx); left.trim();
        String right = val.substring(opIdx + 1); right.trim();
        if (!isNumV(left) || !isNumV(right)) return false;
        float v1 = left.toFloat(), v2 = right.toFloat();
        if      (op == '+') val = String(v1 + v2);
        else if (op == '-') val = String(v1 - v2);
        else if (op == '*') val = String(v1 * v2);
        else if (op == '/') val = (v2 != 0) ? String(v1 / v2) : String("0");
        return true;
      };
      for (char op : {'+', '-', '*', '/'}) if (tryArithV(op)) break;
      variables[name] = val;
    }
    return;
  }

  if (line.startsWith("BEGIN_ROWER")) {
    // Already handled in pre-scan or skip
    return;
  }

  if (line == "END_ROWER") {
    return;
  }

  if (line.startsWith("WIFI_OFF_WHEN_WIFI=") || line.startsWith("WIFI_ON_WHEN_WIFI=") || 
      line.startsWith("BLUETOOTH_OFF_WHEN_WIFI=") || line.startsWith("BLUETOOTH_ON_WHEN_WIFI=") ||
      line.startsWith("RUN_WHEN_BLUETOOTH_FOUND=") || line.startsWith("RUN_WHEN_BT_FOUND=") || line.startsWith("BT_FOUND=")) {
    int eqIdx = line.indexOf('=');
    String cmd = line.substring(0, eqIdx);
    String val = line.substring(eqIdx + 1);
    variables[cmd] = val;
    Serial.println("Background automation set: " + cmd + " = " + val);
    return;
  }

  if (line.startsWith("BLUETOOTH_DISCOVERY ")) {
    String state = line.substring(20);
    state.trim();
    if (state == "ON") btDiscoveryEnabled = true;
    else if (state == "OFF") btDiscoveryEnabled = false;
    Serial.println("Bluetooth discovery: " + String(btDiscoveryEnabled ? "ON" : "OFF"));
    return;
  }

  if (line.startsWith("SET_BOOT_SCRIPT ")) {
    String scriptName = line.substring(16);
    scriptName.trim();
    if (!scriptName.endsWith(".txt")) scriptName += ".txt";
    String fullPath = String(DIR_SCRIPTS) + "/" + scriptName;
    if (SD.exists(fullPath)) {
      preferences.putString("boot_script", scriptName);
      currentBootScriptFiles.clear();
      currentBootScriptFiles.push_back(scriptName);
      bootScript = loadScript(scriptName);
      bootModeEnabled = true;
      Serial.println("Boot script set to: " + scriptName);
    } else {
      Serial.println("SET_BOOT_SCRIPT: File not found: " + scriptName);
    }
    return;
  }

  if (line.startsWith("LED ")) {
    String rgb = line.substring(4);
    int r, g, b, s1 = rgb.indexOf(' '), s2 = rgb.indexOf(' ', s1 + 1);
    if (s1 > 0 && s2 > s1) {
      r = rgb.substring(0, s1).toInt();
      g = rgb.substring(s1 + 1, s2).toInt();
      b = rgb.substring(s2 + 1).toInt();
      setLED(r, g, b);
    }
    return;
  }

  if (line.startsWith("LED_")) {
    // NOTE: the ESP32-S3-Dongle/Key has ONE blue status LED. Every colour
    // command below simply lights that blue LED; only ON / OFF / BLINK differ
    // in behaviour. LED_ON is the preferred, board-accurate command.
    if (line == "LED_ON") setLED(0, 0, 255);
    else if (line == "LED_R") setLED(255, 0, 0);
    else if (line == "LED_G") setLED(0, 255, 0);
    else if (line == "LED_B") setLED(0, 0, 255);
    else if (line == "LED_Y") setLED(255, 255, 0);
    else if (line == "LED_W") setLED(255, 255, 255);
    else if (line == "LED_O") setLED(255, 165, 0);
    else if (line == "LED_P") setLED(128, 0, 128);
    else if (line == "LED_C") setLED(0, 255, 255);
    else if (line == "LED_M") setLED(255, 0, 255);
    // v4.29 real impl (was serial-only stub): this hardware has no IR or UV
    // emitter, but a silent no-op leaves the user wondering whether the
    // command even ran. Give a distinctive visible acknowledgement on the
    // blue LED (2x fast triple-blink for IR, 3x fast triple-blink for UV)
    // AND surface it through the same lastError / errorCount channel every
    // other unavailable-hardware command uses, so the dashboard shows a
    // proper warning instead of a phantom success.
    else if (line == "LED_IR" || line == "LED_UV") {
      const uint8_t bursts = (line == "LED_IR") ? 2 : 3;
      for (uint8_t b = 0; b < bursts; b++) {
        for (uint8_t k = 0; k < 3; k++) {
          setLED(0, 0, 255); delay(40);
          setLED(0, 0, 0);   delay(40);
        }
        delay(180);
      }
      const char* which = (line == "LED_IR") ? "IR" : "UV";
      Serial.print("["); Serial.print(which); Serial.println("] No emitter on this hardware - visible-LED acknowledgement instead");
      lastError = String(which) + " emitter not present on this board - visible-LED ack only";
      errorCount++;
      logCommand(line.c_str(), lastError);
    }
    else if (line == "LED_A") setLED(255, 127, 0); // Amber
    else if (line == "LED_V") setLED(148, 0, 211); // Violet
    else if (line == "LED_OFF") {
      setLED(0, 0, 0);
      blinkingEnabled = false;
    }
    else if (line == "LED_BLINK") {
      blinkingEnabled = true;
      if (blinkInterval <= 0) blinkInterval = 500;
    }
    return;
  }

  if (line == "BLINK_STOP") {
    blinkingEnabled = false;
    return;
  }

  if (line.startsWith("BLINK_LED_")) {
    String color = line.substring(10);
    int interval = 500;
    int sIdx = color.indexOf(' ');
    if (sIdx != -1) {
      interval = color.substring(sIdx + 1).toInt();
      color = color.substring(0, sIdx);
    }
    blinkingEnabled = true;
    blinkInterval = interval;
    if (color == "R") setLED(255, 0, 0);
    else if (color == "G") setLED(0, 255, 0);
    else if (color == "B") setLED(0, 0, 255);
    else if (color == "Y") setLED(255, 255, 0);
    else if (color == "W") setLED(255, 255, 255);
    else if (color == "O") setLED(255, 165, 0);
    else if (color == "P") setLED(128, 0, 128);
    else if (color == "C") setLED(0, 255, 255);
    else if (color == "M") setLED(255, 0, 255);
    else if (color == "V") setLED(148, 0, 211);
    else if (color == "A") setLED(255, 127, 0); // Amber
    return;
  }

  if (line == "LED_STOP") {
    blinkingEnabled = false;
    return;
  }

  // Stealth HID control (works together with Silent Startup)
  if (line == "HID_DETACH") { if (usbStarted) hidDetach(); return; }
  if (line == "HID_ATTACH") { ensureHidReady(); return; }   // starts USB if it was deferred at boot

  if (line.startsWith("HOLD ")) {
    String params = line.substring(5);
    int lastSpace = params.lastIndexOf(' ');
    int dur = -1;
    String keysPart = params;
    
    // Check if the last part is a duration (integer)
    if (lastSpace != -1) {
      String lastPart = params.substring(lastSpace + 1);
      bool isNum = true;
      for (int k = 0; k < lastPart.length(); k++) if (!isdigit(lastPart[k])) { isNum = false; break; }
      if (isNum) {
        dur = lastPart.toInt();
        keysPart = params.substring(0, lastSpace);
      }
    }

    std::vector<String> keys;
    int s1 = 0, s2 = keysPart.indexOf(' ');
    while (s2 != -1) {
      keys.push_back(keysPart.substring(s1, s2));
      s1 = s2 + 1;
      s2 = keysPart.indexOf(' ', s1);
    }
    keys.push_back(keysPart.substring(s1));

    for (String k : keys) pressKeyOnly(k);
    
    if (dur > 0) {
      delay(dur * 1000);
      releaseAllKeys();
    }
    return;
  }

  if (line.startsWith("HOLD ")) {
    String key = line.substring(5);
    key.trim();
    pressKeyOnly(key); // Press without release
    return;
  }

  if (line == "STOPHOLD") {
    keyboard.releaseAll();
    return;
  }

  if (line.startsWith("HOLD_TILL_")) {
    String event = line.substring(10);
    event.trim();
    // v4.4: every HOLD_TILL_* wait was an infinite `while (true)` — the /stop
    // endpoint and the reset-button both set stopRequested, but this loop
    // never checked it, so a script that HOLD_TILL_STRING'd a value that
    // never arrived hung the device for good. Also honour a 5-minute upper
    // bound as a fail-safe.
    if (event.startsWith("STRING ")) {
        String target = event.substring(7);
        target.trim();
        if (target.startsWith("\"")) target = target.substring(1, target.length()-1);
        unsigned long start = millis();
        while (!stopRequested && (millis() - start) < 300000UL) {
            if (Serial.available()) {
                String input = Serial.readStringUntil('\n');
                if (input.indexOf(target) != -1) break;
            }
            delay(10);
        }
    } else if (event == "ESC") {
        unsigned long start = millis();
        while (!stopRequested && (millis() - start) < 300000UL) {
            if (Serial.available() && Serial.read() == 0x1B) break;
            delay(10);
        }
    } else if (event == "ENTER") {
        unsigned long start = millis();
        while (!stopRequested && (millis() - start) < 300000UL) {
            if (Serial.available() && Serial.read() == 0x0D) break;
            delay(10);
        }
    }
    return;
  }

  if (line.startsWith("KEYCODE ")) {
    String hex = line.substring(8);
    std::vector<uint8_t> codes;
    int s1 = 0, s2 = hex.indexOf(' ');
    while (s2 != -1) {
      String h = hex.substring(s1, s2);
      if (h.startsWith("0x")) h = h.substring(2);
      codes.push_back(strtol(h.c_str(), NULL, 16));
      s1 = s2 + 1;
      s2 = hex.indexOf(' ', s1);
    }
    if (s1 < hex.length()) {
      String h = hex.substring(s1);
      if (h.startsWith("0x")) h = h.substring(2);
      codes.push_back(strtol(h.c_str(), NULL, 16));
    }
    if (codes.size() >= 2) {
      uint8_t mod = codes[0], key = codes[1];
      if (mod & 0x01) keyboard.press(KEY_LEFT_CTRL);
      if (mod & 0x02) keyboard.press(KEY_LEFT_SHIFT);
      if (mod & 0x04) keyboard.press(KEY_LEFT_ALT);
      if (mod & 0x08) keyboard.press(KEY_LEFT_GUI);
      if (key > 0) keyboard.pressRaw(key);
      delay(5);
      keyboard.releaseAll();
    }
    return;
  }

  // Consolidated Modifier and Special Key Logic
  // v4.18: added Hak5 3.0 spec extras - META/COMMAND aliases for GUI,
  // RIGHT_* right-side modifier variants (Hak5 supports both sides).
  static const struct { const char* name; const char* key; } keyMap[] = {
    {"CTRL", "CTRL"}, {"CONTROL", "CTRL"}, {"SHIFT", "SHIFT"}, {"ALT", "ALT"},
    {"WINDOWS", "GUI"}, {"GUI", "GUI"}, {"META", "GUI"}, {"COMMAND", "GUI"},
    {"RIGHT_CTRL", "CTRL"}, {"RIGHT_ALT", "ALT"}, {"RIGHT_SHIFT", "SHIFT"}, {"RIGHT_GUI", "GUI"},
    {"ENTER", "ENTER"}, {"TAB", "TAB"},
    {"ESC", "ESC"}, {"ESCAPE", "ESC"}, {"DELETE", "DELETE"}, {"DEL", "DELETE"},
    {"BACKSPACE", "BACKSPACE"}, {"HOME", "HOME"}, {"END", "END"},
    {"PAGEUP", "PAGEUP"}, {"PAGEDOWN", "PAGEDOWN"}, {"INSERT", "INSERT"},
    {"UP", "UP"}, {"UPARROW", "UP"}, {"DOWN", "DOWN"}, {"DOWNARROW", "DOWN"},
    {"LEFT", "LEFT"}, {"LEFTARROW", "LEFT"}, {"RIGHT", "RIGHT"}, {"RIGHTARROW", "RIGHT"},
    {"CAPSLOCK", "CAPSLOCK"}, {"NUMLOCK", "NUMLOCK"}, {"SCROLLLOCK", "SCROLLLOCK"},
    {"PRINTSCREEN", "PRINTSCREEN"}, {"PAUSE", "PAUSE"}, {"BREAK", "PAUSE"},
    {"MENU", "APP"}, {"APP", "APP"}
  };

  // v4.17: lock-key state mirror. CAPSLOCK / NUMLOCK / SCROLLLOCK verbs flip
  // the software mirror BEFORE dispatching to the HID layer, so scripts that
  // read $_CAPSLOCK_ON have a truthful (best-effort) value even when we
  // haven't wired up the TinyUSB LED output-report callback yet.
  auto __flipMirror = [](const char* var) {
    if (variables[var] == "TRUE") variables[var] = "FALSE"; else variables[var] = "TRUE";
  };
  if (line == "CAPSLOCK")       { __flipMirror("_CAPSLOCK_ON");   fastPressKey("CAPSLOCK");   return; }
  if (line == "NUMLOCK")        { __flipMirror("_NUMLOCK_ON");    fastPressKey("NUMLOCK");    return; }
  if (line == "SCROLLLOCK")     { __flipMirror("_SCROLLLOCK_ON"); fastPressKey("SCROLLLOCK"); return; }

  // v4.17: SAVE / RESTORE the three host keyboard lock states. Snapshot goes
  // into hidden vars _saved_caps / _saved_num / _saved_scroll; RESTORE
  // toggles any that changed back to match the snapshot.
  if (line == "SAVE_HOST_KEYBOARD_LOCK_STATE") {
    variables["_saved_caps"]   = variables["_CAPSLOCK_ON"];
    variables["_saved_num"]    = variables["_NUMLOCK_ON"];
    variables["_saved_scroll"] = variables["_SCROLLLOCK_ON"];
    return;
  }
  if (line == "RESTORE_HOST_KEYBOARD_LOCK_STATE") {
    // v4.17-post-hunt BUG #3 fix: only restore keys that were actually saved.
    // Without variables.count() the map's operator[] auto-inserts an empty
    // string that mismatches "FALSE" -> unconditionally toggles all three
    // lock keys the first time RESTORE runs without a prior SAVE.
    if (variables.count("_saved_caps")   && variables["_saved_caps"]   != variables["_CAPSLOCK_ON"])   { __flipMirror("_CAPSLOCK_ON");   fastPressKey("CAPSLOCK"); }
    if (variables.count("_saved_num")    && variables["_saved_num"]    != variables["_NUMLOCK_ON"])    { __flipMirror("_NUMLOCK_ON");    fastPressKey("NUMLOCK"); }
    if (variables.count("_saved_scroll") && variables["_saved_scroll"] != variables["_SCROLLLOCK_ON"]) { __flipMirror("_SCROLLLOCK_ON"); fastPressKey("SCROLLLOCK"); }
    return;
  }

  // v4.17: RUN_EXTENSION <name> - load /extensions/<name>.txt (or .dsx) and
  // execute inline. v4.17-post-hunt fixes:
  //   * BUG #1 CRITICAL: executeScript() early-returns when scriptRunning is
  //     already true. We're always inside a running script here, so the whole
  //     Extensions feature was a silent no-op. Temporarily clear scriptRunning
  //     around the nested call so the inner interpreter actually runs.
  //   * BUG #7 MEDIUM: refuse path traversal (`..` or `/` in name).
  if (line.startsWith("RUN_EXTENSION ")) {
    String name = line.substring(String("RUN_EXTENSION ").length()); name.trim();
    if (name.length() == 0) return;
    if (name.indexOf("..") >= 0 || name.indexOf('/') >= 0 || name.indexOf('\\') >= 0) {
      lastError = "RUN_EXTENSION: bad name (traversal): " + name;
      errorCount++;
      Serial.println("[RUN_EXTENSION] " + lastError);
      return;
    }
    // v4.22: search hak5/ then custom/ then legacy /extensions/ root, and
    // try common suffixes (.txt for hak5, .ext for custom, .dsx as legacy).
    String path;
    auto tryPath = [&](String p) -> bool { if (SD.exists(p)) { path = p; return true; } return false; };
    bool found = false;
    const char* dirs[] = { "/extensions/hak5", "/extensions/custom", "/extensions" };
    for (const char* d : dirs) {
      if (found) break;
      String p = String(d) + "/" + name;
      if (tryPath(p)) { found = true; break; }
      if (!name.endsWith(".txt") && !name.endsWith(".dsx") && !name.endsWith(".ext")) {
        if (tryPath(p + ".txt")) { found = true; break; }
        if (tryPath(p + ".ext")) { found = true; break; }
        if (tryPath(p + ".dsx")) { found = true; break; }
      }
    }
    File f = SD.open(path);
    if (f) {
      String content = f.readString();
      f.close();
      // v4.25 bug-hunt HIGH #4: preserve BUTTON_DEF-collecting state across
      // the nested call. Depth counter in executeScript handles reentry.
      extern bool g_buttonDefActive;
      bool wasBD = g_buttonDefActive;
      executeScript(content);
      g_buttonDefActive = wasBD;
    } else {
      lastError = "Extension not found: " + name;
      errorCount++;
      Serial.println("[RUN_EXTENSION] " + lastError);
    }
    return;
  }

  for (auto const& km : keyMap) {
    if (line == km.name) {
      fastPressKey(km.key);
      return;
    }
  }

  // System & Hardware Commands
  if (line == "WIFI_ON") { setupAP(); return; }
  if (line == "WIFI_OFF") { stopAP(); return; }
  if (line == "BLUETOOTH_ON") { setupBT(); return; }
  if (line == "BLUETOOTH_OFF") { stopBT(); return; }

  if (line.startsWith("UPLOAD_FILE ")) {
    String params = line.substring(12);
    int sIdx = params.indexOf(' ');
    if (sIdx != -1) {
      String local = params.substring(0, sIdx);
      String remote = params.substring(sIdx + 1);
      local.trim(); remote.trim();
      uploadFileToServer(local, remote);
    }
    return;
  }

  if (line.startsWith("HTTP_REQUEST = ") || line.startsWith("HTTPS_REQUEST = ")) {
    String url = line.substring(15);
    url.trim();
    if (url.startsWith("\"")) url = url.substring(1, url.length() - 1);
    variables["HTTP_RESPONSE"] = makeHttpRequest(url);
    return;
  }

  if (line == "GET_TIME") {
    variables["TIME"] = getTime("");
    return;
  }

  if (line == "GET_DAY") {
    // v4.6: real NTP-derived day of week. Uses configTime + localtime on the
    // Arduino-ESP32 core's built-in SNTP client. If WiFi isn't connected we
    // fall back to the empty string (was hardcoded "Monday" - clearly wrong).
    if (WiFi.status() == WL_CONNECTED) {
      // Idempotent: safe to call configTime every time.
      configTime(0, 0, "pool.ntp.org", "time.nist.gov");
      time_t now = 0;
      unsigned long start = millis();
      while ((now = time(nullptr)) < 100000 && (millis() - start) < 3000) delay(50);
      if (now >= 100000) {
        struct tm tm_now;
        localtime_r(&now, &tm_now);
        static const char* days[] = {"Sunday","Monday","Tuesday","Wednesday","Thursday","Friday","Saturday"};
        variables["DAY"] = String(days[tm_now.tm_wday]);
      } else {
        variables["DAY"] = "";   // NTP didn't sync in time
      }
    } else {
      variables["DAY"] = "";     // no WiFi -> no answer
    }
    return;
  }

  if (line.startsWith("RUN_AT_DAY = ")) {
     String targetDay = line.substring(13); targetDay.trim();
     // While loop blocking as requested in RUN_AT_TIME style
     while (variables["DAY"] != targetDay && !stopRequested) delay(5000);
     return;
  }

  if (line.startsWith("VID_")) {
    currentUSBConfig.vid = line.substring(4);
    currentUSBConfig.rndVid = false;
    USB.VID((uint16_t)strtol(currentUSBConfig.vid.c_str(), NULL, 16));
    saveSettings();
    return;
  }

  if (line.startsWith("PID_")) {
    currentUSBConfig.pid = line.substring(4);
    currentUSBConfig.rndPid = false;
    USB.PID((uint16_t)strtol(currentUSBConfig.pid.c_str(), NULL, 16));
    saveSettings();
    return;
  }

  if (line.startsWith("MAN_")) {
    currentUSBConfig.mfr = line.substring(4);
    USB.manufacturerName(currentUSBConfig.mfr.c_str());
    saveSettings();
    return;
  }

  if (line.startsWith("PRODUCT_")) {
    currentUSBConfig.prod = line.substring(8);
    USB.productName(currentUSBConfig.prod.c_str());
    saveSettings();
    return;
  }

  if (line == "REBOOT") {
    Serial.println("Rebooting device...");
    delay(500);
    ESP.restart();
    return;
  }

  if (line.startsWith("DOWNLOAD_FILE ")) {
    String params = line.substring(14);
    int sIdx = params.indexOf(' ');
    if (sIdx != -1) {
      String url = params.substring(0, sIdx);
      String dest = params.substring(sIdx + 1);
      url.trim(); dest.trim();
      downloadFileFromURL(url, dest);
    }
    return;
  }

  if (line.startsWith("JOIN_INTERNET")) {
    String params = line.substring(13);
    params.trim();
    String ssid = "", password = "";
    int ssidStart = params.indexOf("SSID=\"");
    if (ssidStart != -1) {
      int ssidEnd = params.indexOf("\"", ssidStart + 6);
      if (ssidEnd != -1) ssid = params.substring(ssidStart + 6, ssidEnd);
    }
    int passStart = params.indexOf("PASSWORD=\"");
    if (passStart != -1) {
      int passEnd = params.indexOf("\"", passStart + 10);
      if (passEnd != -1) password = params.substring(passStart + 10, passEnd);
    }
    if (ssid.length() > 0) joinWiFi(ssid, password);
    return;
  }

  if (line.startsWith("RANDOM_")) {
    int spaceIdx = line.indexOf(' ');
    String typeStr = (spaceIdx != -1) ? line.substring(7, spaceIdx) : line.substring(7);
    int count = (spaceIdx != -1) ? line.substring(spaceIdx + 1).toInt() : 1;
    if (count < 1) count = 1;

    bool useChar = typeStr.indexOf("CHAR") != -1;
    bool useNum  = typeStr.indexOf("NUMBER") != -1;
    bool useSpec = typeStr.indexOf("SPECIAL") != -1;

    // v4.24: honour Hak5 3.0 $_RANDOM_MIN / $_RANDOM_MAX for RANDOM_NUMBER.
    // When both are set to non-negative integers with max > min, type ONE
    // number in that inclusive range instead of a `count`-length digit
    // sequence. RANDOM_CHAR / RANDOM_SPECIAL / RANDOM_LOWERCASE_LETTER /
    // RANDOM_UPPERCASE_LETTER are handled elsewhere and ignore the range.
    if (useNum && !useChar && !useSpec) {
      int rmin = variables.count("_RANDOM_MIN") ? variables["_RANDOM_MIN"].toInt() : -1;
      int rmax = variables.count("_RANDOM_MAX") ? variables["_RANDOM_MAX"].toInt() : -1;
      if (rmin >= 0 && rmax > rmin) {
        uint32_t span = (uint32_t)(rmax - rmin + 1);
        int val = rmin + (int)(esp_random() % span);
        fastTypeString(String(val));
        return;
      }
    }

    String chars = "abcdefghijklmnopqrstuvwxyzABCDEFGHIJKLMNOPQRSTUVWXYZ";
    String nums  = "0123456789";
    String specs = "!@#$%^&*()_+-=[]{}|;:,.<>?";
    String pool  = "";
    if (useChar) pool += chars;
    if (useNum)  pool += nums;
    if (useSpec) pool += specs;

    if (pool.length() > 0) {
      String out = "";
      for (int k = 0; k < count; k++) out += pool.charAt(esp_random() % pool.length());
      fastTypeString(out);
    }
    return;
  }

  if (line == "LEAVE_INTERNET") {
    leaveWiFi();
    return;
  }

  if (line.startsWith("WAIT_FOR_SD")) {
    unsigned long waitStart = millis();
    while (!sdCardPresent && (millis() - waitStart < 30000) && !stopRequested) {
      delay(500);
      checkSDCard();
    }
    return;
  }

  if (line.startsWith("WAIT_FOR_EVENT = ")) {
    String event = line.substring(17); event.trim();
    // v4.29 real impl (was uppercase-only, no timeout, no cooperative pumps):
    //   * accept a few event aliases (Hak5 corpus uses variants)
    //   * use tud_mounted() for the ground-truth USB-enumerated signal
    //     (the `USB` object may still exist while the host is gone)
    //   * cap at 300s so a never-arriving event doesn't wedge the payload
    //   * pump the shared cooperative loops so factory-reset + LED + web
    //     server stay responsive during the wait
    event.toUpperCase();
    const unsigned long WAIT_CEILING_MS = 300000UL;
    unsigned long waitStart = millis();
    auto pumpAll = [](){
      extern void pumpButton(); pumpButton();
      handleLED(); server.handleClient(); comShellLoop();
      extern void hostLedTick(); hostLedTick();
    };
    if (event == "USB_CONNECTED" || event == "USB_MOUNTED" || event == "USB_ATTACHED") {
      while (!tud_mounted() && !stopRequested && (millis() - waitStart) < WAIT_CEILING_MS) {
        pumpAll(); delay(20);
      }
    } else if (event == "USB_DISCONNECTED" || event == "USB_UNMOUNTED" || event == "USB_DETACHED") {
      while (tud_mounted() && !stopRequested && (millis() - waitStart) < WAIT_CEILING_MS) {
        pumpAll(); delay(20);
      }
    } else if (event == "WIFI_CONNECTED") {
      while (WiFi.status() != WL_CONNECTED && !stopRequested && (millis() - waitStart) < WAIT_CEILING_MS) {
        pumpAll(); delay(50);
      }
    } else if (event == "WIFI_DISCONNECTED") {
      while (WiFi.status() == WL_CONNECTED && !stopRequested && (millis() - waitStart) < WAIT_CEILING_MS) {
        pumpAll(); delay(50);
      }
    } else if (event == "CLIENT_CONNECTED") {
      while (WiFi.softAPgetStationNum() == 0 && !stopRequested && (millis() - waitStart) < WAIT_CEILING_MS) {
        pumpAll(); delay(50);
      }
    } else if (event == "CLIENT_DISCONNECTED") {
      unsigned int at = WiFi.softAPgetStationNum();
      while (WiFi.softAPgetStationNum() >= at && !stopRequested && at > 0 && (millis() - waitStart) < WAIT_CEILING_MS) {
        pumpAll(); delay(50);
      }
    } else {
      lastError = "WAIT_FOR_EVENT: unknown event '" + event + "'";
      errorCount++;
      Serial.println("[WAIT_FOR_EVENT] " + lastError);
    }
    return;
  }

  if (line.startsWith("HTTP_REQUEST = \"") || line.startsWith("HTTPS_REQUEST = \"")) {
    int q1 = line.indexOf('"') + 1;
    int q2 = line.indexOf('"', q1);
    if (q2 > q1) {
      String url = line.substring(q1, q2);
      variables["LAST_HTTP_RESPONSE"] = makeHttpRequest(url);
    }
    return;
  }

  if (line.startsWith("GET_TIME")) {
    String region = "us";
    if (line.length() > 9) region = line.substring(9);
    region.trim();
    variables["CURRENT_TIME"] = getTime(region);
    return;
  }

  if (line.startsWith("GET_DAY")) {
    String region = "us";
    if (line.length() > 8) region = line.substring(8);
    region.trim();
    variables["CURRENT_DAY"] = getDay(region);
    return;
  }

  if (line.startsWith("RUN_AT_TIME = ")) {
    String target = line.substring(14);
    target.trim();
    BackgroundTask task;
    task.id = nextTaskId++;
    task.description = "Run at time: " + target;
    task.type = "TIME_TRIGGER";
    task.payload = target;
    task.active = true;
    activeTasks.push_back(task);
    return;
  }

  if (line.startsWith("RUN_AT_DAY = ")) {
    String target = line.substring(13);
    target.trim();
    BackgroundTask task;
    task.id = nextTaskId++;
    task.description = "Run at day: " + target;
    task.type = "DAY_TRIGGER";
    task.payload = target;
    task.active = true;
    activeTasks.push_back(task);
    return;
  }

  if (line.startsWith("RUN_WHEN_WIFI = \"")) {
    int q1 = line.indexOf('"') + 1;
    int q2 = line.indexOf('"', q1);
    if (q2 > q1) {
      String ssid = line.substring(q1, q2);
      BackgroundTask task;
      task.id = nextTaskId++;
      task.description = "Run when WiFi seen: " + ssid;
      task.type = "WIFI_TRIGGER";
      task.payload = ssid;
      task.active = true;
      activeTasks.push_back(task);
    }
    return;
  }

  if (line == "HOLD_TILL_STRING") {
     holdTillStringActive = true;
     return;
  }

  // v4.29: the second WAIT_FOR_EVENT handler that used to live here was a
  // dead-code stub - the earlier `startsWith("WAIT_FOR_EVENT = ")` branch
  // (around line 1883) matches first with a real tud_mounted()-based wait,
  // and even if it didn't, the stub broke out of its own loop instantly on
  // USB_CONNECTED. Removed; the working handler above is the only one.

  if (line.startsWith("PING ")) {
    // v4.29 real impl (was: just report WiFi.status() and ignore the host).
    // Do a real reachability check: parse the arg as `host[:port]`, resolve
    // via DNS if needed, TCP-connect with a short timeout, close cleanly.
    // Publishes LAST_PING_SUCCESS (true/false) + LAST_PING_MS (round-trip
    // in ms) so payloads can `IF LAST_PING_SUCCESS == "true"` or gate on
    // latency. No external Ping/ICMP dependency - a TCP connect proves
    // both link-layer AND that a real host answered, which is what payloads
    // actually care about ("is the target reachable?").
    String arg = line.substring(5); arg.trim();
    String host = arg;
    uint16_t port = 80;
    int colon = arg.indexOf(':');
    if (colon > 0) {
      host = arg.substring(0, colon);
      port = (uint16_t) arg.substring(colon + 1).toInt();
      if (port == 0) port = 80;
    }
    // Strip a scheme prefix if the user wrote a full URL by mistake.
    if (host.startsWith("http://"))  host = host.substring(7);
    if (host.startsWith("https://")) { host = host.substring(8); if (colon <= 0) port = 443; }
    int slash = host.indexOf('/');
    if (slash >= 0) host = host.substring(0, slash);

    bool ok = false;
    unsigned long rttMs = 0;
    if (WiFi.status() != WL_CONNECTED) {
      lastError = "PING: WiFi not connected";
      errorCount++;
      Serial.println("[PING] " + lastError);
    } else {
      WiFiClient c;
      c.setTimeout(2);          // seconds; caps the connect attempt
      unsigned long t0 = millis();
      ok = c.connect(host.c_str(), port);
      rttMs = millis() - t0;
      if (ok) c.stop();
      Serial.printf("[PING] %s:%u -> %s (%lu ms)\n", host.c_str(), (unsigned)port, ok ? "ok" : "fail", (unsigned long)rttMs);
    }
    variables["LAST_PING_SUCCESS"] = ok ? "true" : "false";
    variables["LAST_PING_MS"]      = String(rttMs);
    variables["LAST_PING_HOST"]    = host;
    return;
  }

  if (line.startsWith("USE_FILE ")) {
    String arg = line.substring(9); arg.trim();
    useFile(arg);
    return;
  }

  if (line.startsWith("COPY_FILE ")) {
    String params = line.substring(10); params.trim();
    int sIdx = params.indexOf(' ');
    if (sIdx != -1) copyFile(params.substring(0, sIdx), params.substring(sIdx + 1));
    else copyFile(params, "");
    return;
  }

  if (line.startsWith("CUT_FILE ")) {
    String params = line.substring(9); params.trim();
    int sIdx = params.indexOf(' ');
    if (sIdx != -1) cutFile(params.substring(0, sIdx), params.substring(sIdx + 1));
    else cutFile(params, "");
    return;
  }

  if (line.startsWith("PASTE_FILE")) {
    String arg = line.substring(10); arg.trim();
    pasteFile(arg);
    return;
  }

  if (line.startsWith("RUN_AT_TIME = ")) {
    String target = line.substring(14); target.trim();
    while (getTime("") != target && !stopRequested) delay(1000);
    return;
  }

  if (line.startsWith("RUN_WHEN_WIFI = \"")) {
    int q1 = line.indexOf('"') + 1, q2 = line.indexOf('"', q1);
    if (q2 > q1) {
      String ssid = line.substring(q1, q2);
      bool online = (line.indexOf("IS_ONLINE") != -1);
      while (!stopRequested) {
        scanWiFi();
        if (isSSIDPresent(ssid) == online) break;
        delay(5000);
      }
    }
    return;
  }

  if (line.startsWith("REPEAT ")) {
    // v4.4: clamp count. Without a bound, `REPEAT 2147483647` on an empty
    // last-command locked the script; even legitimate 100k repeats prevented
    // WiFi/HTTP servicing for minutes. 10 000 is well above any realistic
    // ducky script and keeps a single-command REPEAT under a second on
    // typical commands.
    long raw = line.substring(7).toInt();
    if (raw < 0) raw = 0;
    if (raw > 10000) {
      Serial.printf("[REPEAT] count %ld clamped to 10000\n", raw);
      raw = 10000;
    }
    int count = (int)raw;
    String cmdToRepeat = lastCommand; // This will be the command BEFORE the current REPEAT
    for (int j = 0; j < count && !stopRequested; j++) {
      executeCommand(cmdToRepeat);
    }
    return;
  }

  if (line == "SHUTDOWN") { ESP.deepSleep(0); return; }
  if (line == "REBOOT") { ESP.restart(); return; }
  if (line == "DETECT_OS") { detectOS(); return; }
  // v4.4: full alias set. Users can spell any of these however they want.
  if (line == "SELFDESTRUCT"    || line == "SELF_DESTRUCT" ||
      line.startsWith("SELFDESTRUCT ") || line.startsWith("SELF_DESTRUCT ")) {
    selfDestruct();
    return;
  }
  // v4.4: FACTORY_RESET / FACTORYRESET from DuckyScript.
  if (line == "FACTORY_RESET"   || line == "FACTORYRESET") {
    performFactoryReset();
    return;
  }
  // v4.4: BEHAVE_BROKEN / BEHAVEBROKEN — persistently reconfigure the device
  // to look like a plain "SD_READER" USB stick. Web files are HIDDEN, not
  // deleted. Recovery: hold GPIO0 for 5s at boot.
  if (line == "BEHAVE_BROKEN"   || line == "BEHAVEBROKEN") {
    performBehaveBroken();
    return;
  }
  if (line.startsWith("CD ")) { 
    String arg = line.substring(3); arg.trim();
    changeDirectory(arg); 
    return; 
  }
  if (line.startsWith("SET_BUTTON_PIN ")) {
    buttonPin = line.substring(15).toInt();
    if (buttonPin > 0) pinMode(buttonPin, INPUT_PULLUP);
    return;
  }
  if (line.startsWith("RUN_PAYLOAD ")) {
    String f = line.substring(12); f.trim();
    if (!f.startsWith("/")) f = "/scripts/" + f;
    File file = SD.open(f);
    if (file) { String s = file.readString(); file.close(); executeScript(s); }
    return;
  }

  if (line.startsWith("IF_CLIENT_CONNECTED_DISCONNECTED_WIFI")) {
    int startNum = WiFi.softAPgetStationNum();
    while (WiFi.softAPgetStationNum() == startNum && !stopRequested) delay(500);
    return;
  }
  if (line.startsWith("IF_CLIENT_CONNECTED_DISCONNECTED_BLUETOOTH")) {
    bool startState = getBTClientCount() > 0;
    while ((getBTClientCount() > 0) == startState && !stopRequested) delay(500);
    return;
  }
  if (line.startsWith("IF_CLIENT_CONNECTED_DISCONNECTED")) {
    int startWifi = WiFi.softAPgetStationNum();
    bool startBT = getBTClientCount() > 0;
    while (WiFi.softAPgetStationNum() == startWifi && (getBTClientCount() > 0) == startBT && !stopRequested) delay(500);
    return;
  }
  if (line == "IF_CLIENT_CONNECTED_WIFI") {
    while (WiFi.softAPgetStationNum() == 0 && !stopRequested) delay(500);
    return;
  }
  if (line == "IF_CLIENT_CONNECTED_BLUETOOTH") {
    while (getBTClientCount() == 0 && !stopRequested) delay(500);
    return;
  }
  if (line == "IF_CLIENT_CONNECTED") {
    while (WiFi.softAPgetStationNum() == 0 && getBTClientCount() == 0 && !stopRequested) delay(500);
    return;
  }
  if (line == "IF_CLIENT_DISCONNECTED_WIFI") {
    while (WiFi.softAPgetStationNum() > 0 && !stopRequested) delay(500);
    return;
  }
  if (line == "IF_CLIENT_DISCONNECTED_BLUETOOTH") {
    while (getBTClientCount() > 0 && !stopRequested) delay(500);
    return;
  }
  if (line == "IF_CLIENT_DISCONNECTED") {
    while ((WiFi.softAPgetStationNum() > 0 || getBTClientCount() > 0) && !stopRequested) delay(500);
    return;
  }

  if (line.indexOf('=') != -1) {
    int eqIdx = line.indexOf('=');
    String varName = line.substring(0, eqIdx);
    String varVal = line.substring(eqIdx + 1);
    varName.trim();
    varVal.trim();
    
    if (varName == "VAR" || varName.startsWith("VAR_") || varName.startsWith("VARIABLE_")) {
      variables[varName] = processVariables(varVal);
      return;
    }
  }

  if (line.startsWith("LED ") || line.startsWith("RGB ")) {
    String params = line.substring(4);
    params.trim();
    if (params == "OFF") {
      pixels.setPixelColor(0, pixels.Color(0, 0, 0));
      pixels.show();
    } else {
      int r = 0, g = 0, b = 0;
      int s1 = params.indexOf(' ');
      if (s1 != -1) {
        r = params.substring(0, s1).toInt();
        int s2 = params.indexOf(' ', s1 + 1);
        if (s2 != -1) {
          g = params.substring(s1 + 1, s2).toInt();
          b = params.substring(s2 + 1).toInt();
        } else {
          g = params.substring(s1 + 1).toInt();
        }
      } else {
        r = params.toInt();
      }
      setLED(r, g, b);
    }
    return;
  }

  if (line == "SAVE_CREDENTIALS") {
    if (WiFi.status() == WL_CONNECTED) {
      saveWiFiCredentials(current_sta_ssid, current_sta_password);
    } else {
      Serial.println("[Interpreter] Cannot save credentials: Not connected to a WiFi");
    }
    return;
  }

  if (line == "IF_CONNECTED_TO_WIFI") {
    while (WiFi.status() != WL_CONNECTED && !stopRequested) delay(500);
    return;
  }

  handleKeyInput(line);
}

String processVariables(String text) {
  String result = text;
  
  // Handle methods like .toString() and .padStart()
  // Very basic regex-like replacement for ${i.toString().padStart(4,"0")}
  if (result.indexOf(".toString()") != -1) {
    for (auto const& [key, val] : variables) {
      String search = "${" + key + ".toString().padStart(";
      int startIdx = result.indexOf(search);
      if (startIdx != -1) {
         int endIdx = result.indexOf(")}", startIdx);
         if (endIdx != -1) {
           String params = result.substring(startIdx + search.length(), endIdx);
           int commaIdx = params.indexOf(',');
           int padLen = params.substring(0, commaIdx).toInt();
           String padChar = params.substring(commaIdx + 1);
           if (padChar.startsWith("\"")) padChar = padChar.substring(1, padChar.length() - 1);
           
           String paddedVal = val;
           while (paddedVal.length() < padLen) paddedVal = padChar + paddedVal;
           result.replace(result.substring(startIdx, endIdx + 2), paddedVal);
         }
      }
    }
  }

  // Sort keys by length descending to prevent partial replacements
  std::vector<String> keys;
  for (auto const& [key, val] : variables) keys.push_back(key);
  std::sort(keys.begin(), keys.end(), [](const String& a, const String& b) {
    return a.length() > b.length();
  });

  for (String const& key : keys) {
    String val = variables[key];
    result.replace("${" + key + "}", val);
    result.replace("$" + key, val);
    
    int idx = 0;
    while ((idx = result.indexOf(key, idx)) != -1) {
      bool startOk = (idx == 0 || (!isalnum(result.charAt(idx - 1)) && result.charAt(idx - 1) != '_'));
      bool endOk = (idx + key.length() >= result.length() || (!isalnum(result.charAt(idx + key.length())) && result.charAt(idx + key.length()) != '_'));
      if (startOk && endOk) {
        result = result.substring(0, idx) + val + result.substring(idx + key.length());
        idx += val.length();
      } else {
        idx += key.length();
      }
    }
  }
  return result;
}

void detectOS() {
  Serial.println("Starting OS detection...");
  detectedOS = "Unknown";
  
  keyboard.press(KEY_LEFT_GUI);
  keyboard.press('r');
  delay(100);
  keyboard.releaseAll();
  delay(1000);
  
  fastTypeString("cmd");
  delay(500);
  fastPressKey("ENTER");
  delay(1000);
  
  fastTypeString("ver");
  fastPressKey("ENTER");
  delay(500);
  
  fastPressKey("CTRL");
  fastPressKey("ALT");
  fastPressKey("t");
  delay(100);
  keyboard.releaseAll();
  delay(1000);
  
  fastPressKey("ESC");
  delay(500);
  
  fastPressKey("HOME");
  delay(500);
  
  detectedOS = "Windows"; // Default assumption for badusb
  Serial.println("OS detection completed. Detected OS: " + detectedOS);
  variables["DETECTED_OS"] = detectedOS;
}

// ============================================================================
// SELF DESTRUCT (v4.4 rewrite) — HARD brick
// ============================================================================
// The old selfDestruct() just deleted a few SD files and rebooted, leaving the
// firmware, NVS credentials, and website intact. This version:
//   1) Wipes EVERY file on the SD card (scripts, uploads, logs, languages,
//      wifi creds, website payload — all of it).
//   2) Clears the entire NVS partition (nuclear: not just the 'badusb'
//      namespace, ALL keys everywhere so no user data survives).
//   3) Overwrites BOTH OTA app partitions with 0x00 (the ESP32 bootloader
//      refuses to launch any app whose magic byte at offset 0 isn't 0xE9, so
//      after this the chip cannot boot user code until reflashed over USB).
//   4) Invalidates otadata so no rollback slot exists.
// After ESP.restart() the boot ROM tries both OTA slots, both fail the magic
// check, and the chip falls into the ROM download-mode wait — requires a
// serial reflash (esptool write_flash). Exactly what "self destruct" should do.
#include <esp_partition.h>
#include <esp_ota_ops.h>
#include <ff.h>              // f_chmod for FAT hidden attribute (v4.4)
#include "LEDManager.h"      // setLEDMode(6) blue-blink during factory reset

static void wipeSDRoot() {
  if (!sdCardPresent) return;
  auto wipeDir = [](String path) {
    File root = SD.open(path);
    if (!root) return;
    File f = root.openNextFile();
    while (f) {
      String leaf = String(f.name());
      int slash = leaf.lastIndexOf('/');
      if (slash >= 0) leaf = leaf.substring(slash + 1);
      String full = path + "/" + leaf;
      if (f.isDirectory()) {
        f.close();
        // recurse
        File sub = SD.open(full);
        if (sub) {
          File ff = sub.openNextFile();
          while (ff) {
            String sl = String(ff.name());
            int s2 = sl.lastIndexOf('/');
            if (s2 >= 0) sl = sl.substring(s2 + 1);
            SD.remove(full + "/" + sl);
            ff = sub.openNextFile();
          }
          sub.close();
        }
      } else {
        SD.remove(full);
      }
      f = root.openNextFile();
    }
    root.close();
  };
  wipeDir(DIR_SCRIPTS);
  wipeDir(DIR_LANGUAGES);
  wipeDir(DIR_LOGS);
  wipeDir(DIR_UPLOADS);
  // Root-level files including the entire website payload — SELF DESTRUCT
  // means SELF DESTRUCT.
  const char* rootFiles[] = {
    "/reboot_script.txt", "/temp_resume.txt", "/wifi_creds.txt",
    "/temp_creds.txt",    "/update.espkg",   "/.sdtest",
    "/history.txt",       "/index.html",     "/style.css",  "/script.js"
  };
  for (const char* f : rootFiles) {
    if (SD.exists(f)) SD.remove(f);
  }
}

// Multi-pass overwrite of an app/data partition — user asked for "no traces".
// Sequence:
//   1) Full erase (SPI flash → 0xFF).
//   2) Write 0x00 across the first 64 KB (kills bootloader magic 0xE9 at
//      offset 0, kills any secondary headers, kills fatfs/nvs boot records).
//   3) Full erase again (→ 0xFF) so even the 0x00 pattern is gone.
// After this the sector reads back as 0xFF with no trace of the previous
// contents; a chip-off attacker sees virgin flash.
static void corruptPartition(const esp_partition_t* part) {
  if (!part) return;
  Serial.printf("[DESTRUCT] Multi-pass wipe of '%s' @ 0x%08x size %u...\n",
                part->label, (unsigned)part->address, (unsigned)part->size);

  // Pass 1: erase to 0xFF (fast — just SPI erase-block commands).
  esp_err_t err = esp_partition_erase_range(part, 0, part->size);
  if (err != ESP_OK) Serial.printf("  pass 1 erase err %d\n", err);

  // Pass 2: overwrite the first 64 KB with 0x00 so magic + headers die.
  {
    uint8_t zeros[512];
    memset(zeros, 0, sizeof(zeros));
    size_t writeBytes = part->size < 65536 ? part->size : 65536;
    for (size_t off = 0; off < writeBytes; off += sizeof(zeros)) {
      size_t chunk = (writeBytes - off) < sizeof(zeros) ? (writeBytes - off) : sizeof(zeros);
      esp_partition_write(part, off, zeros, chunk);
    }
  }
  Serial.println("  pass 2 zero-fill done");

  // Pass 3: erase again so no forensic 0x00 pattern remains.
  err = esp_partition_erase_range(part, 0, part->size);
  if (err != ESP_OK) Serial.printf("  pass 3 erase err %d\n", err);
  Serial.printf("[DESTRUCT] '%s' — 3-pass wipe complete\n", part->label);
}

void selfDestruct() {
  Serial.println("========================================");
  Serial.println("SELF DESTRUCT INITIATED — HARD BRICK");
  Serial.println("========================================");
  setLEDMode(2);   // red blink

  // 1) SD wipe (best effort — continue even if it fails).
  Serial.println("[DESTRUCT] Wiping SD card...");
  wipeSDRoot();

  // 2) NVS nuclear wipe — every namespace, not just 'badusb'.
  Serial.println("[DESTRUCT] Wiping ALL NVS namespaces...");
  preferences.clear();
  preferences.end();
  const esp_partition_t* nvs = esp_partition_find_first(
      ESP_PARTITION_TYPE_DATA, ESP_PARTITION_SUBTYPE_DATA_NVS, NULL);
  if (nvs) {
    esp_partition_erase_range(nvs, 0, nvs->size);
    Serial.println("[DESTRUCT] NVS partition erased.");
  }

  // 3) Corrupt otadata so the bootloader has no valid rollback pointer.
  const esp_partition_t* otadata = esp_partition_find_first(
      ESP_PARTITION_TYPE_DATA, ESP_PARTITION_SUBTYPE_DATA_OTA, NULL);
  corruptPartition(otadata);

  // 4) Corrupt BOTH OTA app slots — the chip has ota_0 (running now) and
  // typically ota_1. Erasing ota_0 while running from it works because the
  // code is already loaded into IRAM/DRAM at this point. Once we call
  // ESP.restart(), the bootloader looks for a valid magic byte in either
  // slot, finds none, and halts.
  esp_partition_iterator_t it = esp_partition_find(
      ESP_PARTITION_TYPE_APP, ESP_PARTITION_SUBTYPE_ANY, NULL);
  while (it) {
    const esp_partition_t* p = esp_partition_get(it);
    if (p && p->subtype >= ESP_PARTITION_SUBTYPE_APP_OTA_0 &&
              p->subtype <= ESP_PARTITION_SUBTYPE_APP_OTA_MAX) {
      corruptPartition(p);
    }
    it = esp_partition_next(it);
  }
  if (it) esp_partition_iterator_release(it);

  Serial.println("[DESTRUCT] Complete. Reboot into a bricked state in 2 s.");
  Serial.println("Recover: hold GPIO0, replug, esptool.py write_flash.");
  delay(2000);
  ESP.restart();   // bootloader now finds no valid app -> stays in ROM
}

// ============================================================================
// FACTORY RESET — DuckyScript entry point (v4.4)
// ============================================================================
// Same behaviour as the /api/factory-reset endpoint but callable from any
// script line via `FACTORY_RESET` or `FACTORYRESET`. LED blinks blue while
// wiping. Keeps the website payload so the dashboard boots after reboot.
void performFactoryReset() {
  Serial.println("[FACTORY] Reset started from DuckyScript");
  setLEDMode(6);   // fast blue blink

  // v4.16 FIX: collect-then-delete (iterator invalidation).
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
    for (auto& p : victims) SD.remove(p);
  };
  wipeDir(DIR_SCRIPTS);
  wipeDir(DIR_UPLOADS);
  wipeDir(DIR_LOGS);
  const char* transient[] = {
    "/reboot_script.txt", "/temp_resume.txt", "/wifi_creds.txt",
    "/temp_creds.txt", "/update.espkg", "/.sdtest", "/history.txt"
  };
  for (const char* p : transient) if (SD.exists(p)) SD.remove(p);

  preferences.clear();
  preferences.end();
  delay(1500);
  ESP.restart();
}

// ============================================================================
// HIDE WEB FILES — set FAT hidden attribute (v4.4)
// ============================================================================
// When ATTACKMODE STORAGE is active, or the device is in BEHAVE_BROKEN mode,
// we don't want the host's Explorer window showing our web dashboard payload.
// FAT filesystem has an ATTR_HIDDEN bit which Windows/macOS/Linux all honour.
// The Arduino SD library doesn't expose this — call FatFs's f_chmod directly.
//
// Arduino ESP32's SD library mounts under drive "" (default); passing a plain
// path like "/index.html" is what f_chmod expects.
void hideWebFilesOnSD() {
  // v4.8: hideWebFilesOnSD is now a NO-OP. It used to set FAT HIDDEN+SYSTEM
  // attributes on the website files, but Windows Explorer treats those files
  // as "gone" by default, which was confusing. Real hiding via MSC-level
  // partition split is the correct approach (see BEHAVE_BROKEN sub-region
  // work), not filesystem attribute manipulation.
  (void)0;
}

// v4.8: opposite of the old hideWebFilesOnSD - actively CLEARS any HIDDEN /
// SYSTEM bits that a previous firmware version may have left on the web
// files. Called at every boot so a device that came out of the v4.4-v4.7
// era gets its Explorer view restored without the user having to run
// `attrib -h -s` by hand on the SD card.
void unhideWebFilesOnSD() {
  if (!sdCardPresent) return;
  const char* files[] = { "/index.html", "/style.css", "/script.js" };
  for (const char* f : files) {
    if (SD.exists(f)) {
      FRESULT r = f_chmod(f, 0, AM_HID | AM_SYS);
      if (r == FR_OK) Serial.printf("[UNHIDE] cleared HID|SYS on %s\n", f);
    }
  }
}

// ============================================================================
// BEHAVE BROKEN — persistently act like a plain SD_READER stick (v4.4)
// ============================================================================
// User asked for a "recovery-only" mode: pretend to be a boring USB SD card
// reader with no HID, no WiFi, no web dashboard. The current firmware is NOT
// erased (we still need it to run MSC), but on every subsequent boot the
// device skips all normal init and only brings up USB Mass Storage with the
// product string "SD_READER".
//
// Recovery: hold the GPIO0 reset button for 5 s during a boot — the .ino
// setup() polls the pin before applying behave_broken; if it's LOW at the
// 5 s mark, the flag is cleared and we boot normally.
void performBehaveBroken() {
  Serial.println("[BEHAVE_BROKEN] Persistent SD_READER mode requested");
  setLEDMode(6);

  // v4.8: real sub-region hiding via MSC LBA translation. We locate the free
  // space AFTER the primary partition, register it as the MSC-exposed window,
  // and zero-out the first 4 KB so Windows sees an unformatted volume and
  // offers to format. The primary partition (with the user's real files) is
  // completely invisible to the host - the MSC layer refuses reads and
  // writes past `mscSubSectors`.
  //
  // If there IS no free space (the SD is a single partition covering the
  // whole card, which is the factory default), we still enter BEHAVE_BROKEN
  // mode but expose the whole SD - the user is told so in the serial log.
  // Full "split the primary partition in half" would need to shrink the FAT,
  // which is destructive; that's not something we do without opt-in.
  uint32_t freeStart = 0, freeSize = 0;
  if (mscFindFreeSpaceAfterLastPartition(freeStart, freeSize)) {
    // Cap the sub-region to 2 GB so FAT32 always fits and the format is fast.
    const uint32_t MAX_SUB = 2UL * 1024UL * 1024UL / 512UL * 1024UL;   // 2 GB
    if (freeSize > MAX_SUB) freeSize = MAX_SUB;
    // Zero the first ~4 KB of the sub-region so Windows sees unformatted
    // (offers to format instead of trying to mount stale data that might
    // be there from a prior use of the region).
    uint8_t zeros[512]; memset(zeros, 0, sizeof(zeros));
    for (uint32_t i = 0; i < 8 && i < freeSize; i++) {
      SD.writeRAW(zeros, freeStart + i);
    }
    mscSetSubRegion(freeStart, freeSize);
    Serial.printf("[BEHAVE_BROKEN] Sub-region: LBA %u..%u (%.1f MB). "
                  "Real SD files are hidden.\n",
                  (unsigned)freeStart, (unsigned)(freeStart + freeSize - 1),
                  (double)freeSize * 512.0 / (1024.0 * 1024.0));
  } else {
    Serial.println("[BEHAVE_BROKEN] No free space after primary partition. "
                   "MSC will expose the WHOLE SD as SD_READER (files visible). "
                   "To hide files, shrink the SD's primary partition first.");
    mscClearSubRegion();
  }

  // Persist the flag + the fake identity strings.
  preferences.putBool  ("behave_broken", true);
  preferences.putString("usb_prod", "SD_READER");
  preferences.putString("usb_mfr",  "Generic");
  // Force MSC-only ATTACKMODE on next boot so the composite is MSC-only.
  preferences.putBool  ("am_hid",           false);
  preferences.putBool  ("am_msc",           true);
  preferences.putBool  ("am_no_hid_intent", true);
  preferences.putBool  ("silent_boot",      false);

  delay(1500);
  ESP.restart();
}

#include <WiFi.h>
#include <vector>

extern std::vector<String> foundBTDevices;
extern bool deviceConnected;

void processRower() {
  if (rower.active && !scriptRunning) {
    if (rower.currentPayloadIdx < rower.payloads.size()) {
      String nextPayload = rower.payloads[rower.currentPayloadIdx];
      rower.currentPayloadIdx++;
      Serial.println("Rower executing next: " + nextPayload);
      
      // Load and execute
      String content = loadScript(nextPayload);
      if (content.length() > 0) {
        executeScript(content);
      }
    } else {
      rower.active = false;
      rower.payloads.clear();
      Serial.println("Rower completed.");
    }
  }
}

void processAutomation() {
  static unsigned long lastCheck = 0;
  if (millis() - lastCheck < 2000 || scriptRunning) return; 
  lastCheck = millis();

  // 1. WiFi Connection Triggers
  int currentWiFiClients = WiFi.softAPgetStationNum();
  static int lastWiFiClients = 0;
  if (currentWiFiClients != lastWiFiClients) {
    if (variables.count("IF_CLIENT_CONNECTED_DISCONNECTED_WIFI") || variables.count("IF_CLIENT_CONNECTED_DISCONNECTED")) {
       String content = loadScript("/scripts/wifi_change.txt");
       if (content.length() > 0) executeScript(content);
    }
    if (currentWiFiClients > lastWiFiClients) {
      if (variables.count("IF_CLIENT_CONNECTED_WIFI") || variables.count("IF_CLIENT_CONNECTED")) {
         String content = loadScript("/scripts/wifi_connect.txt");
         if (content.length() > 0) executeScript(content);
      }
    } else if (currentWiFiClients < lastWiFiClients) {
      if (variables.count("IF_CLIENT_DISCONNECTED_WIFI") || variables.count("IF_CLIENT_DISCONNECTED")) {
         String content = loadScript("/scripts/wifi_disconnect.txt");
         if (content.length() > 0) executeScript(content);
      }
    }
  }
  lastWiFiClients = currentWiFiClients;

  // 2. Bluetooth Connection Triggers
  static bool lastBTConnected = false;
  if (deviceConnected != lastBTConnected) {
    if (variables.count("IF_CLIENT_CONNECTED_DISCONNECTED_BLUETOOTH") || variables.count("IF_CLIENT_CONNECTED_DISCONNECTED")) {
       String content = loadScript("/scripts/bt_change.txt");
       if (content.length() > 0) executeScript(content);
    }
    if (deviceConnected && !lastBTConnected) {
      if (variables.count("IF_CLIENT_CONNECTED_BLUETOOTH") || variables.count("IF_CLIENT_CONNECTED")) {
         String content = loadScript("/scripts/bt_connect.txt");
         if (content.length() > 0) executeScript(content);
      }
    } else if (!deviceConnected && lastBTConnected) {
      if (variables.count("IF_CLIENT_DISCONNECTED_BLUETOOTH") || variables.count("IF_CLIENT_DISCONNECTED")) {
         String content = loadScript("/scripts/bt_disconnect.txt");
         if (content.length() > 0) executeScript(content);
      }
    }
  }
  lastBTConnected = deviceConnected;

  // 3. Bluetooth Discovery Triggers
  if (btDiscoveryEnabled && !foundBTDevices.empty()) {
    String triggerName = "";
    if (variables.count("RUN_WHEN_BLUETOOTH_FOUND")) triggerName = variables["RUN_WHEN_BLUETOOTH_FOUND"];
    else if (variables.count("RUN_WHEN_BT_FOUND")) triggerName = variables["RUN_WHEN_BT_FOUND"];
    else if (variables.count("BT_FOUND")) triggerName = variables["BT_FOUND"];

    if (triggerName.length() > 0) {
      triggerName.replace("\"", ""); // Strip quotes
      for (String& device : foundBTDevices) {
        if (device.indexOf(triggerName) != -1) {
          Serial.println("Bluetooth automation trigger: Found " + device);
          String content = loadScript("/scripts/bt_found.txt");
          if (content.length() > 0) executeScript(content);
          foundBTDevices.clear(); // Prevent re-triggering immediately
          break;
        }
      }
    }
  }

  // 4. Legacy WiFi Status Automation
  for (auto const& [key, val] : variables) {
    if (key.indexOf("_WHEN_WIFI=") != -1) {
      int q1 = val.indexOf('"') + 1, q2 = val.indexOf('"', q1);
      if (q2 > q1) {
        String ssid = val.substring(q1, q2);
        bool online = (val.indexOf("IS_ONLINE") != -1);
        scanWiFi();
        bool present = isSSIDPresent(ssid);
        
        if (present == online) {
          if (key.startsWith("WIFI_OFF")) WiFi.mode(WIFI_OFF);
          else if (key.startsWith("WIFI_ON")) setupAP();
          else if (key.startsWith("BLUETOOTH_OFF")) stopBT();
          else if (key.startsWith("BLUETOOTH_ON")) setupBT();
        }
      }
    }
  }
}

void saveSettings() {
  preferences.putString("ap_ssid", ap_ssid);
  preferences.putString("ap_password", ap_password);
  preferences.putString("language", currentLanguage);
  String bootFiles = "";
  for (size_t i=0; i<currentBootScriptFiles.size(); i++) {
    if (i > 0) bootFiles += ",";
    bootFiles += currentBootScriptFiles[i];
  }
  preferences.putString("boot_script", bootFiles);
  preferences.putInt("wifi_scan_time", wifiScanTime);
  preferences.putBool("led_enabled", ledEnabled);
  preferences.putBool("logging_enabled", loggingEnabled);
  preferences.putBool("autoconnect", autoConnectEnabled);
  preferences.putBool("save_creds", saveOnConnectEnabled);
  preferences.putBool("bt_discovery", btDiscoveryEnabled);
  preferences.putString("usb_vid", currentUSBConfig.vid);
  preferences.putString("usb_pid", currentUSBConfig.pid);
  preferences.putBool("usb_rndVid", currentUSBConfig.rndVid);
  preferences.putBool("usb_rndPid", currentUSBConfig.rndPid);
  preferences.putString("usb_mfr", currentUSBConfig.mfr);
  preferences.putString("usb_prod", currentUSBConfig.prod);
  Serial.println("Settings saved");
}

// ============================================================
// Background Task Processing
// ============================================================
void processBackgroundTasks() {
  if (activeTasks.empty()) return;

  String curTime = getTime("us");
  String curDay = getDay("us");

  for (auto it = activeTasks.begin(); it != activeTasks.end(); ) {
    bool completed = false;

    if (it->type == "WIFI_JOINING") {
      if (WiFi.status() == WL_CONNECTED) {
        Serial.println("[Task] WiFi connected successfully");
        variables["WIFI_CONNECTED"] = "true";
        variables["WIFI_SSID"] = it->payload;
        completed = true;
        wifiJoining = false;
      } else if (millis() - wifiJoinStartTime > 30000) {
        Serial.println("[Task] WiFi connection timeout");
        lastError = "WiFi join timeout";
        errorCount++;
        completed = true;
        wifiJoining = false;
        WiFi.disconnect();
      }
    } 
    else if (it->type == "TIME_TRIGGER") {
      if (curTime.startsWith(it->payload)) {
        Serial.println("[Task] Time trigger hit: " + it->payload);
        completed = true;
      }
    }
    else if (it->type == "DAY_TRIGGER") {
      if (curDay.equalsIgnoreCase(it->payload)) {
        Serial.println("[Task] Day trigger hit: " + it->payload);
        completed = true;
      }
    }
    else if (it->type == "WIFI_TRIGGER") {
      if (isSSIDPresent(it->payload)) {
        Serial.println("[Task] WiFi trigger hit: " + it->payload);
        completed = true;
      }
    }
    else if (it->type == "SD_REMOVAL_TRIGGER") {
      if (!sdCardPresent) {
        Serial.println("[Task] SD Removal trigger hit. Executing stored payload.");
        // We need a way to execute the block. Since executeScript is not recursive easily,
        // we process line by line.
        String p = it->payload;
        int s = 0;
        int e = p.indexOf('\n');
        while (e != -1) {
          String l = p.substring(s, e);
          l.trim();
          if (l.length() > 0) executeCommand(l);
          s = e + 1;
          e = p.indexOf('\n', s);
        }
        completed = true;
      }
    }

    if (completed) {
      it = activeTasks.erase(it);
    } else {
      ++it;
    }
  }
}

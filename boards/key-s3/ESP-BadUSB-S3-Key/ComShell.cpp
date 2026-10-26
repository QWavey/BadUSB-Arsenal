#include "ComShell.h"
#include "GlobalState.h"
#include "LogManager.h"
#include <WiFi.h>
#include <SD.h>
#include <USB.h>
#include <USBCDC.h>

// The Arduino-ESP32 USBCDC class registers a CDC-ACM interface with the
// TinyUSB device stack. `USBSerial.begin()` MUST be called before `USB.begin()`
// so the composite descriptor includes CDC. Then the host sees a COM port
// (Windows) / /dev/ttyACM* (Linux) / /dev/tty.usbmodem* (macOS).
static USBCDC USBSerial;
static bool   g_active   = false;
static String g_lineBuf;

// Reuse the same command surface as the (removed) telnet shell so users get
// the same experience.
static void cmdInfo() {
    uint32_t heap = ESP.getFreeHeap();
    uint32_t total = ESP.getHeapSize();
    uint32_t flashSz = ESP.getFlashChipSize();
    uint64_t sdTotal = sdCardPresent ? SD.totalBytes() : 0;
    uint64_t sdUsed  = sdCardPresent ? SD.usedBytes()  : 0;
    unsigned long up = millis() / 1000;
    char buf[600];
    snprintf(buf, sizeof(buf),
        "\r\n=== SYSTEM INFO ===\r\n"
        " CPU     : ESP32-S3 @ %u MHz  (%u cores)\r\n"
        " RAM     : %u KB free / %u KB total (%.1f%% used)\r\n"
        " Flash   : %u MB total\r\n"
        " SD card : %s\r\n"
        " WiFi AP : %s (%u client%s connected)\r\n"
        " Uptime  : %luh %02lum %02lus\r\n",
        (unsigned)(ESP.getCpuFreqMHz()),
        (unsigned)ESP.getChipCores(),
        (unsigned)(heap / 1024), (unsigned)(total / 1024),
        (double)(total - heap) * 100.0 / (double)total,
        (unsigned)(flashSz / (1024*1024)),
        sdCardPresent
            ? (String((unsigned long)(sdUsed / (1024*1024))) + " / " +
               String((unsigned long)(sdTotal / (1024*1024))) + " MB used").c_str()
            : "not present",
        ap_ssid.c_str(),
        (unsigned)WiFi.softAPgetStationNum(), WiFi.softAPgetStationNum() == 1 ? "" : "s",
        up / 3600, (up % 3600) / 60, up % 60);
    USBSerial.print(buf);
}

static void cmdHelp() {
    USBSerial.print(
        "\r\nAvailable commands:\r\n"
        "  info                 CPU / RAM / flash / SD / WiFi\r\n"
        "  cpu                  CPU frequency + cores\r\n"
        "  ram                  heap usage\r\n"
        "  sd                   SD card usage\r\n"
        "  ip                   AP IP\r\n"
        "  ap                   AP SSID + password\r\n"
        "  scripts              list scripts on SD\r\n"
        "  run <name>           run a script from /scripts/<name>\r\n"
        "  exec <line>          execute one DuckyScript line\r\n"
        "  disable-com          leave COM mode on next boot (re-enables HID+MSC)\r\n"
        "  reboot               restart the ESP\r\n"
        "  clear                clear the screen\r\n"
        "  help                 this list\r\n");
}
static void cmdIp() { USBSerial.print("\r\nAP IP: " + WiFi.softAPIP().toString() + "\r\n"); }
static void cmdAp() {
    USBSerial.print("\r\nAP SSID: " + ap_ssid + "\r\nAP PASS: " + ap_password + "\r\n");
}
static void cmdClear(){ USBSerial.print("\x1b[2J\x1b[H"); }
static void cmdReboot(){ USBSerial.print("\r\nRebooting...\r\n"); delay(200); ESP.restart(); }

static void cmdDisableCom() {
    USBSerial.print("\r\nCOM mode will be OFF on next boot. Rebooting...\r\n");
    delay(200);
    preferences.putBool("com_on", false);
    // v4.11 FIX: do NOT call preferences.end() here. `preferences` is the
    // shared global handle used across the whole firmware; closing it would
    // silently break every subsequent putBool/getBool from other modules if
    // the restart is delayed for any reason. The reboot below invalidates
    // the handle anyway.
    delay(400);
    ESP.restart();
}

static void cmdScripts() {
    if (!sdCardPresent) { USBSerial.print("\r\nSD not present\r\n"); return; }
    File d = SD.open(DIR_SCRIPTS);
    if (!d) { USBSerial.print("\r\n(no /scripts dir)\r\n"); return; }
    USBSerial.print("\r\nScripts on SD:\r\n");
    File f = d.openNextFile();
    while (f) {
        if (!f.isDirectory()) {
            String leaf = String(f.name());
            int slash = leaf.lastIndexOf('/');
            if (slash >= 0) leaf = leaf.substring(slash + 1);
            USBSerial.printf("  %s  (%u bytes)\r\n", leaf.c_str(), (unsigned)f.size());
        }
        f = d.openNextFile();
    }
    d.close();
}

// Declared in DuckyInterpreter.h — call directly.
extern void executeScript(const String& script);
extern void executeCommand(String line);
extern String loadScript(String filename);

static void cmdRun(const String& arg) {
    String name = arg; name.trim();
    if (name.length() == 0) { USBSerial.print("\r\nusage: run <script.txt>\r\n"); return; }
    String content = loadScript(name);
    if (content.length() == 0) { USBSerial.print("\r\nScript not found or empty: " + name + "\r\n"); return; }
    USBSerial.print("\r\nRunning " + name + "...\r\n");
    executeScript(content);
    USBSerial.print("\r\nDone.\r\n");
}

static void cmdExec(const String& line) {
    String c = line; c.trim();
    if (c.length() == 0) { USBSerial.print("\r\nusage: exec <ducky line>\r\n"); return; }
    USBSerial.print("\r\nexec: " + c + "\r\n");
    executeCommand(c);
    USBSerial.print("Done.\r\n");
}

static String prompt() {
    return "\r\nesp32-s3-key# ";
}

static void dispatch(const String& raw) {
    String c = raw; c.trim();
    if (c.length() == 0) return;
    // Split first word for arg-taking commands.
    int sp = c.indexOf(' ');
    String head = (sp < 0) ? c : c.substring(0, sp);
    String rest = (sp < 0) ? String("") : c.substring(sp + 1);
    String lower = head; lower.toLowerCase();

    if      (lower == "info")           cmdInfo();
    else if (lower == "help" || lower == "?") cmdHelp();
    else if (lower == "cpu")            USBSerial.printf("\r\nCPU: ESP32-S3 @ %u MHz (%u cores)\r\n",
                                                          (unsigned)ESP.getCpuFreqMHz(), (unsigned)ESP.getChipCores());
    else if (lower == "ram" || lower == "mem" || lower == "free")
                                        USBSerial.printf("\r\nRAM: %u KB free / %u KB total\r\n",
                                                          (unsigned)(ESP.getFreeHeap()/1024), (unsigned)(ESP.getHeapSize()/1024));
    else if (lower == "sd" || lower == "storage" || lower == "df") {
        if (!sdCardPresent) USBSerial.print("\r\nSD card not present\r\n");
        else USBSerial.printf("\r\nSD: %lu MB used / %lu MB total\r\n",
                              (unsigned long)(SD.usedBytes()/(1024*1024)),
                              (unsigned long)(SD.totalBytes()/(1024*1024)));
    }
    else if (lower == "ip")             cmdIp();
    else if (lower == "ap")             cmdAp();
    else if (lower == "scripts" || lower == "ls") cmdScripts();
    else if (lower == "run" || lower == "start")  cmdRun(rest);
    else if (lower == "exec" || lower == "e")     cmdExec(rest);
    else if (lower == "disable-com" || lower == "off") cmdDisableCom();
    else if (lower == "reboot" || lower == "restart")  cmdReboot();
    else if (lower == "clear" || lower == "cls")       cmdClear();
    else { USBSerial.print("\r\nUnknown: " + c + "  (try 'help')"); }
}

void comShellBegin() {
    // Register the CDC interface BEFORE USB.begin() so the composite descriptor
    // includes it. The caller (setup()) then calls USB.begin() itself.
    USBSerial.begin();
    g_active = true;
    logDebug("[COM] CDC shell armed on next USB.begin()");
}

bool comShellActive() { return g_active; }

void comShellLoop() {
    if (!g_active) return;
    // First time a host opens the port, print a banner.
    static bool banner = false;
    if (!banner && USBSerial) {   // USBCDC::operator bool == host has opened the port
        banner = true;
        USBSerial.print(
            "\r\n"
            "================================\r\n"
            " ESP32-S3 BadUSB — COM shell    \r\n"
            "================================\r\n"
            "Type 'help' for a command list.\r\n");
        USBSerial.print(prompt());
    }
    if (banner && !USBSerial) {
        // Host closed the port — reset banner so next opener sees it again.
        banner = false;
        g_lineBuf = "";
    }

    while (USBSerial && USBSerial.available()) {
        int c = USBSerial.read();
        if (c < 0) break;
        if (c == '\r' || c == '\n') {
            // Consume the LF of a CR/LF pair.
            if (c == '\r' && USBSerial.available() && USBSerial.peek() == '\n') USBSerial.read();
            dispatch(g_lineBuf);
            g_lineBuf = "";
            if (USBSerial) USBSerial.print(prompt());
            continue;
        }
        if (c == 8 || c == 127) {
            if (g_lineBuf.length()) {
                g_lineBuf.remove(g_lineBuf.length() - 1);
                USBSerial.write((uint8_t)'\b'); USBSerial.write((uint8_t)' '); USBSerial.write((uint8_t)'\b');
            }
            continue;
        }
        // Echo so the operator sees what they type in PuTTY.
        USBSerial.write((uint8_t)c);
        g_lineBuf += (char)c;
        if (g_lineBuf.length() > 256) g_lineBuf = g_lineBuf.substring(0, 256);
    }
}

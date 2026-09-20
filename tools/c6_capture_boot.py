#!/usr/bin/env python3
"""Capture the C6 UART/USJ boot log after a clean (non-download) reset.

Why this tool exists
--------------------
The C6 never seems to host the "P4-KeyLogger" AP, yet the WiFi stack + full
sdkconfig are proven identical to the known-good version3 snapshot. The only
observation we have so far is that the C6 comes up in ROM *download* mode
("boot:0x4 DOWNLOAD", "waiting for download") whenever it is reset with the
BOOT button still held. ROM download mode is decided purely by the GPIO9
strap - no application code can cause it.

This script performs an esptool-style HARD RESET (pulse EN low -> release,
exactly like idf.py flash does at the end) WITHOUT steering GPIO9, so the
chip boots from flash with whatever strap state it currently has. It then
reads the console for ~12 seconds and prints the log.

Usage
-----
    python tools/c6_capture_boot.py [COM_PORT]     (default: COM35)

Interpreting the output
-----------------------
*  "I (..) main: AP "P4-KeyLogger" up on channel 6"  -> WiFi really hosts;
   the problem is on the phone/PC scan side or RF. Not firmware.
*  "Guru Meditation" / "abort()" / "Backtrace"        -> the app crashed
   before/while starting WiFi. SAVE THE LOG and paste it to the agent.
*  "rst:0x4 (RTCWDT_RTC_RST)" + "waiting for download" -> chip is held in
   ROM download mode: RELEASE THE BOOT BUTTON (keep USB connected) and rerun.
*  any "esp_wifi_*" error line                        -> real init failure,
   e.g. "esp_wifi_start: ESP_ERR_NOT_SUPPORTED". Save the log.

Requires: pyserial  (pip install pyserial)
"""
import sys
import time

try:
    import serial
except ImportError:
    sys.exit("pyserial is required:  pip install pyserial")

PORT = sys.argv[1] if len(sys.argv) > 1 else "COM35"
BAUD = 115200
READ_SECONDS = 12.0

HIT_AP = ('AP "P4-KeyLogger" up on channel', "AP up on channel")
HIT_DOWNLOAD = ("boot:0x4 DOWNLOAD", "waiting for download", "rst:0x4",
                "load 0x4010", "jump to runtime", "download mode")


def reset_to_run(s):
    s.setDTR(False)   # keep GPIO9 strap line deasserted (never force download)
    s.setRTS(True)    # EN asserted (reset) - on boards where RTS->EN
    time.sleep(0.12)
    s.setRTS(False)   # EN released -> chip boots from flash with current strap
    time.sleep(0.12)


def main():
    print(f"Opening {PORT} @ {BAUD} ...")
    with serial.Serial(PORT, BAUD, timeout=0.2) as s:
        time.sleep(0.3)
        s.reset_input_buffer()
        print("Hard-resetting C6 (normal boot, BOOT/strap state untouched) ...")
        reset_to_run(s)
        buf = b""
        t0 = time.time()
        while time.time() - t0 < READ_SECONDS:
            d = s.read(4096)
            if d:
                buf += d
        txt = buf.decode("latin1", "replace")
        if not txt.strip():
            sys.exit("No console output. Is the C6 USB really on "
                     f"{PORT}? Is the P4 powered (C6 gets its rail from it)?")
        print("-" * 60)
        print(txt)
        print("-" * 60)
        low = txt.lower()
        if any(m in txt for m in HIT_DOWNLOAD) and "jump to runtime" in low:
            print("=> NORMAL BOOT (loader -> app).")
        if any(m in txt for m in HIT_DOWNLOAD):
            print("=> CHIP IS IN ROM DOWNLOAD MODE.")
            print('   The BOOT button / IO9 strap is LOW. Release BOOT (keep '
                  'USB) and rerun:  python tools/c6_capture_boot.py')
            return 1
        if any(m in txt for m in HIT_AP):
            print("=> WiFi AP 'P4-KeyLogger' IS UP. Not a firmware issue - "
                  "look at scan/antenna side.")
        if "guru meditation" in low or "abort()" in low:
            print("=> APP CRASHED. Save this log; paste it to the agent.")
            return 1
        for line in txt.splitlines():
            if "esp_wifi" in line and (" error" in line or "failed" in line):
                print("=> WIFI INIT FAILURE LINE: " + line.strip())
        return 0


if __name__ == "__main__":
    sys.exit(main())
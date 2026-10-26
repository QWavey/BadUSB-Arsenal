#ifndef ATTACK_MODE_H
#define ATTACK_MODE_H

#include <Arduino.h>

// Hak5-compatible ATTACKMODE + SIZE runtime configuration.
// Persisted to Preferences; applied at boot (composite descriptor is fixed at
// USB init, so changes to HID/STORAGE composition require a reboot).
struct AttackModeConfig {
  bool     hid     = true;     // present a HID keyboard
  bool     storage = false;    // present a USB Mass Storage device
  uint16_t vid     = 0x303A;   // last configured VID (0x303A = Espressif)
  uint16_t pid     = 0x0002;   // last configured PID
  uint64_t sizeBytes = 0;      // 0 = use full SD size; else clamp to actual
};

extern AttackModeConfig currentAttackMode;

// Parse and act on an ATTACKMODE line. Returns true if the line was
// recognised (and handled), false to let the interpreter fall through.
// Accepts (in any order): HID, STORAGE (aliases MSC, STORE), OFF, VID_XXXX,
// PID_XXXX, and a SIZE_<n>_<unit> token (GB/MB/KB) mixed in.
bool handleAttackModeLine(const String& line);

// Parse and act on a standalone SIZE line ("SIZE_22_GB"). Returns true if
// the line was recognised.
bool handleSizeLine(const String& line);

// Load persisted attack-mode/size settings into currentAttackMode.
void loadAttackModePrefs();

// Return the effective storage size in bytes (clamped to actual SD size).
// If sizeBytes == 0, returns the full card size.
uint64_t effectiveStorageBytes();

#endif // ATTACK_MODE_H

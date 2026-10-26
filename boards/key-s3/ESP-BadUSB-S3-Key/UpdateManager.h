#ifndef UPDATE_MANAGER_H
#define UPDATE_MANAGER_H

#include "GlobalState.h"

// Bundled update package (.espkg) support.
//
// A .espkg file bundles the website files (written to the SD card) AND a new
// firmware image (flashed over-the-air). It is applied website-first, firmware
// last, because applying the firmware reboots the chip.
//
// Container format (little-endian):
//   [0]   6 bytes  magic  = 'E','S','P','K','G',0x01
//   [6]   uint32   manifest length (M)
//   [10]  M bytes  manifest JSON: { "version":"..", "sd":[{"path","size"}...],
//                                   "fw":{"size"}, optional "crc32" fields }
//   [10+M] payloads, concatenated in this exact order:
//            every sd[] file's bytes (in listed order), then the fw bytes.

#define ESPKG_TMP_PATH "/update.espkg"

void handleUpdatePackageUpload();  // HTTP_UPLOAD: stream the .espkg to SD
void handleUpdatePackagePost();    // POST finalizer: arm apply, respond
void handleUpdateStatus();         // GET /api/update-status -> progress JSON
void processPendingUpdate();       // call from loop(): applies an armed package

#endif // UPDATE_MANAGER_H

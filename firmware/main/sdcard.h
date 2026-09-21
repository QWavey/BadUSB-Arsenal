#pragma once
// MicroSD card mount on the P4's SLOT_0 (SLOT_1 is the SDIO link to C6).
// Non-fatal on failure. Call once from app_main; check sdcard_present().
#include <stdbool.h>
#include "esp_err.h"
esp_err_t sdcard_mount(void);
bool      sdcard_present(void);

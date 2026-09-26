#pragma once
// USB Mass Storage on the P4. Exposes the "msc" partition (see
// partitions.csv, 16 MB FAT with wear-leveling) as a USB drive so
// ATTACKMODE STORAGE payloads have somewhere to drop / read files.
//
// Call msc_storage_init() ONCE before tinyusb_driver_install so the
// tud_msc_* callbacks in msc_storage.c have a working wear-leveling
// handle when TinyUSB enumerates.
//
// msc_storage_set_enabled(false) tells the tud_msc callbacks to
// report "no media present" — the SCSI TEST UNIT READY starts
// failing and the host sees an empty CD-ROM-shaped stub. Flipping it
// back to true re-exposes the volume without re-enumerating.
//
// msc_storage_set_fake_capacity(bytes) overrides what the LUN
// reports to the host as its size — DuckyScript's SIZE_512M /
// SIZE_1G / SIZE_2G commands persist through this. Zero means
// "report the real partition size".

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include "esp_err.h"

esp_err_t msc_storage_init(void);
void      msc_storage_set_enabled(bool on);
bool      msc_storage_enabled(void);
void      msc_storage_set_fake_capacity(uint64_t bytes);
uint64_t  msc_storage_capacity_bytes(void);
// Signal the host that the medium changed so it re-reads capacity (SIZE_).
void      msc_storage_signal_media_change(void);
// Reset the pending media-change eject before a fresh USB enumeration
// (called by main.c's usb_apply_state to prevent leaked state).
void      msc_storage_clear_media_change(void);
// Format the payload volume as a FRESH FATFS advertising `fake_bytes` of
// total capacity. Uses a shim diskio that reports the fake sector count so
// f_mkfs builds boot sector + FAT tables for the fake size; writes past the
// real 16 MB end are silently accepted (they just don't persist), so file
// storage still tops out at the real backing size. MUST be called while
// USB is detached (no dual-mount). Returns ESP_OK on success.
esp_err_t msc_storage_format_fake(uint64_t fake_bytes);

// Mount the payload-volume FAT read-only ON THE EXISTING wear-leveling handle
// (no second wl_mount), enumerate the root directory into `out` as one line
// per entry ("name\tsize\tisdir\n"), unmount, and return. REFUSES when the
// volume is currently exposed to the host (msc_storage_enabled()) so the
// device never dual-mounts the FAT that Windows owns. `out` is always NUL-
// terminated; *nfiles (optional) gets the entry count.
esp_err_t msc_storage_list(char *out, size_t out_sz, int *nfiles);

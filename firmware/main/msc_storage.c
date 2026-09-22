// USB Mass Storage backend for the P4. Owns:
//   - the "msc" partition (16 MB FAT, wear-leveling)
//   - the tud_msc_* callbacks TinyUSB dispatches on SCSI transactions
//   - a persisted enable/disable flag (ATTACKMODE STORAGE) and a
//     fake-capacity override (DuckyScript SIZE_1G etc.)
//
// The design intentionally KEEPS the MSC interface in the composite
// descriptor at all times — hot-adding an interface to TinyUSB requires
// a full re-install which drops the HID composite too. Instead we
// answer TEST UNIT READY with "no media" when the user has ATTACKMODE
// HID (no storage), and with the real FAT geometry when they have
// ATTACKMODE STORAGE. Windows briefly shows a drive letter with no
// media on toggle, then it goes live — exactly matching Hak5's
// behaviour.

#include "msc_storage.h"

#include <string.h>
#include "esp_log.h"
#include "esp_err.h"
#include "esp_partition.h"
#include "wear_levelling.h"
#include "nvs.h"

#include "tinyusb.h"
#include "class/msc/msc.h"
#include "class/msc/msc_device.h"  // tud_msc_set_sense (missing from msc.h)

static const char *TAG = "msc";

// Wear-leveled partition handle. Lives for the duration of the app.
static wl_handle_t s_wl = WL_INVALID_HANDLE;
static uint32_t    s_sector_size = 4096;
static uint32_t    s_sector_count = 0;   // real geometry
// ATTACKMODE STORAGE default: OFF. Hak5 semantics — a bare ATTACKMODE HID
// means keyboard + mouse ONLY, with no exposed volume; only ATTACKMODE HID
// STORAGE (or SIZE_* which implies STORAGE) makes the 16 MB payload volume
// visible to the host. Historically this defaulted to on and the drive
// popped up on first plug even when no ducky script had run — a data leak.
static bool        s_enabled = false;
static uint64_t    s_fake_capacity = 0;  // 0 = report real geometry

// -------- boot init: mount the wear-leveled partition ------------------
// We do NOT mount FAT here — the host is going to own the filesystem
// via MSC READ10/WRITE10 blocks. If we wanted to expose the same
// volume to ESP-IDF code we'd add esp_vfs_fat_spiflash_mount_rw_wl on
// top; there's no user of it here yet so we save the RAM.
esp_err_t msc_storage_init(void) {
    if (s_wl != WL_INVALID_HANDLE) return ESP_OK;
    const esp_partition_t *p = esp_partition_find_first(
        ESP_PARTITION_TYPE_DATA, ESP_PARTITION_SUBTYPE_DATA_FAT, "msc");
    if (!p) {
        ESP_LOGE(TAG, "no 'msc' partition found — check partitions.csv");
        return ESP_ERR_NOT_FOUND;
    }
    esp_err_t err = wl_mount(p, &s_wl);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "wl_mount: %s", esp_err_to_name(err));
        return err;
    }
    s_sector_size  = wl_sector_size(s_wl);
    s_sector_count = wl_size(s_wl) / s_sector_size;

    // Restore user preferences. Default = OFF when no NVS entry exists
    // (see the "ATTACKMODE STORAGE default: OFF" note at s_enabled).
    nvs_handle_t h;
    if (nvs_open("msc", NVS_READONLY, &h) == ESP_OK) {
        uint8_t on = 0;
        nvs_get_u8(h, "on", &on);
        s_enabled = on != 0;
        uint64_t cap = 0;
        nvs_get_u64(h, "cap", &cap);
        s_fake_capacity = cap;
        nvs_close(h);
    }
    ESP_LOGI(TAG, "MSC ready — %u sectors x %u bytes = %u MiB, %s, fake=%llu",
             (unsigned)s_sector_count, (unsigned)s_sector_size,
             (unsigned)(((uint64_t)s_sector_count * s_sector_size) >> 20),
             s_enabled ? "ENABLED" : "disabled",
             (unsigned long long)s_fake_capacity);
    return ESP_OK;
}

static void save_prefs(void) {
    nvs_handle_t h;
    if (nvs_open("msc", NVS_READWRITE, &h) != ESP_OK) return;
    nvs_set_u8 (h, "on",  s_enabled ? 1 : 0);
    nvs_set_u64(h, "cap", s_fake_capacity);
    nvs_commit(h); nvs_close(h);
}

void msc_storage_set_enabled(bool on) {
    if (s_enabled == on) return;
    s_enabled = on;
    save_prefs();
    ESP_LOGI(TAG, "ATTACKMODE STORAGE %s", on ? "on" : "off");
}
bool msc_storage_enabled(void) { return s_enabled; }

void msc_storage_set_fake_capacity(uint64_t bytes) {
    s_fake_capacity = bytes;
    save_prefs();
    ESP_LOGI(TAG, "fake capacity = %llu bytes", (unsigned long long)bytes);
}

uint64_t msc_storage_capacity_bytes(void) {
    if (s_fake_capacity) return s_fake_capacity;
    return (uint64_t)s_sector_count * s_sector_size;
}

// -------- TinyUSB MSC callbacks ---------------------------------------
// These fire from the TinyUSB task on core 1. Any long / blocking work
// here stalls the USB stack — wl_* calls are ~ms for small transfers
// which is fine.

void tud_msc_inquiry_cb(uint8_t lun, uint8_t vendor_id[8], uint8_t product_id[16], uint8_t product_rev[4]) {
    (void)lun;
    memcpy(vendor_id,  "P4BadUSB", 8);
    memcpy(product_id, "Payload Volume  ", 16);
    memcpy(product_rev, "1.0 ", 4);
}

// TEST UNIT READY. Return false + set sense = MEDIUM NOT PRESENT
// (SENSE_KEY=2, ASC=0x3A) when the user has toggled STORAGE off.
bool tud_msc_test_unit_ready_cb(uint8_t lun) {
    (void)lun;
    if (!s_enabled) {
        tud_msc_set_sense(lun, SCSI_SENSE_NOT_READY, 0x3A, 0x00);
        return false;
    }
    return true;
}

void tud_msc_capacity_cb(uint8_t lun, uint32_t *block_count, uint16_t *block_size) {
    (void)lun;
    uint32_t real_blocks = s_sector_count;
    uint32_t block_sz    = s_sector_size;
    if (s_fake_capacity) {
        // Round the user's requested capacity to the sector size and
        // clamp to what the partition can actually back. Reads beyond
        // the real end return zeros.
        uint64_t fake_blocks = (s_fake_capacity + block_sz - 1) / block_sz;
        if (fake_blocks > (uint64_t)0xFFFFFFFFULL) fake_blocks = 0xFFFFFFFFULL;
        *block_count = (uint32_t)fake_blocks;
    } else {
        *block_count = real_blocks;
    }
    *block_size = (uint16_t)block_sz;
}

// SCSI START/STOP UNIT (eject). We honour eject as "flip the enable
// flag off" so the host doesn't keep asking for capacity; a re-plug
// via ATTACKMODE HID STORAGE toggles it back.
bool tud_msc_start_stop_cb(uint8_t lun, uint8_t power_condition, bool start, bool load_eject) {
    (void)lun; (void)power_condition;
    if (load_eject && !start) msc_storage_set_enabled(false);
    return true;
}

int32_t tud_msc_read10_cb(uint8_t lun, uint32_t lba, uint32_t offset, void *buffer, uint32_t bufsize) {
    (void)lun;
    if (s_wl == WL_INVALID_HANDLE) return -1;
    if (lba >= s_sector_count) { memset(buffer, 0, bufsize); return (int32_t)bufsize; }
    esp_err_t err = wl_read(s_wl, (size_t)lba * s_sector_size + offset, buffer, bufsize);
    return (err == ESP_OK) ? (int32_t)bufsize : -1;
}

int32_t tud_msc_write10_cb(uint8_t lun, uint32_t lba, uint32_t offset, uint8_t *buffer, uint32_t bufsize) {
    (void)lun;
    if (s_wl == WL_INVALID_HANDLE) return -1;
    if (lba >= s_sector_count) return -1;
    // wl_write needs an aligned sector erase first, then a write. Do
    // it in one shot when offset==0 and bufsize is a whole sector,
    // which is the common case for MSC 512/4096-byte block IO.
    size_t addr = (size_t)lba * s_sector_size + offset;
    esp_err_t err;
    if (offset == 0 && bufsize == s_sector_size) {
        err = wl_erase_range(s_wl, addr, s_sector_size);
        if (err != ESP_OK) return -1;
    }
    err = wl_write(s_wl, addr, buffer, bufsize);
    return (err == ESP_OK) ? (int32_t)bufsize : -1;
}

// SCSI command not covered by the shortcuts above. TinyUSB's own
// example returns "invalid command" here; we do the same.
int32_t tud_msc_scsi_cb(uint8_t lun, uint8_t const scsi_cmd[16], void *buffer, uint16_t bufsize) {
    (void)lun; (void)scsi_cmd; (void)buffer; (void)bufsize;
    tud_msc_set_sense(lun, SCSI_SENSE_ILLEGAL_REQUEST, 0x20, 0x00);
    return -1;
}

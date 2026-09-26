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

#include <stdio.h>
#include <stdlib.h>
#include "ff.h"
#include "diskio_impl.h"   // ff_diskio_get_drive / ff_diskio_unregister
#include "diskio_wl.h"     // ff_diskio_register_wl_partition

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
// When the reported capacity changes while the volume is already attached, the
// host won't re-read geometry on its own. The reliable way to force it (per the
// TinyUSB maintainer) is to SIMULATE AN EJECT then INSERT: return "medium not
// present" (ASC 0x3A) for a few TEST UNIT READY polls so the host drops the
// volume, then become ready again so it re-mounts and re-reads READ CAPACITY.
// A single UNIT ATTENTION was too subtle for Windows.
static volatile int s_media_eject_polls = 0;   // >0 = report medium-absent this many polls

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

// Tell the host the medium changed: eject for a few polls, then re-appear so it
// re-reads capacity (see s_media_eject_polls).
void msc_storage_signal_media_change(void) { s_media_eject_polls = 4; }
// Called before a fresh enumeration so an old pending eject can't answer
// "not present" during the initial TEST UNIT READY sweep.
void msc_storage_clear_media_change(void) { s_media_eject_polls = 0; }

uint64_t msc_storage_capacity_bytes(void) {
    if (s_fake_capacity) return s_fake_capacity;
    return (uint64_t)s_sector_count * s_sector_size;
}

// ------- Format the payload volume at a FAKE capacity -------------------
// FATFS needs a diskio that reports the FAKE sector count so f_mkfs builds a
// boot sector + FAT tables sized for the FAKE volume, not the real 16 MB.
// This shim wraps the real wl handle and, on writes past the real end, just
// pretends success (fake data area sits beyond backing storage → users can
// write ~16 MB of files before their writes silently fail, which is the
// deliberate "fake size" trade-off).
#include "diskio_impl.h"

static wl_handle_t s_shim_wl = WL_INVALID_HANDLE;
static uint32_t    s_shim_sector_size = 4096;
static uint32_t    s_shim_real_sectors = 0;
static uint32_t    s_shim_fake_sectors = 0;

static DSTATUS shim_init(BYTE pdrv)   { (void)pdrv; return 0; }
static DSTATUS shim_status(BYTE pdrv) { (void)pdrv; return 0; }
static DRESULT shim_read(BYTE pdrv, BYTE *buff, DWORD sector, UINT count) {
    (void)pdrv;
    if (s_shim_wl == WL_INVALID_HANDLE) return RES_NOTRDY;
    if (sector >= s_shim_real_sectors) { memset(buff, 0, (size_t)count * s_shim_sector_size); return RES_OK; }
    if (sector + count > s_shim_real_sectors) {
        UINT tail = (sector + count) - s_shim_real_sectors;
        UINT head = count - tail;
        if (head && wl_read(s_shim_wl, (size_t)sector * s_shim_sector_size, buff, (size_t)head * s_shim_sector_size) != ESP_OK) return RES_ERROR;
        memset(buff + (size_t)head * s_shim_sector_size, 0, (size_t)tail * s_shim_sector_size);
        return RES_OK;
    }
    return wl_read(s_shim_wl, (size_t)sector * s_shim_sector_size, buff, (size_t)count * s_shim_sector_size) == ESP_OK ? RES_OK : RES_ERROR;
}
static DRESULT shim_write(BYTE pdrv, const BYTE *buff, DWORD sector, UINT count) {
    (void)pdrv;
    if (s_shim_wl == WL_INVALID_HANDLE) return RES_NOTRDY;
    if (sector >= s_shim_real_sectors) return RES_OK;      // silently drop past-end writes
    UINT clip = count;
    if (sector + clip > s_shim_real_sectors) clip = s_shim_real_sectors - sector;
    size_t addr = (size_t)sector * s_shim_sector_size;
    size_t len  = (size_t)clip   * s_shim_sector_size;
    if (wl_erase_range(s_shim_wl, addr, len) != ESP_OK) return RES_ERROR;
    if (wl_write(s_shim_wl, addr, buff, len)  != ESP_OK) return RES_ERROR;
    return RES_OK;
}
static DRESULT shim_ioctl(BYTE pdrv, BYTE cmd, void *buff) {
    (void)pdrv;
    switch (cmd) {
        case CTRL_SYNC:         return RES_OK;
        case GET_SECTOR_COUNT:  *(DWORD*)buff = s_shim_fake_sectors; return RES_OK;
        case GET_SECTOR_SIZE:   *(WORD*)buff  = (WORD)s_shim_sector_size; return RES_OK;
        case GET_BLOCK_SIZE:    *(DWORD*)buff = 1; return RES_OK;
        case CTRL_TRIM:         return RES_OK;
    }
    return RES_PARERR;
}
static const ff_diskio_impl_t s_shim_impl = {
    .init = shim_init, .status = shim_status,
    .read = shim_read, .write = shim_write, .ioctl = shim_ioctl,
};

esp_err_t msc_storage_format_fake(uint64_t fake_bytes) {
    if (s_wl == WL_INVALID_HANDLE) return ESP_ERR_INVALID_STATE;
    if (s_enabled)                 return ESP_ERR_INVALID_STATE;  // MUST be detached
    if (fake_bytes < 4 * 1024 * 1024) fake_bytes = (uint64_t)s_sector_count * s_sector_size;  // <4 MB is silly

    uint32_t sect_sz = s_sector_size;
    uint64_t fake_sect = (fake_bytes + sect_sz - 1) / sect_sz;
    if (fake_sect > (uint64_t)0xFFFFFFFFULL) fake_sect = 0xFFFFFFFFULL;

    // Pick a cluster size that keeps the FAT table inside our real 16 MB
    // backing (FAT32 entry = 4 bytes). Try 4 KB, 8, 16, 32, 64 KB and pick
    // the smallest that keeps two FATs under ~10 MB. For a 4 GB fake,
    // 4 KB clusters -> 4 MB FAT -> fits. For 1 TB, need bigger clusters.
    uint32_t au_bytes = sect_sz;
    while (au_bytes < 64 * 1024) {
        uint64_t clusters = fake_bytes / au_bytes;
        uint64_t fat_bytes = clusters * 4 + 4096;   // + reserved
        if (fat_bytes * 2 < 10ULL * 1024 * 1024) break;   // 2 FATs under 10 MB
        au_bytes *= 2;
    }

    BYTE pdrv = 0xFF;
    if (ff_diskio_get_drive(&pdrv) != ESP_OK || pdrv == 0xFF) return ESP_FAIL;

    s_shim_wl = s_wl;
    s_shim_sector_size  = sect_sz;
    s_shim_real_sectors = s_sector_count;
    s_shim_fake_sectors = (uint32_t)fake_sect;
    ff_diskio_register(pdrv, &s_shim_impl);

    char drv[3] = { (char)('0' + pdrv), ':', 0 };
    MKFS_PARM opt = { .fmt = FM_FAT32, .n_fat = 1, .align = 1, .n_root = 0, .au_size = au_bytes };
    void *workbuf = malloc(FF_MAX_SS * 4);
    esp_err_t ret = ESP_OK;
    if (!workbuf) { ret = ESP_ERR_NO_MEM; goto done; }
    FRESULT fr = f_mkfs(drv, &opt, workbuf, FF_MAX_SS * 4);
    if (fr != FR_OK) {
        ESP_LOGE(TAG, "f_mkfs failed: %d (fake=%llu au=%u)", fr, (unsigned long long)fake_bytes, (unsigned)au_bytes);
        ret = ESP_FAIL;
    } else {
        ESP_LOGI(TAG, "formatted fake FAT32: %llu bytes, au=%u, real_backing=%u sectors",
                 (unsigned long long)fake_bytes, (unsigned)au_bytes, (unsigned)s_sector_count);
    }
    free(workbuf);
done:
    ff_diskio_unregister(pdrv);
    s_shim_wl = WL_INVALID_HANDLE;
    return ret;
}

// Read-only listing of the payload-volume FAT root, mounted on the EXISTING
// wear-leveling handle so we don't wl_mount twice. Only safe while the volume
// is detached from the host (otherwise Windows owns the FAT).
esp_err_t msc_storage_list(char *out, size_t out_sz, int *nfiles) {
    if (nfiles) *nfiles = 0;
    if (out && out_sz) out[0] = 0;
    if (!out || out_sz < 2)          return ESP_ERR_INVALID_ARG;
    if (s_wl == WL_INVALID_HANDLE)   return ESP_ERR_INVALID_STATE;
    if (s_enabled)                   return ESP_ERR_INVALID_STATE;  // attached to host

    BYTE pdrv = 0xFF;
    if (ff_diskio_get_drive(&pdrv) != ESP_OK || pdrv == 0xFF) return ESP_FAIL;
    esp_err_t err = ff_diskio_register_wl_partition(pdrv, s_wl);
    if (err != ESP_OK) return err;

    char drv[3] = { (char)('0' + pdrv), ':', 0 };
    FATFS *fs = calloc(1, sizeof(FATFS));
    if (!fs) { ff_diskio_unregister(pdrv); return ESP_ERR_NO_MEM; }

    esp_err_t ret = ESP_OK;
    FRESULT fr = f_mount(fs, drv, 1);   // 1 = mount immediately
    if (fr != FR_OK) {
        ret = ESP_ERR_NOT_FOUND;        // unformatted / no FAT yet
    } else {
        size_t w = 0; int n = 0;
        FF_DIR dir; FILINFO fno;
        if (f_opendir(&dir, drv) == FR_OK) {
            while (f_readdir(&dir, &fno) == FR_OK && fno.fname[0]) {
                if (fno.fname[0] == '.') continue;         // '.'/'..'
                if (fno.fattrib & AM_HID) continue;        // hidden/system
                int wrote = snprintf(out + w, out_sz - w, "%s\t%lu\t%d\n",
                                     fno.fname, (unsigned long)fno.fsize,
                                     (fno.fattrib & AM_DIR) ? 1 : 0);
                if (wrote < 0 || (size_t)wrote >= out_sz - w) break;  // buffer full
                w += wrote; n++;
            }
            f_closedir(&dir);
        }
        out[w] = 0;
        if (nfiles) *nfiles = n;
        f_mount(NULL, drv, 0);          // unmount
    }
    ff_diskio_unregister(pdrv);
    free(fs);
    return ret;
}

// -------- TinyUSB MSC callbacks ---------------------------------------
// These fire from the TinyUSB task on core 1. Any long / blocking work
// here stalls the USB stack — wl_* calls are ~ms for small transfers
// which is fine.

void tud_msc_inquiry_cb(uint8_t lun, uint8_t vendor_id[8], uint8_t product_id[16], uint8_t product_rev[4]) {
    (void)lun;
    memcpy(vendor_id,  "P4BadUSB", 8);
    memcpy(product_id, "Payload Volume  ", 16);
    // Encode a hash of the current fake capacity into product_rev so a SIZE_
    // change flips the LUN identity — Windows caches disk geometry per
    // (vendor,product,rev) and would otherwise reuse the old size.
    uint32_t h = (uint32_t)(s_fake_capacity ^ (s_fake_capacity >> 32));
    static const char hex[] = "0123456789ABCDEF";
    product_rev[0] = hex[(h >> 12) & 0xF];
    product_rev[1] = hex[(h >>  8) & 0xF];
    product_rev[2] = hex[(h >>  4) & 0xF];
    product_rev[3] = hex[(h >>  0) & 0xF];
}

// TEST UNIT READY. Return false + set sense = MEDIUM NOT PRESENT
// (SENSE_KEY=2, ASC=0x3A) when the user has toggled STORAGE off.
bool tud_msc_test_unit_ready_cb(uint8_t lun) {
    (void)lun;
    if (!s_enabled) {
        tud_msc_set_sense(lun, SCSI_SENSE_NOT_READY, 0x3A, 0x00);   // medium not present
        return false;
    }
    if (s_media_eject_polls > 0) {
        // Simulate an eject for a few polls so the host drops the volume, then
        // fall through to ready so it re-mounts and re-reads the new capacity.
        s_media_eject_polls--;
        tud_msc_set_sense(lun, SCSI_SENSE_NOT_READY, 0x3A, 0x00);   // medium not present
        return false;
    }
    return true;
}

// Diagnostic: what did the last READ CAPACITY(10) actually report to Windows?
// This is the ground truth for the "SIZE_ shows 16 MB" bug — if fake_capacity
// is set but Windows sees 16 MB, one of these two exposes exactly where.
volatile uint32_t g_msc_last_block_count = 0;
volatile uint16_t g_msc_last_block_size  = 0;
volatile uint64_t g_msc_last_fake_bytes  = 0;
volatile uint32_t g_msc_capacity_cb_calls = 0;

void tud_msc_capacity_cb(uint8_t lun, uint32_t *block_count, uint16_t *block_size) {
    (void)lun;
    uint32_t real_blocks = s_sector_count;
    uint32_t block_sz    = s_sector_size;
    if (s_fake_capacity) {
        // Round the user's requested capacity UP to the sector size, and clamp
        // to what block_count (uint32_t) can hold. The MSC layer reports
        // (block_count-1) in READ CAPACITY(10) but block_count itself is
        // uint32_t, so 2^32 * block_sz = 16 TiB max for a 4 KiB sector.
        uint64_t fake_blocks = (s_fake_capacity + block_sz - 1) / block_sz;
        if (fake_blocks > (uint64_t)0xFFFFFFFFULL) fake_blocks = 0xFFFFFFFFULL;
        *block_count = (uint32_t)fake_blocks;
    } else {
        *block_count = real_blocks;
    }
    *block_size = (uint16_t)block_sz;
    g_msc_last_block_count = *block_count;
    g_msc_last_block_size  = *block_size;
    g_msc_last_fake_bytes  = s_fake_capacity;
    g_msc_capacity_cb_calls++;
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

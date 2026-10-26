#include "MSCManager.h"
#include "GlobalState.h"
#include "AttackMode.h"
#include "LogManager.h"        // logDebug() writes to /logs/debug.txt on SD
#include "esp32-hal-tinyusb.h" // pulls in tusb.h + tinyusb_enable_interface
#include "tusb.h"
#include "class/msc/msc.h"
#include "class/msc/msc_device.h"
#include <SD.h>
#include <Preferences.h>

bool mscRegistered = false;

// ---- v4.8: MSC sub-region translation --------------------------------------
// When mscSubSectors > 0, MSC reports a virtual disk of exactly that many
// 512-byte sectors, and every host LBA X is translated to real SD LBA
// (mscBaseSector + X). Everything outside [mscBaseSector, mscBaseSector +
// mscSubSectors) on the real SD is completely invisible and untouched by
// host writes.
//
// Populated from NVS at boot by mscLoadSubRegion() (called from setup() BEFORE
// mscBegin()). Cleared by mscClearSubRegion() (called from BEHAVE_BROKEN
// recovery). See MSCManager.h for the write-through API used by
// performBehaveBroken() and the SIZE_ command.
uint32_t mscBaseSector = 0;
uint32_t mscSubSectors = 0;

void mscLoadSubRegion() {
  Preferences p; p.begin("badusb", true);
  mscBaseSector = p.getUInt("msc_base", 0);
  mscSubSectors = p.getUInt("msc_sect", 0);
  p.end();
  // v4.16 FIX: bounds check vs the currently mounted card. If the user
  // swapped in a smaller SD after configuring BEHAVE_BROKEN or SIZE, the
  // sub-region can now overrun end-of-card and every MSC read/write returns
  // -1 (host sees a bricked drive with no clear cause). Auto-clear the
  // sub-region in that case.
  if (mscSubSectors > 0 && sdCardPresent) {
    uint32_t totalSectors = (uint32_t)(SD.totalBytes() / 512ULL);
    uint64_t end = (uint64_t)mscBaseSector + (uint64_t)mscSubSectors;
    if (end > (uint64_t)totalSectors) {
      Serial.printf("[MSC] Stored sub-region (base=%u sectors=%u end=%llu) "
                    "overruns current SD (%u sectors). Clearing.\n",
                    (unsigned)mscBaseSector, (unsigned)mscSubSectors,
                    (unsigned long long)end, (unsigned)totalSectors);
      mscClearSubRegion();
      return;
    }
  }
  if (mscSubSectors > 0) {
    Serial.printf("[MSC] Sub-region active: base LBA %u, size %u sectors "
                  "(%.1f MB). Host only sees this slice.\n",
                  (unsigned)mscBaseSector, (unsigned)mscSubSectors,
                  (double)mscSubSectors * 512.0 / (1024.0 * 1024.0));
  }
}

void mscSetSubRegion(uint32_t baseSector, uint32_t subSectors) {
  Preferences p; p.begin("badusb", false);
  p.putUInt("msc_base", baseSector);
  p.putUInt("msc_sect", subSectors);
  p.end();
  mscBaseSector = baseSector;
  mscSubSectors = subSectors;
  Serial.printf("[MSC] Sub-region set: base=%u sectors=%u\n",
                (unsigned)baseSector, (unsigned)subSectors);
}

void mscClearSubRegion() {
  Preferences p; p.begin("badusb", false);
  p.remove("msc_base");
  p.remove("msc_sect");
  p.end();
  mscBaseSector = 0;
  mscSubSectors = 0;
  Serial.println("[MSC] Sub-region cleared - MSC now exposes the full SD.");
}

// Parse the MBR at LBA 0 of the real SD and return the free space after the
// last primary partition. Returns true if any free space was found (>= 8 MB
// is a reasonable floor to avoid tiny slivers that no OS wants to format).
// The out params report where the free region starts and how big it is, in
// 512-byte sectors.
bool mscFindFreeSpaceAfterLastPartition(uint32_t& outStart, uint32_t& outSize) {
  outStart = 0;
  outSize  = 0;
  if (!sdCardPresent) return false;

  uint8_t mbr[512];
  if (!SD.readRAW(mbr, 0)) {
    Serial.println("[MSC] parseMBR: readRAW(0) failed");
    return false;
  }
  if (mbr[510] != 0x55 || mbr[511] != 0xAA) {
    Serial.println("[MSC] parseMBR: signature missing (not a partitioned SD)");
    return false;
  }

  // Four 16-byte partition entries starting at offset 446.
  uint32_t maxEnd = 0;
  bool anyValid = false;
  for (int i = 0; i < 4; i++) {
    const uint8_t* e = mbr + 446 + (i * 16);
    uint8_t type = e[4];
    if (type == 0x00) continue;                    // empty slot
    uint32_t start =
        (uint32_t)e[8]  |
       ((uint32_t)e[9]  << 8) |
       ((uint32_t)e[10] << 16) |
       ((uint32_t)e[11] << 24);
    uint32_t count =
        (uint32_t)e[12] |
       ((uint32_t)e[13] << 8) |
       ((uint32_t)e[14] << 16) |
       ((uint32_t)e[15] << 24);
    if (count == 0) continue;
    uint32_t end = start + count;                  // one past last sector
    if (end > maxEnd) maxEnd = end;
    anyValid = true;
    Serial.printf("[MSC] partition %d: type=0x%02X LBA %u..%u (%u sectors)\n",
                  i, (unsigned)type, (unsigned)start, (unsigned)(end - 1),
                  (unsigned)count);
  }
  if (!anyValid) {
    Serial.println("[MSC] parseMBR: no valid partitions found");
    return false;
  }

  uint32_t totalSectors = (uint32_t)(SD.totalBytes() / 512ULL);
  if (maxEnd >= totalSectors) {
    Serial.printf("[MSC] no free space after primary partition (last end=%u, "
                  "total=%u sectors)\n", (unsigned)maxEnd, (unsigned)totalSectors);
    return false;
  }
  uint32_t freeStart = maxEnd;
  uint32_t freeSize  = totalSectors - maxEnd;
  const uint32_t MIN_FREE = 8UL * 1024UL * 1024UL / 512UL;   // 8 MB
  if (freeSize < MIN_FREE) {
    Serial.printf("[MSC] only %u sectors free after primary partition (<8 MB); "
                  "sub-region not created\n", (unsigned)freeSize);
    return false;
  }
  outStart = freeStart;
  outSize  = freeSize;
  Serial.printf("[MSC] free region: LBA %u..%u (%.1f MB)\n",
                (unsigned)freeStart, (unsigned)(freeStart + freeSize - 1),
                (double)freeSize * 512.0 / (1024.0 * 1024.0));
  return true;
}

// -------------- Composite descriptor for the MSC interface --------------
// One bulk-in + one bulk-out endpoint. The tinyusb HAL picks the endpoint
// numbers; TUD_MSC_DESCRIPTOR builds a 23-byte interface descriptor
// (Interface header 9B + Endpoint OUT 7B + Endpoint IN 7B).

static uint16_t mscDescCb(uint8_t *dst, uint8_t *itf) {
  uint8_t itfnum   = *itf;
  uint8_t stridx   = tinyusb_add_string_descriptor("ESP32-S3 Storage");
  uint8_t epOut    = tinyusb_get_free_out_endpoint();
  uint8_t epIn     = tinyusb_get_free_in_endpoint();
  uint16_t epsize  = 64;
  logDebug("[MSC] descCb: itfnum=" + String(itfnum) +
           " stridx=" + String(stridx) +
           " epOut=" + String(epOut) +
           " epIn=" + String(epIn));
  if (epOut == 0 || epIn == 0) {
    logDebug("[MSC] ERROR: endpoint allocation failed (epOut/epIn=0)");
  }
  uint8_t descriptor[TUD_MSC_DESC_LEN] = {
    TUD_MSC_DESCRIPTOR(itfnum, stridx, epOut, (uint8_t)(0x80 | epIn), epsize)
  };
  memcpy(dst, descriptor, sizeof(descriptor));
  *itf += 1;   // we consumed 1 interface slot
  return sizeof(descriptor);
}

bool mscBegin() {
  if (!currentAttackMode.storage) {
    logDebug("[MSC] not registered (currentAttackMode.storage=false)");
    return false;
  }
  esp_err_t err = tinyusb_enable_interface(USB_INTERFACE_MSC, TUD_MSC_DESC_LEN, mscDescCb);
  if (err != ESP_OK) {
    logDebug("[MSC] tinyusb_enable_interface FAILED: " + String((int)err));
    return false;
  }
  mscRegistered = true;
  logDebug("[MSC] Registered composite MSC interface");
  return true;
}

// -------------------- TinyUSB MSC callbacks --------------------
// These are C-linkage callbacks called from the TinyUSB device task.
// Log every important call so we can diagnose enumeration problems by
// inspecting /logs/debug.txt on the SD card afterwards.

extern "C" {

// How many logical units (drives) we present. Return 0 = LUN 0 only.
uint8_t tud_msc_get_maxlun_cb(void) {
  return 1;
}

// SCSI INQUIRY response.
void tud_msc_inquiry_cb(uint8_t /*lun*/, uint8_t vendor_id[8],
                        uint8_t product_id[16], uint8_t product_rev[4]) {
  const char v[] = "ESP32S3";
  const char p[] = "BadUSB Storage";
  const char r[] = "1.0 ";
  memset(vendor_id, ' ', 8);   memcpy(vendor_id,  v, strlen(v) < 8  ? strlen(v)  : 8);
  memset(product_id, ' ', 16); memcpy(product_id, p, strlen(p) < 16 ? strlen(p) : 16);
  memset(product_rev, ' ', 4); memcpy(product_rev, r, strlen(r) < 4 ? strlen(r) : 4);
  logDebug("[MSC] INQUIRY response sent (SD present=" + String(sdCardPresent ? "yes" : "no") + ")");
}

// Media present / ready
bool tud_msc_test_unit_ready_cb(uint8_t /*lun*/) {
  bool r = sdCardPresent;
  static bool last = true;
  if (r != last) { logDebug("[MSC] test_unit_ready -> " + String(r ? "READY" : "NOT-READY")); last = r; }
  return r;
}

// Report disk capacity (block count + block size) to the host.
// SD.sectorSize() reads from the cached _card struct so it doesn't do SPI;
// SD.totalBytes() (inside effectiveStorageBytes) does, but capacity_cb is
// only called during enumeration, well before any ESP-side SD activity.
void tud_msc_capacity_cb(uint8_t /*lun*/, uint32_t *block_count, uint16_t *block_size) {
  uint32_t ss = SD.sectorSize();
  if (ss == 0) ss = 512;
  *block_size = (uint16_t)ss;

  // v4.8: sub-region translation. When mscSubSectors > 0, the host sees
  // exactly that many sectors regardless of the real SD size, and every LBA
  // access is offset by mscBaseSector on the way through to the SD driver.
  if (mscSubSectors > 0) {
    *block_count = mscSubSectors;
    return;
  }

  uint64_t bytes = effectiveStorageBytes();
  if (bytes == 0) {
    *block_count = 0;
  } else {
    uint64_t sectors = bytes / ss;
    if (sectors > 0xFFFFFFFFULL) sectors = 0xFFFFFFFFULL;
    *block_count = (uint32_t)sectors;
  }
}

// Host wants to eject / start / stop the medium.
// Accept both — refusing eject would make Windows show "Cannot eject" errors.
bool tud_msc_start_stop_cb(uint8_t /*lun*/, uint8_t /*power_condition*/,
                           bool start, bool load_eject) {
  logDebug("[MSC] start_stop start=" + String(start) + " eject=" + String(load_eject));
  return true;
}

// SCSI READ10: read `bufsize` bytes starting at LBA `lba`+`offset`.
// NOTE: no logDebug() here — logDebug uses the SD File API which shares the
// SPI mutex with SD.readRAW/writeRAW. Nested access would deadlock/corrupt.
int32_t tud_msc_read10_cb(uint8_t /*lun*/, uint32_t lba, uint32_t offset,
                          void *buffer, uint32_t bufsize) {
  if (!sdCardPresent) return -1;
  const uint32_t SS = 512;
  if (offset != 0)      return -1;                 // partial-sector unsupported
  if (bufsize % SS != 0) return -1;
  uint32_t sectors = bufsize / SS;
  // v4.8: sub-region translation - bound-check + offset into real SD.
  // v4.11 SECURITY: integer-overflow-safe bounds check. `lba + sectors` can
  // wrap to a small value in uint32 when a malicious host sends
  // lba=0xFFFFFFF0, sectors=0x20, defeating the sub-region isolation.
  if (mscSubSectors > 0) {
    if (sectors > mscSubSectors || lba > mscSubSectors - sectors) return -1;
    lba += mscBaseSector;
  }
  uint8_t* p = (uint8_t*)buffer;
  for (uint32_t i = 0; i < sectors; i++) {
    if (!SD.readRAW(p + (i * SS), lba + i)) return -1;
  }
  return (int32_t)bufsize;
}

// SCSI WRITE10: write `bufsize` bytes starting at LBA `lba`+`offset`.
int32_t tud_msc_write10_cb(uint8_t /*lun*/, uint32_t lba, uint32_t offset,
                           uint8_t *buffer, uint32_t bufsize) {
  if (!sdCardPresent) return -1;
  const uint32_t SS = 512;
  if (offset != 0)      return -1;
  if (bufsize % SS != 0) return -1;
  uint32_t sectors = bufsize / SS;
  // v4.8: sub-region translation - refuse writes past the sub-region so a
  // host format can't reach into the primary partition and clobber files.
  // v4.11 SECURITY: overflow-safe (see read10 comment).
  if (mscSubSectors > 0) {
    if (sectors > mscSubSectors || lba > mscSubSectors - sectors) return -1;
    lba += mscBaseSector;
  }
  for (uint32_t i = 0; i < sectors; i++) {
    if (!SD.writeRAW(buffer + (i * SS), lba + i)) return -1;
  }
  return (int32_t)bufsize;
}

// Generic SCSI command handler. Return
//   >0 : bytes returned in `buffer`
//    0 : success, no data
//   -1 : STALL (command not supported); host may retry or accept the failure
int32_t tud_msc_scsi_cb(uint8_t /*lun*/, uint8_t const scsi_cmd[16],
                        void* /*buffer*/, uint16_t /*bufsize*/) {
  // We don't implement any custom SCSI verbs (MODE_SENSE, PREVENT_ALLOW,
  // REQUEST_SENSE etc. are handled inside tinyusb). Return -1 so tinyusb
  // sets sense data indicating the command is unsupported, rather than
  // silently pretending success (which stalls Windows waiting for data).
  logDebug("[MSC] scsi_cb op=0x" + String(scsi_cmd[0], HEX));
  return -1;
}

}  // extern "C"

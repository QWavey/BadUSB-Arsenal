#ifndef MSC_MANAGER_H
#define MSC_MANAGER_H

#include <stdint.h>   // v4.9: for uint32_t used by the sub-region API below

// USB Mass Storage device that exposes the SD card as a removable disk.
// Registered as a composite interface alongside HID via
// tinyusb_enable_interface(USB_INTERFACE_MSC, ...) BEFORE USB.begin().

// Register the MSC interface with TinyUSB. Must be called BEFORE USB.begin().
// Returns true on success. If STORAGE is not requested in the current
// AttackMode, returns false without registering (HID-only device).
bool mscBegin();

// True once the MSC interface has been registered this boot.
extern bool mscRegistered;

// v4.8 sub-region translation. When mscSubSectors > 0, the MSC host sees a
// virtual disk of exactly mscSubSectors 512-byte sectors, and every host LBA
// X is translated to real SD LBA (mscBaseSector + X). Everything outside
// that window is invisible and untouchable from the host side.
extern uint32_t mscBaseSector;
extern uint32_t mscSubSectors;

// Load the sub-region parameters from NVS (call from setup() BEFORE mscBegin).
void mscLoadSubRegion();

// Persist and apply a new sub-region.
void mscSetSubRegion(uint32_t baseSector, uint32_t subSectors);

// Clear the sub-region so MSC exposes the full SD again.
void mscClearSubRegion();

// Parse the MBR at real LBA 0 and return the start + length of the largest
// contiguous free-space region after the last valid primary partition.
// Returns false when there is no MBR, no free space, or too little
// (< ~8 MB) to be useful. Sizes are in 512-byte sectors.
bool mscFindFreeSpaceAfterLastPartition(uint32_t& outStart, uint32_t& outSize);

#endif // MSC_MANAGER_H

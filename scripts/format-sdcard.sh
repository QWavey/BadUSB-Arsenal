#!/usr/bin/env bash
# Formats and seeds a MicroSD for GhostLink.
# Usage: sudo ./scripts/format-sdcard.sh /dev/sdX
#
# WARNING: destroys everything on the target device. Double-check the path.
set -euo pipefail

if [[ $# -ne 1 ]]; then
  echo "usage: $0 /dev/sdX   (NOT a partition, the whole disk)"
  exit 1
fi
DEV="$1"
[[ -b "$DEV" ]] || { echo "not a block device: $DEV"; exit 1; }

echo "This will WIPE $DEV. Type the device path to confirm:"
read -r CONFIRM
[[ "$CONFIRM" == "$DEV" ]] || { echo "aborted"; exit 1; }

sudo wipefs -a "$DEV"
sudo parted -s "$DEV" mklabel msdos mkpart primary fat32 1MiB 100%
sudo mkfs.vfat -F32 -n GHOSTLINK "${DEV}1"
MNT=$(mktemp -d)
sudo mount "${DEV}1" "$MNT"
sudo cp -r sdcard-template/* "$MNT/"
sync
sudo umount "$MNT"
rmdir "$MNT"
echo "ok: $DEV ready. Insert into the WebBee and boot."

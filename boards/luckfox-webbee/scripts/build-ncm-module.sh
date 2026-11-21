#!/bin/sh
# Builds usb_f_ncm.ko for the WebBee's kernel (Linux 5.10.160, ARMv7, no SMP, no preempt,
# no module versioning). The device's gadget core (u_ether, libcomposite) is built in, so the
# module uses those symbols and does not carry a second copy of them.
#
# Run in WSL Ubuntu:  sh scripts/build-ncm-module.sh
# Output: ~/kbuild/out/usb_f_ncm.ko
set -eu

KVER=5.10.160
WORK="$HOME/kbuild"
TREE="$WORK/linux-$KVER"
CROSS=arm-linux-gnueabihf-
OUT="$WORK/out"
REPO_DIR=$(cd "$(dirname "$0")/.." && pwd)

mkdir -p "$WORK" "$OUT"
cd "$WORK"
[ -f "linux-$KVER.tar.xz" ] || wget -q -O "linux-$KVER.tar.xz" "https://cdn.kernel.org/pub/linux/kernel/v5.x/linux-$KVER.tar.xz"
[ -d "$TREE" ] || tar xf "linux-$KVER.tar.xz"

cd "$TREE"
# The Luckfox RV1106 config: the struct module layout of this config, with DEBUG_MUTEXES on,
# is the one the device's own modules use (init at 0xd4, exit at 0x178, size 0x180).
LFP=https://raw.githubusercontent.com/LuckfoxTECH/luckfox-pico/main/sysdrv/source/kernel/arch/arm/configs
[ -f "$WORK/luckfox_rv1106_linux_defconfig" ] || wget -q -O "$WORK/luckfox_rv1106_linux_defconfig" "$LFP/luckfox_rv1106_linux_defconfig"
cp "$WORK/luckfox_rv1106_linux_defconfig" arch/arm/configs/luckfox_rv1106_defconfig
make ARCH=arm CROSS_COMPILE=$CROSS luckfox_rv1106_defconfig
# Match the device's kernel build: thumb2 (vermagic), no SMP or preempt, module unload, no symbol
# versions, and DEBUG_MUTEXES (the struct module layout the device's modules use).
scripts/config --disable SMP --disable PREEMPT --enable THUMB2_KERNEL \
               --enable MODULES --enable MODULE_UNLOAD --disable MODVERSIONS \
               --enable DEBUG_MUTEXES --enable USB_GADGET --enable USB_CONFIGFS
make ARCH=arm CROSS_COMPILE=$CROSS olddefconfig
make ARCH=arm CROSS_COMPILE=$CROSS syncconfig
make ARCH=arm CROSS_COMPILE=$CROSS -j"$(nproc)" modules_prepare

# Build only f_ncm.c, out of tree, against this kernel's headers.
NCM="$WORK/ncm"
rm -rf "$NCM" && mkdir -p "$NCM"
cp "$TREE/drivers/usb/gadget/function/f_ncm.c" "$NCM/"
cp "$TREE/drivers/usb/gadget/function/"*.h "$NCM/"
cp "$TREE/drivers/usb/gadget/configfs.h" "$NCM/"
cat > "$NCM/Makefile" <<'MK'
obj-m := usb_f_ncm.o
usb_f_ncm-y := f_ncm.o
# The device's kernel has no __stack_chk_guard; build without stack protection.
ccflags-y := -fno-stack-protector
MK
cd "$NCM"
make -C "$TREE" M="$NCM" ARCH=arm CROSS_COMPILE=$CROSS modules
cp "$NCM/usb_f_ncm.ko" "$OUT/usb_f_ncm.ko"
echo "built: $OUT/usb_f_ncm.ko"

# Hardware & flashing

## Target: Luckfox WebBee

- **SoC**: Rockchip **RV1106** (ARM Cortex-A7, up to 1.2 GHz, RISC-V coprocessor,
  0.5 TOPS NPU)
- **Memory**: 128/256MB DDR, 128MB SPI NAND
- **Storage**: SD/MicroSD slot (SPI NAND is primary; SD is for user data /
  GhostLink SD layout)
- **USB**: USB-C OTG, device mode (NCM/RNDIS/HID/MSC gadgets)
- **Network**: 10/100 Ethernet via onboard PHY, exposed as `eth0`
- **Button**: 1, on a GPIO (varies — the firmware tries a few paths)
- **LED**: at least 1 status LED

## Partition map (SPI NAND, stock Luckfox layout — do not change)

Decoded from the Luckfox 2025-06-07 release's `sd_update.txt`:

| Partition  | NAND offset  | NAND size  | Contents                         |
|------------|--------------|------------|-----------------------------------|
| env        | 0x00000000   | 0x00040000 | 256 KiB U-Boot env               |
| idblock    | 0x00040000   | 0x00040000 | 256 KiB id block + loader stage1 |
| uboot      | 0x00080000   | 0x00080000 | 512 KiB U-Boot                   |
| boot       | 0x00100000   | 0x00400000 | 4 MiB kernel + DTB (FIT image)   |
| oem        | 0x00500000   | 0x01E00000 | 30 MiB OEM userspace (UBI)       |
| userdata   | 0x02300000   | 0x00600000 | 6 MiB userdata (UBI)             |
| rootfs     | 0x02900000   | 0x05500000 | 85 MiB rootfs (UBI)              |

`rootfs.img` is a UBI image; GhostLink installs on top of the stock Buildroot
by overlaying `/usr/sbin/ghostlink` and `/etc/init.d/S99ghost` before
repacking.

## Boot path we rely on

1. ROM boots from idblock, loads stage1 from NAND.
2. U-Boot comes up, loads kernel + DTB from `boot`.
3. Kernel mounts UBI `rootfs`, exec's `/sbin/init` (busybox init).
4. `/etc/init.d/rcS` runs. GhostLink's `S99ghost` is the final step.
5. GhostLink mounts SD at `/mnt/sd`, reads `config.txt`, and takes over.

## Flashing (Windows, stock Luckfox tooling)

1. Install `Rockchip USB driver` via `RK Driver Assistant`, reboot PC.
2. Open `SocToolKit.exe` **as admin**.
3. Choose `RV1106`.
4. Hold **BOOT** on the WebBee while plugging USB-C — it enumerates as
   MaskRom.
5. Select the `webbee-ghostlink/` **rootfs** build output's parent folder.
6. Click **Download** to flash just the rootfs partition, or **Upgrade** for
   the whole `update.img`.

## Flashing (Linux)

Place a new `update.img` in `output/image/`, then:

```bash
sudo ./rkflash.sh update
# or
sudo upgrade_tool uf output/image/update.img
```

## SD card

Format a MicroSD as **FAT32** (preferred) or exFAT/ext4. The firmware auto-
detects. Lay out the folders shown in the main README and drop `config.txt`
at the root. The scaffold is in `sdcard-template/`.

## HID interface

HID gadget is composite with NCM. On an RV1106 kernel built with
`CONFIG_USB_CONFIGFS_F_HID=y`, `/dev/hidg0` is a boot-protocol keyboard. The
stock Luckfox WebBee buildroot has this on. If it isn't, add it to the
kernel defconfig and rebuild.

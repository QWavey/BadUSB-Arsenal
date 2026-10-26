# Testing GhostLink on hardware without a cross-compiler

The C firmware needs a Linux toolchain we don't have on this Windows box.
Fastest honest test path uses `ghostlink.sh` — a pure-shell PoC that uses only
what the stock Luckfox buildroot ships. It validates the whole design on real
hardware today; the C binary drops in later and replaces it.

## Flow

1. **Flash stock Luckfox WebBee firmware** (device is already in MaskRom).
2. **Prepare an SD card** with `sdcard-template/` + the shell PoC on it.
3. **Boot**. Device enumerates as USB-Ethernet (NCM) + HID keyboard. DHCP
   hands your laptop an address in 172.16.24.0/24.
4. **Verify** via WebUI, SSH, pcap on SD.

## 1. Flash stock firmware

```
Tools: C:\Users\Flo\Downloads\NewApps\Tools_and_Firmware\Flash_tool (RUN AS ADMIN)\SocToolKit.exe
Image: C:\Users\Flo\Downloads\NewApps\Tools_and_Firmware\Original_Firmware\Luckfox_Pico_WebBee_Flash_250607\
```

Steps (SocToolKit GUI):
- Right-click SocToolKit.exe → Run as administrator.
- Choose SoC: **RV1106**.
- Hold BOOT on the WebBee while plugging USB-C — it should show as MaskRom.
- Click `Firmware` tab → point at the `Luckfox_Pico_WebBee_Flash_250607` dir.
- Click **Upgrade** (full image) — ~40s.
- Device reboots into stock Buildroot.

## 2. Prepare the SD card

- Format a MicroSD (≥4 GB) as FAT32, label `GHOSTLINK`.
- Copy the entire **sdcard-template/** tree to the SD root. Verify these
  exist at the top level:
  - `config.txt`
  - `ghostlink.sh`
  - `Records/` `Payloads/` `Languages/` `WebUI/` ...
- Make `ghostlink.sh` executable later on the device: `chmod +x /mnt/sd/ghostlink.sh`.

## 3. Boot and auto-start

Insert SD, plug USB-C. The stock buildroot comes up in a couple of seconds.

Hook ghostlink.sh into autostart. Easiest route via serial / ADB:

```sh
# Connect: adb shell  (or serial 115200)
mount /dev/mmcblk1p1 /mnt
sh /mnt/ghostlink.sh &
```

Or install a persistent autostart entry (per Luckfox's "Autostart Configuration"):

```sh
# on the WebBee, as root
cat > /etc/init.d/S99ghostlink <<'EOF'
#!/bin/sh
case "$1" in
  start) sh /mnt/sd/ghostlink.sh > /dev/kmsg 2>&1 & ;;
  stop)  pkill -f ghostlink.sh ;;
esac
exit 0
EOF
chmod +x /etc/init.d/S99ghostlink
```

Reboot. On next boot, `ghostlink.sh` auto-launches.

## 4. Verify from the laptop

Unplug and re-plug USB-C into your laptop.

- Windows should enumerate "GhostLink WebBee Passthrough" as a USB Ethernet
  adapter. DHCP gives you e.g. `172.16.24.10`.
- Browse to **http://172.16.24.1/** — see the shell PoC dashboard.
- `/cgi-bin/status` shows interfaces + gadget state.
- SSH to `root@172.16.24.1` (default stock password; set in Luckfox docs).
- Check recording: `ls -lh /mnt/sd/Records/` — pcap files should be growing.
- `tcpdump -r /mnt/sd/Records/rec_*.pcap -c 20` or copy via `scp` and open in
  Wireshark.
- HID: `echo -ne '\x00\x00\x04\x00\x00\x00\x00\x00' > /dev/hidg0` should type
  the letter `a` into whatever has focus on the host. (Boot-protocol byte 3 =
  HID keycode.)

## Next: real C firmware

Once this works, we go for the real build:

```bash
# in WSL / Ubuntu
wget https://files.luckfox.com/wiki/Luckfox-Pico/Software/arm-rockchip830-linux-uclibcgnueabihf.tar.gz
tar xzf arm-rockchip830-linux-uclibcgnueabihf.tar.gz -C ~/
export PATH=~/arm-rockchip830-linux-uclibcgnueabihf/bin:$PATH

cd /mnt/c/Users/Flo/Downloads/NewApps/webbee-ghostlink
./scripts/build.sh
# -> out/ghostlink  (~180KB stripped)
```

Then overlay into the rootfs or copy to `/usr/sbin/ghostlink` on the device
and replace the ghostlink.sh launch with the C binary.

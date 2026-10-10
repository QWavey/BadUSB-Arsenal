#!/bin/sh
# =============================================================================
# ghostlink.sh - BusyBox-friendly PoC of the GhostLink firmware
# =============================================================================
# Pure shell. Uses only what the stock Luckfox WebBee buildroot ships
# (configfs, iproute2, nftables, udhcpd, busybox httpd, tcpdump).
# No cross-compiler needed, no overlay — drop onto SD and run.
#
# Validates the end-to-end design (SD config -> gadget + bridge + record +
# webui) on real hardware before we invest in the C build. The C binary
# will later replace this script in /usr/sbin/ghostlink.
# =============================================================================

SD=/mnt/sd
CFG=$SD/config.txt
LOG=$SD/Logs/ghostlink.log

log() {
    local t
    t=$(date +'%H:%M:%S')
    echo "$t $*" | tee -a "$LOG" 2>/dev/null
}

# ---------- 1. mount SD ----------
mkdir -p /mnt/sd
if ! mountpoint -q "$SD"; then
    for fs in vfat exfat ext4; do
        mount -t $fs /dev/mmcblk1p1 "$SD" 2>/dev/null && break
    done
fi

mkdir -p "$SD/Records" "$SD/Payloads" "$SD/Languages" "$SD/WebUI" \
         "$SD/Scripts" "$SD/Loot" "$SD/Spoofhosts" "$SD/Honeypot" \
         "$SD/Firmware" "$SD/Logs" "$SD/Modules" "$SD/Rules"

if [ ! -f "$CFG" ]; then
    cat > "$CFG" <<EOF
USB=TRUE
PASSTHROUGH_LAN=TRUE
NETMODE=NAT
ASSIGN_IP=TRUE
GATEWAY_IP=172.16.24.1
DHCP_RANGE=172.16.24.10,172.16.24.60
LOG_TO_SD=TRUE
WEBUI=TRUE
WEBUI_PORT=80
HID_ENABLED=TRUE
DEADNET=FALSE
REDIRECT=FALSE
EOF
fi

log "ghostlink-sh starting"

# ---------- 2. parse config.txt into env ----------
# grep lines of form KEY=VAL, export each. Strip inline # comments.
while IFS='=' read -r k v; do
    case "$k" in \#*|'') continue ;; esac
    v="${v%%#*}"
    v="${v%"${v##*[![:space:]]}"}"
    k="${k%"${k##*[![:space:]]}"}"
    k="${k#"${k%%[![:space:]]*}"}"
    [ -n "$k" ] || continue
    eval "GCFG_$k='$v'"
done < "$CFG"

truthy() { case "$1" in TRUE|true|1|YES|yes|on|ON) return 0 ;; *) return 1 ;; esac; }

# ---------- 3. USB gadget via configfs ----------
if truthy "${GCFG_USB:-TRUE}"; then
    log "setting up USB gadget"
    modprobe libcomposite 2>/dev/null
    mkdir -p /sys/kernel/config
    mountpoint -q /sys/kernel/config || mount -t configfs none /sys/kernel/config

    G=/sys/kernel/config/usb_gadget/ghostlink
    mkdir -p $G
    echo "${GCFG_USB_VID:-0x1D6B}" > $G/idVendor
    echo "${GCFG_USB_PID:-0x0137}" > $G/idProduct
    echo "0x0100" > $G/bcdDevice
    echo "0x0200" > $G/bcdUSB

    mkdir -p $G/strings/0x409
    SERIAL=$(cat /proc/cpuinfo | awk -F: '/Serial/{gsub(/ /,"",$2); print $2}')
    [ -n "$SERIAL" ] || SERIAL="GHOSTLINK$$"
    echo "$SERIAL" > $G/strings/0x409/serialnumber
    echo "${GCFG_USB_MANUFACTURER:-GhostLink}" > $G/strings/0x409/manufacturer
    echo "${GCFG_USB_PRODUCT:-WebBee Passthrough}" > $G/strings/0x409/product

    mkdir -p $G/configs/c.1/strings/0x409
    echo "GhostLink" > $G/configs/c.1/strings/0x409/configuration
    echo 250 > $G/configs/c.1/MaxPower

    # NCM function
    mkdir -p $G/functions/ncm.usb0
    echo "02:00:60:0D:AA:BB" > $G/functions/ncm.usb0/host_addr
    echo "02:00:60:0D:AA:CC" > $G/functions/ncm.usb0/dev_addr
    ln -sf $G/functions/ncm.usb0 $G/configs/c.1/ncm.usb0

    # HID function
    if truthy "${GCFG_USB_HID:-TRUE}"; then
        mkdir -p $G/functions/hid.usb0
        echo 1 > $G/functions/hid.usb0/protocol
        echo 1 > $G/functions/hid.usb0/subclass
        echo 8 > $G/functions/hid.usb0/report_length
        # Boot keyboard HID descriptor
        printf '\x05\x01\x09\x06\xa1\x01\x05\x07\x19\xe0\x29\xe7\x15\x00\x25\x01\x75\x01\x95\x08\x81\x02\x95\x01\x75\x08\x81\x03\x95\x05\x75\x01\x05\x08\x19\x01\x29\x05\x91\x02\x95\x01\x75\x03\x91\x03\x95\x06\x75\x08\x15\x00\x25\x65\x05\x07\x19\x00\x29\x65\x81\x00\xc0' > $G/functions/hid.usb0/report_desc
        ln -sf $G/functions/hid.usb0 $G/configs/c.1/hid.usb0
    fi

    UDC=$(ls /sys/class/udc | head -n1)
    if [ -n "$UDC" ]; then
        echo "$UDC" > $G/UDC
        log "USB gadget bound to UDC $UDC"
    else
        log "WARN: no UDC found"
    fi
fi

# ---------- 4. bring up interfaces ----------
sleep 0.3
ip link set lo up
[ -e /sys/class/net/eth0 ] && ip link set eth0 up
[ -e /sys/class/net/usb0 ] && ip link set usb0 up

# ---------- 5. passthrough ----------
if truthy "${GCFG_PASSTHROUGH_LAN:-TRUE}"; then
    MODE="${GCFG_NETMODE:-BRIDGE}"
    case "$MODE" in
    BRIDGE|TRANSPARENT)
        log "bridge mode"
        ip link add name br0 type bridge 2>/dev/null
        ip link set br0 up
        [ -e /sys/class/net/eth0 ] && ip link set eth0 master br0
        [ -e /sys/class/net/usb0 ] && ip link set usb0 master br0
        [ "$MODE" = "BRIDGE" ] && udhcpc -i br0 -b -q 2>/dev/null &
        ;;
    NAT)
        log "NAT mode"
        echo 1 > /proc/sys/net/ipv4/ip_forward
        [ -e /sys/class/net/usb0 ] && ip addr add "${GCFG_GATEWAY_IP:-172.16.24.1}/24" dev usb0
        [ -e /sys/class/net/eth0 ] && udhcpc -i eth0 -b -q 2>/dev/null &
        nft add table ip nat 2>/dev/null
        nft 'add chain ip nat postrouting { type nat hook postrouting priority 100; }' 2>/dev/null
        nft 'add rule ip nat postrouting oifname eth0 masquerade' 2>/dev/null
        ;;
    ISOLATE)
        echo 0 > /proc/sys/net/ipv4/ip_forward
        [ -e /sys/class/net/usb0 ] && ip addr add "${GCFG_GATEWAY_IP:-172.16.24.1}/24" dev usb0
        ;;
    esac
fi

# ---------- 6. DHCP server on host side ----------
if truthy "${GCFG_ASSIGN_IP:-TRUE}" && [ -e /sys/class/net/usb0 ]; then
    DR="${GCFG_DHCP_RANGE:-172.16.24.10,172.16.24.60}"
    RS="${DR%%,*}"
    RE="${DR##*,}"
    cat > /tmp/udhcpd.conf <<EOF
start   $RS
end     $RE
interface usb0
option subnet 255.255.255.0
option router ${GCFG_GATEWAY_IP:-172.16.24.1}
option dns ${GCFG_DNS_UPSTREAM:-1.1.1.1}
option lease 86400
lease_file /tmp/udhcpd.leases
pidfile /tmp/udhcpd.pid
EOF
    [ -f /tmp/udhcpd.leases ] || touch /tmp/udhcpd.leases
    udhcpd /tmp/udhcpd.conf 2>>$LOG &
    log "udhcpd on usb0 (range $RS-$RE)"
fi

# ---------- 7. recording ----------
if truthy "${GCFG_LOG_TO_SD:-TRUE}"; then
    IF=br0
    [ -e /sys/class/net/br0 ] || IF=eth0
    TS=$(date -u +%Y%m%dT%H%M%SZ)
    OUT="$SD/Records/rec_${TS}.pcap"
    log "recorder on $IF -> $OUT (rotating)"
    (
        while true; do
            TS=$(date -u +%Y%m%dT%H%M%SZ)
            OUT="$SD/Records/rec_${TS}.pcap"
            tcpdump -i $IF -w "$OUT" -C ${GCFG_RECORD_ROTATE_MB:-64} \
                    -W 1 -s 65535 2>>$LOG
        done
    ) &
    echo $! > /tmp/ghostlink-rec.pid
fi

# ---------- 8. webui (BusyBox httpd) ----------
if truthy "${GCFG_WEBUI:-TRUE}"; then
    mkdir -p /tmp/www
    cat > /tmp/www/index.html <<'EOF'
<!doctype html><html><head><meta charset=utf-8>
<title>GhostLink (shell PoC)</title>
<style>body{font-family:system-ui;background:#0a0d10;color:#e4e8ec;padding:24px}
code,pre{background:#141a20;padding:6px 10px;border-radius:4px;display:block;white-space:pre-wrap}</style>
</head><body>
<h1>GhostLink <small style="color:#4ac0a1">shell PoC</small></h1>
<p>USB gadget + bridge + recorder up. SSH <code>root@172.16.24.1</code>.</p>
<h3>Links</h3>
<ul>
  <li><a href="/cgi-bin/status">/cgi-bin/status</a></li>
  <li><a href="/cgi-bin/config">/cgi-bin/config</a></li>
  <li><a href="/cgi-bin/records">/cgi-bin/records</a></li>
</ul>
</body></html>
EOF
    mkdir -p /tmp/www/cgi-bin
    cat > /tmp/www/cgi-bin/status <<'EOF'
#!/bin/sh
echo "Content-Type: text/plain"
echo
echo "GhostLink shell PoC"
echo "uptime: $(uptime)"
echo
echo "== interfaces =="
ip -br addr
echo
echo "== bridge =="
bridge link 2>/dev/null || ip link show type bridge
echo
echo "== udc =="
ls /sys/class/udc 2>/dev/null
echo
echo "== gadget UDC bound =="
cat /sys/kernel/config/usb_gadget/ghostlink/UDC 2>/dev/null
EOF
    cat > /tmp/www/cgi-bin/config <<EOF
#!/bin/sh
echo "Content-Type: text/plain"
echo
cat $CFG
EOF
    cat > /tmp/www/cgi-bin/records <<EOF
#!/bin/sh
echo "Content-Type: text/plain"
echo
ls -lh $SD/Records 2>/dev/null
EOF
    chmod +x /tmp/www/cgi-bin/*
    httpd -p "${GCFG_WEBUI_PORT:-80}" -h /tmp/www -f &
    log "webui on :${GCFG_WEBUI_PORT:-80}"
fi

# ---------- 9. HID smoke (optional) ----------
if truthy "${GCFG_HID_ENABLED:-TRUE}" && [ -e /dev/hidg0 ]; then
    log "HID keyboard at /dev/hidg0 ready"
fi

log "ghostlink-sh ready"
# Keep the parent alive so service managers don't restart us. Rely on
# individual workers to self-restart.
while :; do sleep 60; done

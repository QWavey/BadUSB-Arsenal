#!/bin/sh
# GhostLink on-device launcher. Runs from /root/ghostlink. No SD required.
# Started by /etc/init.d/S99ghost at boot.

set -u
ROOT=/root/ghostlink
export GL_ROOT=$ROOT
LOG=$ROOT/logs/ghostlink.log
mkdir -p "$ROOT/logs" "$ROOT/records" "$ROOT/verdicts"

log() { echo "$(date +'%H:%M:%S') ghostlink $*" | tee -a "$LOG"; }
log "starting"

# -- config -------------------------------------------------------------------
CFG=$ROOT/config.txt
[ -f "$CFG" ] || { log "no config.txt, bailing"; exit 1; }

getcfg() {
    grep -E "^$1=" "$CFG" | tail -n1 | sed -E "s/^$1=//; s/#.*$//; s/[[:space:]]+$//"
}
truthy() { case "$1" in TRUE|true|1|YES|yes|on|ON) return 0 ;; *) return 1 ;; esac; }

ENABLE_HID=$(getcfg ENABLE_HID)
WEBUI=$(getcfg WEBUI)
DNSMASQ=$(getcfg DNSMASQ)
DNSMASQ_IFACE=$(getcfg DNSMASQ_IFACE)
DHCP_RANGE=$(getcfg DHCP_RANGE)
DNS_UPSTREAM=$(getcfg DNS_UPSTREAM)
REDIRECT=$(getcfg REDIRECT)
REDIRECT_FROM=$(getcfg REDIRECT_FROM)
REDIRECT_TO=$(getcfg REDIRECT_TO)
LOG_FLAG=$(getcfg LOG)

# -- kill any previous instance (idempotent restart) --------------------------
# busybox has no pkill/pgrep, so use ps+grep+kill.
kill_byname() {
    for p in $(ps 2>/dev/null | awk -v n="$1" 'index($0, n) && $0!~"awk" {print $1}'); do
        kill "$p" 2>/dev/null
    done
}
proc_running() {
    ps 2>/dev/null | awk -v n="$1" 'index($0, n) && $0!~"awk" {found=1} END{exit !found}'
}

for mine in sniff.py webui.py redirect302.py ghostlink-dnsmasq; do
    kill_byname "$mine"
done
sleep 0.3

# -- optional: wire up HID (one-time rebind) ----------------------------------
# The stock gadget 'rockchip' has hid.usb0 declared under functions/ but not
# linked into config b.1. Linking it + rebinding gives us /dev/hidg0.
# This drops adb for ~2s. SSH on 172.32.0.93 keeps you reachable.
if truthy "${ENABLE_HID:-FALSE}" && [ ! -e /dev/hidg0 ]; then
    G=/sys/kernel/config/usb_gadget/rockchip
    if [ -d "$G/functions/hid.usb0" ] && [ ! -e "$G/configs/b.1/hid.usb0" ]; then
        log "wiring HID into gadget (adb will drop briefly)"
        UDC=$(cat "$G/UDC")
        echo "" > "$G/UDC"
        ln -s "$G/functions/hid.usb0" "$G/configs/b.1/hid.usb0"
        echo "$UDC" > "$G/UDC"
        sleep 0.5
    fi
fi

# -- dnsmasq (DHCP + optional DNS spoof/redirect) -----------------------------
if truthy "${DNSMASQ:-TRUE}" && command -v dnsmasq >/dev/null 2>&1; then
    DNS_CONF=/tmp/ghostlink.dnsmasq.conf
    IFACE=${DNSMASQ_IFACE:-usb0}
    {
        echo "interface=$IFACE"
        echo "bind-interfaces"
        echo "no-resolv"
        echo "no-hosts"
        echo "dhcp-range=${DHCP_RANGE:-172.32.0.100,172.32.0.120,255.255.0.0,12h}"
        for s in $(echo "${DNS_UPSTREAM:-1.1.1.1}" | tr ',' ' '); do
            echo "server=$s"
        done
        if truthy "${REDIRECT:-FALSE}" && [ -n "${REDIRECT_FROM:-}" ]; then
            TARGET_IP=$(echo "$REDIRECT_TO" | sed -E 's#^https?://##; s#/.*##')
            for h in $(echo "$REDIRECT_FROM" | tr ',' ' '); do
                h=$(echo "$h" | sed -E 's#^https?://##')
                echo "address=/$h/$TARGET_IP"
            done
        fi
        echo "log-facility=$ROOT/logs/dnsmasq.log"
        echo "pid-file=/run/ghostlink-dnsmasq.pid"
        echo "dhcp-leasefile=/tmp/dnsmasq.leases"
    } > "$DNS_CONF"
    dnsmasq -C "$DNS_CONF" -x /run/ghostlink-dnsmasq.pid -z >>"$LOG" 2>&1 &
    log "dnsmasq on $IFACE"
fi

# -- sniffer + verdict stream (Python) ----------------------------------------
if truthy "${LOG_FLAG:-TRUE}"; then
    python3 "$ROOT/sniff.py" >>"$LOG" 2>&1 &
    log "sniff.py started"
fi

# -- webui (Python) -----------------------------------------------------------
if truthy "${WEBUI:-TRUE}"; then
    python3 "$ROOT/webui.py" >>"$LOG" 2>&1 &
    log "webui.py started"
fi

# -- redirect: tiny HTTP 302 on :8080 -----------------------------------------
# dnsmasq already steers A records to our IP; this answers HTTP there.
if truthy "${REDIRECT:-FALSE}" && truthy "$(getcfg REDIRECT_HTTP_302)"; then
    python3 "$ROOT/redirect302.py" >>"$LOG" 2>&1 &
    log "redirect 302 on :8080"
fi

# -- settle message -----------------------------------------------------------
log "up. ssh root@172.32.0.93  |  http://172.32.0.93/"

# -- supervise ----------------------------------------------------------------
# Keep PID 1 of our tree alive. Children self-restart via systemd-light pattern.
while :; do
    sleep 30
    if truthy "${WEBUI:-TRUE}" && ! proc_running webui.py; then
        python3 "$ROOT/webui.py" >>"$LOG" 2>&1 &
        log "webui respawned"
    fi
    if truthy "${LOG_FLAG:-TRUE}" && ! proc_running sniff.py; then
        python3 "$ROOT/sniff.py" >>"$LOG" 2>&1 &
        log "sniff respawned"
    fi
done

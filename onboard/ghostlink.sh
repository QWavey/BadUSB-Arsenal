#!/bin/sh
# GhostLink on-device launcher. Runs from /root/ghostlink. No SD required.
# Started by /etc/init.d/S99ghost at boot.
#
# Subcommands:
#   (none)            start all services
#   reload-dnsmasq    rebuild dnsmasq config from current config.txt and
#                     restart dnsmasq only (used by webui on config save).

set -u
ROOT=/root/ghostlink
export GL_ROOT=$ROOT
LOG=$ROOT/logs/ghostlink.log
mkdir -p "$ROOT/logs" "$ROOT/records" "$ROOT/verdicts"

log() { echo "$(date +'%H:%M:%S') ghostlink $*" | tee -a "$LOG"; }

CFG=$ROOT/config.txt
getcfg() {
    grep -E "^$1=" "$CFG" | tail -n1 | sed -E "s/^$1=//; s/#.*$//; s/[[:space:]]+$//"
}
truthy() { case "$1" in TRUE|true|1|YES|yes|on|ON) return 0 ;; *) return 1 ;; esac; }

write_dnsmasq_conf() {
    IFACE=${1:-usb0}
    DR=${2:-172.32.0.100,172.32.0.120,255.255.0.0,12h}
    DNS=${3:-1.1.1.1,9.9.9.9}
    RED=${4:-FALSE}
    RED_FROM=${5:-}
    RED_TO=${6:-172.32.0.93}
    DNS_CONF=/tmp/ghostlink.dnsmasq.conf
    {
        echo "interface=$IFACE"
        echo "bind-interfaces"
        echo "no-resolv"
        echo "no-hosts"
        echo "dhcp-range=$DR"
        for s in $(echo "$DNS" | tr ',' ' '); do echo "server=$s"; done
        if truthy "$RED" && [ -n "$RED_FROM" ]; then
            TARGET_IP=$(echo "$RED_TO" | sed -E 's#^https?://##; s#/.*##')
            for h in $(echo "$RED_FROM" | tr ',' ' '); do
                h=$(echo "$h" | sed -E 's#^https?://##; s#/.*##')
                echo "address=/$h/$TARGET_IP"
            done
        fi
        # IMPORTANT: log to /tmp (tmpfs), not /root. tmpfs is RAM-backed so
        # it never wears flash and never fills the rootfs. Also keep the
        # file tiny and never request log-queries.
        echo "log-facility=/tmp/dnsmasq.log"
        echo "pid-file=/run/ghostlink-dnsmasq.pid"
        echo "dhcp-leasefile=/tmp/dnsmasq.leases"
    } > $DNS_CONF
    echo $DNS_CONF
}

if [ "${1:-}" = "reload-dnsmasq" ]; then
    [ -f "$CFG" ] || { echo "no config.txt"; exit 1; }
    IFACE=$(getcfg DNSMASQ_IFACE); IFACE=${IFACE:-usb0}
    DR=$(getcfg DHCP_RANGE)
    DNS=$(getcfg DNS_UPSTREAM)
    RED=$(getcfg REDIRECT)
    RF=$(getcfg REDIRECT_FROM)
    RT=$(getcfg REDIRECT_TO)
    write_dnsmasq_conf "$IFACE" "$DR" "$DNS" "$RED" "$RF" "$RT" > /dev/null
    # kill old
    for p in $(ps 2>/dev/null | awk '/dnsmasq.*ghostlink/ && !/awk/ {print $1}'); do
        kill "$p" 2>/dev/null
    done
    sleep 0.3
    dnsmasq -C /tmp/ghostlink.dnsmasq.conf -x /run/ghostlink-dnsmasq.pid -z >>"$LOG" 2>&1 &
    log "dnsmasq reloaded (REDIRECT=$RED REDIRECT_FROM=$RF REDIRECT_TO=$RT)"
    exit 0
fi

log "starting"

# -- config -------------------------------------------------------------------
[ -f "$CFG" ] || { log "no config.txt, bailing"; exit 1; }

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
    IFACE=${DNSMASQ_IFACE:-usb0}
    write_dnsmasq_conf "$IFACE" "$DHCP_RANGE" "$DNS_UPSTREAM" \
                       "${REDIRECT:-FALSE}" "${REDIRECT_FROM:-}" "${REDIRECT_TO:-172.32.0.93}" >/dev/null
    dnsmasq -C /tmp/ghostlink.dnsmasq.conf -x /run/ghostlink-dnsmasq.pid -z >>"$LOG" 2>&1 &
    log "dnsmasq on $IFACE (REDIRECT=${REDIRECT:-FALSE})"
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

# redirect is handled inline by webui.py (Host-header 302 on :80), so the
# separate redirect302.py service is no longer needed. Still ship it for
# anyone who wants a secondary :8080 forwarder:
if truthy "${REDIRECT:-FALSE}" && truthy "$(getcfg REDIRECT_HTTP_302_8080)"; then
    python3 "$ROOT/redirect302.py" >>"$LOG" 2>&1 &
    log "redirect-8080 enabled"
fi

# -- disk guard: independent shell watchdog ----------------------------------
# Belt-and-braces for the Python disk guard. Checks /root free space every
# 2s and nukes records+verdicts if free drops into emergency. Also trims
# any logs that have grown past LOG_MAX_KB. Writes a one-line warning to
# kmsg so the problem is visible even with no console.
(
    LOG_MAX_KB=256
    WARN=15
    CRIT=8
    PANIC=5
    while :; do
        free=$(df -Pm /root 2>/dev/null | awk 'NR==2 {print $4}')
        [ -n "$free" ] || { sleep 2; continue; }
        # trim any oversized log, always (cheap, no stat per file)
        for lf in "$ROOT/logs"/*.log /tmp/dnsmasq.log; do
            [ -f "$lf" ] || continue
            sz=$(stat -c %s "$lf" 2>/dev/null || echo 0)
            if [ "$sz" -gt $((LOG_MAX_KB * 1024 * 2)) ]; then
                tail -c $((LOG_MAX_KB * 1024)) "$lf" > "$lf.tmp" 2>/dev/null && mv "$lf.tmp" "$lf"
            fi
        done
        if [ "$free" -lt "$CRIT" ]; then
            echo "GhostLink: EMERGENCY free=${free}MB, wiping records+verdicts" > /dev/kmsg 2>/dev/null
            rm -f "$ROOT/records"/*.pcap "$ROOT/verdicts"/*.jsonl 2>/dev/null
            # also hard-trim logs
            for lf in "$ROOT/logs"/*.log /tmp/dnsmasq.log; do
                [ -f "$lf" ] || continue
                tail -c $((32 * 1024)) "$lf" > "$lf.tmp" 2>/dev/null && mv "$lf.tmp" "$lf"
            done
        elif [ "$free" -lt "$WARN" ]; then
            # prune oldest half of records+verdicts
            ls -t "$ROOT/records"/rec_*.pcap 2>/dev/null | tail -n +3 | xargs -r rm -f 2>/dev/null
            ls -t "$ROOT/verdicts"/rec_*.jsonl 2>/dev/null | tail -n +6 | xargs -r rm -f 2>/dev/null
        fi
        sleep 2
    done
) &
log "disk guard watchdog online"

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

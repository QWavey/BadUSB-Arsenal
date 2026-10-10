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
            # address= needs a bare IP. We always point at ourselves (the
            # device's usb0 IP) so the browser arrives at :80 here, and
            # webui.py 302s to REDIRECT_TO via Host-header. REDIRECT_TO
            # may be a hostname or URL — that is only used by the 302
            # Location header, not by dnsmasq.
            TARGET_IP=$(ip -4 -o addr show dev usb0 2>/dev/null | awk '{print $4}' | cut -d/ -f1)
            [ -n "$TARGET_IP" ] || TARGET_IP="172.32.0.93"
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

if [ "${1:-}" = "rebind-gadget" ]; then
    # Add hid.usb0 into the stock gadget's config and rebind UDC. Drops adb
    # for ~2s; sshd on 172.32.0.93 stays reachable because RNDIS gets re-
    # bound in the same cycle. Idempotent: no-op if hid.usb0 is already linked
    # and /dev/hidg0 exists.
    G=/sys/kernel/config/usb_gadget/rockchip
    if [ ! -d "$G" ]; then
        log "ERROR: gadget rockchip not found at $G"
        exit 1
    fi
    # discover the active config dir — stock Luckfox uses b.1 but future
    # buildroots may differ. Find the first configs/*/ dir.
    CFG_DIR=$(ls -d $G/configs/*/ 2>/dev/null | head -n1)
    [ -n "$CFG_DIR" ] || { log "ERROR: no gadget config dir under $G/configs/"; exit 1; }

    if [ -e "$CFG_DIR/hid.usb0" ] && [ -e /dev/hidg0 ]; then
        log "rebind-gadget: already up ($CFG_DIR/hid.usb0, /dev/hidg0)"
        exit 0
    fi

    UDC=$(cat "$G/UDC" 2>/dev/null)
    if [ -z "$UDC" ]; then
        # Gadget may already be unbound. Discover first UDC available.
        UDC=$(ls /sys/class/udc 2>/dev/null | head -n1)
    fi
    [ -n "$UDC" ] || { log "ERROR: no UDC available"; exit 1; }
    log "rebind-gadget: unbinding UDC=$UDC"
    echo "" > "$G/UDC" 2>/dev/null

    # Add the symlink if missing
    if [ ! -e "$CFG_DIR/hid.usb0" ]; then
        ln -s "$G/functions/hid.usb0" "$CFG_DIR/hid.usb0" 2>/dev/null \
            || { log "ERROR: symlink hid.usb0 failed"; echo "$UDC" > "$G/UDC" 2>/dev/null; exit 1; }
    fi

    # Rebind
    echo "$UDC" > "$G/UDC" 2>/dev/null
    sleep 1

    # Verify
    if [ -e /dev/hidg0 ]; then
        log "rebind-gadget: /dev/hidg0 is up"
        chmod 666 /dev/hidg0 2>/dev/null
        exit 0
    else
        log "ERROR: rebind returned but /dev/hidg0 missing — rolling back"
        echo "" > "$G/UDC" 2>/dev/null
        rm -f "$CFG_DIR/hid.usb0"
        echo "$UDC" > "$G/UDC" 2>/dev/null
        exit 1
    fi
fi

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
    rm -f /run/ghostlink-dnsmasq.pid
    # -k keeps dnsmasq in foreground; we background with & ourselves so the
    # shell owns the lifetime. daemon mode (-z alone) was failing silently
    # on this busybox init environment.
    setsid nohup dnsmasq -C /tmp/ghostlink.dnsmasq.conf -k >>"$LOG" 2>&1 </dev/null &
    sleep 0.5
    if ps 2>/dev/null | grep -q "[d]nsmasq.*ghostlink"; then
        log "dnsmasq reloaded (REDIRECT=$RED REDIRECT_FROM=$RF REDIRECT_TO=$RT)"
    else
        log "ERROR: dnsmasq failed to start after reload — check /tmp/ghostlink.dnsmasq.conf"
    fi
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

# -- optional: wire up HID (uses the rebind-gadget subcommand) ----------------
# Delegates to the subcommand so webui.py can also trigger it on config save
# without a reboot. Idempotent — no-op if /dev/hidg0 already exists.
if truthy "${ENABLE_HID:-FALSE}" && [ ! -e /dev/hidg0 ]; then
    log "ENABLE_HID=TRUE and /dev/hidg0 missing — calling rebind-gadget"
    sh "$0" rebind-gadget || log "WARN: rebind-gadget failed; HID disabled this boot"
fi

# -- dnsmasq (DHCP + optional DNS spoof/redirect) -----------------------------
if truthy "${DNSMASQ:-TRUE}" && command -v dnsmasq >/dev/null 2>&1; then
    IFACE=${DNSMASQ_IFACE:-usb0}
    write_dnsmasq_conf "$IFACE" "$DHCP_RANGE" "$DNS_UPSTREAM" \
                       "${REDIRECT:-FALSE}" "${REDIRECT_FROM:-}" "${REDIRECT_TO:-172.32.0.93}" >/dev/null
    rm -f /run/ghostlink-dnsmasq.pid
    setsid nohup dnsmasq -C /tmp/ghostlink.dnsmasq.conf -k >>"$LOG" 2>&1 </dev/null &
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

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
    # The second sed only strips comments preceded by whitespace, so
    # values containing a lone '#' (passwords, URL fragments) survive.
    grep -E "^$1=" "$CFG" | tail -n1 | sed -E "s/^$1=//; s/[[:space:]]+#.*$//; s/[[:space:]]+$//"
}
truthy() { case "$1" in TRUE|true|1|YES|yes|on|ON) return 0 ;; *) return 1 ;; esac; }

# A PC connected over usb0 keeps whatever DHCP lease it already holds until it chooses to
# renew — it has no way to know the device's own subnet/NET_MODE changed underneath it. A
# brief admin down/up on usb0 (plain `ip link`, nothing touching the USB gadget/UDC — that
# is a separate function from ffs.adb, so this never risks adb) is seen by the host as a
# normal link flap and makes most DHCP clients re-request immediately, same as a cable
# unplug/replug. ~8s down was needed in testing; a couple of seconds was not enough for the
# host to notice. Always backgrounded: never block boot or a config reload on this.
usb0_fresh_lease_bounce() {
    (
        ip link set usb0 down 2>/dev/null
        sleep 8
        ip link set usb0 up 2>/dev/null
        log "usb0: bounced link to make the PC re-DHCP onto the current subnet"
    ) &
}

# Where records and verdicts are written. Both directories under ROOT are symlinks: they point at
# the flash copy, or at the SD card when RECORD_SD=TRUE and the card is mounted. The sniffer and the
# web UI always use the same paths, so only the link changes. A real directory left from before is
# moved into the flash copy first.
apply_record_dir() {
    FLASH="$ROOT/store/flash"
    target="$FLASH"
    rs=$(getcfg RECORD_SD); rs=${rs:-TRUE}   # on by default: the card is used when it is there
    if truthy "$rs" && grep -q " /mnt/sdcard " /proc/mounts 2>/dev/null; then
        if mkdir -p /mnt/sdcard/ghostlink/records /mnt/sdcard/ghostlink/verdicts 2>/dev/null; then
            target=/mnt/sdcard/ghostlink
        else
            log "WARN: SD card is not writable; records stay on flash"
        fi
    fi
    mkdir -p "$FLASH/records" "$FLASH/verdicts"
    for n in records verdicts; do
        if [ -d "$ROOT/$n" ] && [ ! -L "$ROOT/$n" ]; then
            mv "$ROOT/$n"/* "$FLASH/$n"/ 2>/dev/null
            if ! rmdir "$ROOT/$n" 2>/dev/null; then
                log "ERROR: $ROOT/$n is not empty; not linked"; continue
            fi
        fi
        ln -sfn "$target/$n" "$ROOT/$n"
    done
    log "records go to $target"
}

# -- NAT (NET_MODE=NAT): the PC uses this device as its gateway ------------------
# natd.py translates the PC's traffic and sends it out of eth0 from NAT_IP: a spare address on the
# LAN that the device's kernel does not own, so the kernel drops anything sent to it. eth0 itself
# gets its address and default route from the LAN by DHCP (udhcpc), for the device's own traffic.
start_nat() {
    if ! ip -4 -o addr show dev eth0 2>/dev/null | grep -q "inet "; then
        proc_running udhcpc || udhcpc -i eth0 -b -q -t 5 -T 3 >>"$LOG" 2>&1
        return 1
    fi
    WAN_IP=$(ip -4 -o addr show dev eth0 2>/dev/null | awk '{print $4}' | cut -d/ -f1 | head -n1)
    GW=$(ip route show default dev eth0 2>/dev/null | awk '{print $3}' | head -n1)
    if [ -z "$WAN_IP" ] || [ -z "$GW" ]; then
        return 1
    fi
    BASE=$(echo "$WAN_IP" | cut -d. -f1-3)
    NAT_IP="$BASE.250"
    [ "$NAT_IP" = "$WAN_IP" ] && NAT_IP="$BASE.249"
    PC_NET=$(ip route show dev usb0 2>/dev/null | awk '/scope link/ {print $1; exit}')
    PC_NET=${PC_NET:-172.32.0.0/16}
    python3 "$ROOT/natd.py" usb0 eth0 "$NAT_IP" "$GW" "$PC_NET" >>"$LOG" 2>&1 &
    echo $! > /run/ghostlink-natd.pid
    log "NAT: usb0 -> eth0 as $NAT_IP via $GW (PC net $PC_NET)"
}

stop_nat() {
    kill_byname "python3 $ROOT/natd.py"
    rm -f /run/ghostlink-natd.pid
    log "NAT stopped"
}

start_https() {
    # Start HTTPS interception server for REDIRECT_ALL (must run with elevated privileges for port 443)
    if [ "$(getcfg REDIRECT_ALL)" != "TRUE" ]; then
        return
    fi
    if [ -z "$(getcfg REDIRECT_ALL_TO)" ]; then
        log "HTTPS: REDIRECT_ALL_TO not configured"
        return
    fi
    python3 "$ROOT/https_server.py" "$ROOT" 0.0.0.0 >>"$LOG" 2>&1 &
    echo $! > /run/ghostlink-https.pid
    log "HTTPS interception server started"
}

stop_https() {
    kill_byname "python3 $ROOT/https_server.py"
    rm -f /run/ghostlink-https.pid
    log "HTTPS server stopped"
}

# -- LAN auto-detect: reach the device at the PC's real LAN address -----------------
# With the bridge up, a PC on usb0 takes its DHCP lease from the real LAN, so the device's
# usb0 address (172.32.0.93) is out of its reach. lan_detect.py listens to the DHCP replies
# crossing eth0 and writes the LAN's subnet and a free address to /tmp/ghostlink.lan.json.
# detect_and_bind_lan_ip() adds that address to eth0 (the DHCP one stays) and reloads dnsmasq
# so the REDIRECT_ALL catch-all points at it. Best effort: a failure is logged, never fatal.
LAN_STATE=/tmp/ghostlink.lan.json

# One string field of LAN_STATE, empty when missing. lan_detect.py writes indented JSON, one
# field per line. The file is built from LAN traffic, so each value is validated before use.
lan_state_get() {
    sed -nE "s/.*\"$1\"[[:space:]]*:[[:space:]]*\"([^\"]*)\".*/\1/p" "$LAN_STATE" 2>/dev/null | head -n1
}

# Prints the suggested address when the state file says detected and the value is a plain
# dotted-quad IPv4 address. Returns 1 otherwise.
lan_state_ip() {
    local ip
    [ -f "$LAN_STATE" ] || return 1
    grep -Eq '"detected"[[:space:]]*:[[:space:]]*true' "$LAN_STATE" 2>/dev/null || return 1
    ip=$(lan_state_get suggested_ip)
    printf '%s\n' "$ip" | awk -F '[.]' '/^[0-9]+\.[0-9]+\.[0-9]+\.[0-9]+$/ && $1<=255 && $2<=255 && $3<=255 && $4<=255 {ok=1} END {exit !ok}' || return 1
    printf '%s\n' "$ip"
}

# True when eth0 currently holds the address $1.
eth0_has_ip() {
    ip -4 -o addr show dev eth0 2>/dev/null | awk '{print $4}' | cut -d/ -f1 | grep -qxF "$1"
}

# Starts the detection in the background and returns at once, so boot never waits on it.
# Skipped without the LAN cable, and in NAT mode: there the PC already sits on the device's own
# 172.32 subnet, and natd keeps BASE.250 on eth0 as its NAT_IP, which the kernel must not own.
# There is no "eth0 has an IP" test on purpose: only NAT runs udhcpc on eth0, so in bridge mode
# eth0 has no address of its own. The carrier is what the DHCP sniff needs.
#
# One attempt only catches a PC that DHCPDISCOVERs inside the window (fresh plug-in, or a lease
# renewal, which can be hours away). So this retries several short listens spread over a few
# minutes after boot/link-up, instead of one 12 s shot: still a best-effort background task, never
# blocking boot, and it gives up for good once bound or once the retry budget is spent.
detect_and_bind_lan_ip() {
    # GL_ETH0_CARRIER_PATH lets a test point this at a fake carrier file instead of the
    # real sysfs path; unset (the only way this runs in production), it's exactly the
    # literal path this always read before.
    [ "$(cat "${GL_ETH0_CARRIER_PATH:-/sys/class/net/eth0/carrier}" 2>/dev/null)" = "1" ] || return 0
    [ "${NET_MODE:-PASSIVE}" != "NAT" ] || return 0
    [ -f "$ROOT/lan_detect.py" ] || return 0
    (
        attempt=1
        max_attempts=6          # ~6 * (12s listen + 20s gap) =~ 3.5 min of retrying, then give up
        while [ "$attempt" -le "$max_attempts" ]; do
            timeout 15 python3 "$ROOT/lan_detect.py" eth0 12 >/tmp/lan_detect.out 2>&1
            rc=$?
            new_ip=""
            if [ "$rc" -eq 0 ]; then new_ip=$(lan_state_ip) || new_ip=""; fi
            if [ -n "$new_ip" ]; then
                subnet=$(lan_state_get subnet)
                if ! printf '%s\n' "$subnet" | grep -Eq '^[0-9.]+/([1-9]|[12][0-9]|3[0-2])$'; then
                    log "LAN auto-detect: detector result has no usable subnet"
                elif ip addr add "$new_ip/${subnet#*/}" dev eth0 2>/dev/null || eth0_has_ip "$new_ip"; then
                    log "LAN auto-detect: bound $new_ip on eth0 (detected LAN $subnet, attempt $attempt/$max_attempts)"
                    if [ "$(getcfg WEBUI_PASS)" = "" ] || [ "$(getcfg WEBUI_PASS)" = "ghostlink" ]; then
                        log "LAN auto-detect: WARNING webui is now LAN-reachable at $new_ip with the default WEBUI_PASS — change it in config.txt"
                    fi
                    # write_dnsmasq_conf reads the state file, so reload to move the catch-all onto it.
                    if [ "$(getcfg REDIRECT_ALL)" = "TRUE" ]; then
                        sh "$0" reload-dnsmasq >>"$LOG" 2>&1 || log "LAN auto-detect: dnsmasq reload failed"
                    fi
                    return 0
                else
                    log "LAN auto-detect: could not bind $new_ip on eth0"
                fi
                return 0
            fi
            attempt=$((attempt + 1))
            [ "$attempt" -le "$max_attempts" ] && sleep 20
        done
        log "LAN auto-detect: no DHCP traffic observed on eth0 after $max_attempts attempts; giving up until next boot/reload"
    ) &
}

write_dnsmasq_conf() {
    IFACE=${1:-usb0}
    DR=${2:-172.16.24.10,172.16.24.60,255.255.255.0,12h}
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
        # NAT mode: the PC uses this device as its gateway as well as its DNS server.
        if [ "$(getcfg NET_MODE)" = "NAT" ]; then
            GWIP=$(ip -4 -o addr show dev "$IFACE" 2>/dev/null | awk '{print $4}' | cut -d/ -f1 | head -n1)
            [ -n "$GWIP" ] && echo "dhcp-option=option:router,$GWIP"
        fi
        for s in $(echo "$DNS" | tr ',' ' '); do echo "server=$s"; done
        if truthy "$RED" && [ -n "$RED_FROM" ]; then
            # address= needs a bare IP. When HONEYPOT=TRUE we point trapped
            # hosts at the honeypot's dedicated secondary IP so the admin
            # dashboard on .93 stays untouched. Otherwise point at the
            # device's usb0 IP (classic 302 redirect flow).
            HP_ON=$(getcfg HONEYPOT)
            HP_IP=$(getcfg HONEYPOT_IP)
            if truthy "$HP_ON" && [ -n "$HP_IP" ]; then
                TARGET_IP="$HP_IP"
            else
                TARGET_IP=$(ip -4 -o addr show dev "$IFACE" 2>/dev/null | awk '{print $4}' | cut -d/ -f1 | head -n1)
                [ -n "$TARGET_IP" ] || TARGET_IP="172.32.0.93"
            fi
            for h in $(echo "$RED_FROM" | tr ',' ' '); do
                # *.example.com covers the same names as example.com: address= matches subdomains too
                h=$(echo "$h" | sed -E 's#^https?://##; s#^\*\.##; s#/.*##')
                # Only hostnames get an address= line. Tokens such as "#" or
                # "(comma-sep)" left by an older config edit would reach dnsmasq.
                case "$h" in ''|*[!A-Za-z0-9.-]*) continue ;; esac
                echo "address=/$h/$TARGET_IP"
            done
            # Also trap WEBSITE_VISIT=<host> REDIRECT_HONEYPOT rules
            if truthy "$HP_ON"; then
                HP_RULES=$(getcfg HONEYPOT_RULES)
                echo "$HP_RULES" | tr ';' '\n' | while IFS= read -r line; do
                    # Match "ON? WEBSITE_VISIT[= ]host REDIRECT_HONEYPOT"
                    h=$(echo "$line" | sed -nE 's/^[[:space:]]*(ON[[:space:]]+)?WEBSITE_VISIT[[:space:]]*=?[[:space:]]*([^[:space:]]+)[[:space:]]+REDIRECT_HONEYPOT[[:space:]]*$/\2/Ip')
                    [ -n "$h" ] && echo "address=/$h/$TARGET_IP"
                done
            fi
        fi
        # -- redirect-all (captive-portal style) --------------------------------
        # REDIRECT_ALL=TRUE answers every name with this device
        # (address=/#/<device address>) except an allowlist that keeps resolving
        # upstream. Each exempt name and the target's own host get explicit
        # server=/name/ lines: dnsmasq matches a named domain more specifically
        # than the "#" catch-all, so those go to the real upstream while
        # everything else is caught. The catch-all line is emitted last. This is
        # independent of REDIRECT above (the per-URL spoof), which is kept as-is.
        RED_ALL=$(getcfg REDIRECT_ALL)
        RA_TO=$(getcfg REDIRECT_ALL_TO)
        # Same host extraction as REDIRECT_FROM: strip scheme, leading "*.", path.
        RA_HOST=$(echo "$RA_TO" | sed -E 's#^https?://##; s#^\*\.##; s#/.*##')
        if truthy "${RED_ALL:-FALSE}" && [ -n "$RA_HOST" ]; then
            # Catch-all target: the LAN address detect_and_bind_lan_ip bound on eth0, once it has.
            # The candidate comes from a file built from LAN traffic, so it only counts when eth0
            # holds exactly that address (fixed-string match). Otherwise it stays the usb0 address.
            CATCH_IP=172.32.0.93
            LAN_CAND=$(sed -nE 's/.*"suggested_ip"[[:space:]]*:[[:space:]]*"([^"]*)".*/\1/p' "${LAN_STATE:-/tmp/ghostlink.lan.json}" 2>/dev/null | head -n1)
            if [ -n "$LAN_CAND" ] && grep -Eq '"detected"[[:space:]]*:[[:space:]]*true' "${LAN_STATE:-/tmp/ghostlink.lan.json}" 2>/dev/null \
               && ip -4 -o addr show dev eth0 2>/dev/null | awk '{print $4}' | cut -d/ -f1 | grep -qxF "$LAN_CAND"; then
                CATCH_IP=$LAN_CAND
            fi
            echo "# mode: REDIRECT_ALL (catch-all -> $CATCH_IP, allowlist upstream)"
            EXEMPT=$(getcfg REDIRECT_EXEMPT)
            # Default allowlist when the key is empty or missing.
            [ -n "$EXEMPT" ] || EXEMPT="anthropic.com,claude.ai,claude.com,claudeusercontent.com,msftconnecttest.com,msftncsi.com,windowsupdate.com"
            # Exempt names (and the redirect target's host) resolve normally so
            # the client can still reach them; 1.1.1.1/9.9.9.9 match DNS_UPSTREAM.
            for d in $(echo "$EXEMPT,$RA_HOST" | tr ',' ' '); do
                d=$(echo "$d" | sed -E 's#^https?://##; s#^\*\.##; s#/.*##')
                # Same sanitising as the REDIRECT_FROM loop: drop empties/junk.
                case "$d" in ''|*[!A-Za-z0-9.-]*) continue ;; esac
                echo "server=/$d/1.1.1.1"
                echo "server=/$d/9.9.9.9"
            done
            # dnsmasq 2.85 on this device has no filter-aaaa/filter-AAAA option (confirmed:
            # it rejects the config outright with "bad option"), so an IPv6-capable PC would
            # otherwise get a real AAAA answer from upstream and skip the device entirely.
            # address=/domain/ip is supported since very old dnsmasq and takes either an IPv4
            # or an IPv6 literal for the same domain, so answering "::" (the IPv6 unspecified
            # address) for every AAAA query achieves the same practical effect as filter-aaaa:
            # the client's connection to :: fails immediately and falls back to the IPv4
            # address below, instead of silently using a real IPv6 path around the device.
            echo "address=/#/::"
            # Everything not exempt answers with the device. Keep this line last.
            echo "address=/#/$CATCH_IP"
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

if [ "${1:-}" = "apply-record-dir" ]; then
    apply_record_dir
    exit 0
fi

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
    RA=$(getcfg REDIRECT_ALL)
    DN=$(getcfg DNSMASQ)
    # DNSMASQ off means stop it, not start it.
    if ! truthy "${DN:-TRUE}"; then
        for p in $(ps 2>/dev/null | awk '/dnsmasq.*ghostlink/ && !/awk/ {print $1}'); do
            kill "$p" 2>/dev/null
        done
        log "dnsmasq disabled, stopped"
        exit 0
    fi
    command -v dnsmasq >/dev/null 2>&1 || { log "ERROR: dnsmasq not installed"; exit 1; }
    # Keep the working config aside: a rejected candidate must not stay in place,
    # because the supervisor respawns dnsmasq from that file.
    cp /tmp/ghostlink.dnsmasq.conf /tmp/ghostlink.dnsmasq.prev 2>/dev/null
    OLD_DHCP_RANGE=$(grep '^dhcp-range=' /tmp/ghostlink.dnsmasq.prev 2>/dev/null | head -n1)
    write_dnsmasq_conf "$IFACE" "$DR" "$DNS" "$RED" "$RF" "$RT" > /dev/null
    # Test the new config before touching the running server, so a bad
    # DHCP_RANGE or DNS line keeps the working dnsmasq up.
    if ! dnsmasq --test -C /tmp/ghostlink.dnsmasq.conf >>"$LOG" 2>&1; then
        [ -f /tmp/ghostlink.dnsmasq.prev ] && mv /tmp/ghostlink.dnsmasq.prev /tmp/ghostlink.dnsmasq.conf
        log "ERROR: new dnsmasq config rejected; old server kept running"
        exit 1
    fi
    # Hold off the supervisor's respawn for the length of this run: it would race
    # us for port 53. The flag holds our pid, so a flag left behind is detected.
    RELOAD_FLAG=/run/ghostlink-dnsmasq.reloading
    echo $$ > "$RELOAD_FLAG"
    trap 'rm -f "$RELOAD_FLAG"' EXIT
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
    # Success means the instance started above wrote its pidfile and is alive.
    # Matching any dnsmasq reported success even when this start had failed.
    NEW=""
    for _ in 1 2 3 4 5 6 7 8 9 10; do
        sleep 0.3
        NEW=$(cat /run/ghostlink-dnsmasq.pid 2>/dev/null)
        if [ -n "$NEW" ] && grep -q dnsmasq "/proc/$NEW/cmdline" 2>/dev/null; then break; fi
        NEW=""
    done
    if [ -n "$NEW" ]; then
        log "dnsmasq reloaded (REDIRECT=$RED REDIRECT_FROM=$RF REDIRECT_TO=$RT REDIRECT_ALL=${RA:-FALSE})"
        # A PC already attached to usb0 only needs to re-DHCP when the subnet it would be
        # offered actually changed (e.g. a NET_MODE switch, or DHCP_RANGE itself edited) —
        # not on every trivial config save (REDIRECT_FROM/TO, REDIRECT toggle, etc.), which
        # would otherwise delay the save by the bounce's ~8s for no reason.
        NEW_DHCP_RANGE=$(grep '^dhcp-range=' /tmp/ghostlink.dnsmasq.conf 2>/dev/null | head -n1)
        if [ "$IFACE" = "usb0" ] && [ -n "$OLD_DHCP_RANGE" ] && [ "$OLD_DHCP_RANGE" != "$NEW_DHCP_RANGE" ]; then
            log "usb0 DHCP range changed ($OLD_DHCP_RANGE -> $NEW_DHCP_RANGE); bouncing link"
            usb0_fresh_lease_bounce
        fi
        rm -f /tmp/ghostlink.dnsmasq.prev

        # Also manage HTTPS server state based on REDIRECT_ALL
        if [ "$RA" = "TRUE" ]; then
            if ! pidfile_running /run/ghostlink-https.pid "https_server.py"; then
                start_https
            fi
        else
            if pidfile_running /run/ghostlink-https.pid "https_server.py"; then
                stop_https
            fi
        fi
    else
        # Put the working config back: the supervisor respawns dnsmasq from this file
        [ -f /tmp/ghostlink.dnsmasq.prev ] && mv /tmp/ghostlink.dnsmasq.prev /tmp/ghostlink.dnsmasq.conf
        log "ERROR: dnsmasq failed to start after reload — check /tmp/ghostlink.dnsmasq.conf"
        exit 1
    fi
    exit 0
fi

log "starting"

# -- single-instance guard ----------------------------------------------------
# Prevents a double-start from spawning two webuis / two sniffers / two
# dnsmasqs on the same ports (second webui crashes with EADDRINUSE 98 and
# dumps a traceback into the log). If a previous ghostlink.sh died without
# cleaning its pidfile, the kill -0 check lets us claim the slot anyway.
PIDFILE=/run/ghostlink.pid
if [ -f "$PIDFILE" ]; then
    oldpid=$(cat "$PIDFILE" 2>/dev/null)
    # A reused pid is not a reason to refuse every later start: check that it is really ghostlink.
    if [ -n "$oldpid" ] && [ "$oldpid" != "$$" ] && kill -0 "$oldpid" 2>/dev/null \
       && grep -q ghostlink "/proc/$oldpid/cmdline" 2>/dev/null; then
        log "already running as pid $oldpid, exiting"
        exit 0
    fi
fi
echo $$ > "$PIDFILE"
trap 'rm -f "$PIDFILE"' EXIT
trap 'exit 0' INT TERM

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

pidfile_running() {
    # Check if a pidfile's process is alive (avoids ps overhead for single-instance daemons).
    # Usage: pidfile_running /run/ghostlink-forwarder.pid command_name
    local pidfile="$1" name="$2"
    [ -f "$pidfile" ] || return 1
    local pid; pid=$(cat "$pidfile" 2>/dev/null) || return 1
    [ -n "$pid" ] || return 1
    # Check if pid is in /proc and its command matches the expected name.
    if [ -f "/proc/$pid/cmdline" ]; then
        grep -q "$name" "/proc/$pid/cmdline" 2>/dev/null && return 0
    fi
    return 1
}

for mine in sniff.py webui.py redirect302.py button.py forwarder.py ghostlink.dnsmasq; do
    kill_byname "$mine"
done
sleep 0.3

# True while a reload-dnsmasq run is in progress. The flag holds that run's pid;
# a flag whose process is gone is stale, so it is removed and ignored.
reload_in_progress() {
    f=/run/ghostlink-dnsmasq.reloading
    [ -f "$f" ] || return 1
    p=$(cat "$f" 2>/dev/null)
    if [ -n "$p" ] && [ -d "/proc/$p" ]; then return 0; fi
    rm -f "$f"
    return 1
}

# -- HID gadget rebind — DISABLED by default ----------------------------------
# On the Luckfox stock gadget, writing to UDC tears down ffs.adb (FunctionFS
# backed by adbd); adbd doesn't survive it. The whole composite gadget then
# fails to come back up and the device appears bricked over USB. Rebind
# only when the user explicitly accepts the risk via HID_FORCE_REBIND=TRUE
# AND has a recovery channel (serial console or a safe adbd-orchestration
# script). See docs/HID.md.
if truthy "${ENABLE_HID:-FALSE}" && \
   truthy "$(getcfg HID_FORCE_REBIND)" && \
   [ ! -e /dev/hidg0 ]; then
    log "HID_FORCE_REBIND=TRUE accepted — calling rebind-gadget (RISKY)"
    sh "$0" rebind-gadget || log "WARN: rebind-gadget failed; HID disabled this boot"
elif truthy "${ENABLE_HID:-FALSE}" && [ ! -e /dev/hidg0 ]; then
    log "ENABLE_HID=TRUE but HID_FORCE_REBIND != TRUE — skipping rebind (safe default)"
fi

# -- dnsmasq (DHCP + optional DNS spoof/redirect) -----------------------------
if truthy "${DNSMASQ:-TRUE}" && command -v dnsmasq >/dev/null 2>&1; then
    IFACE=${DNSMASQ_IFACE:-usb0}
    write_dnsmasq_conf "$IFACE" "$DHCP_RANGE" "$DNS_UPSTREAM" \
                       "${REDIRECT:-FALSE}" "${REDIRECT_FROM:-}" "${REDIRECT_TO:-172.32.0.93}" >/dev/null
    # A bad value in config.txt must not become a config the supervisor respawns forever:
    # test it first, and fall back to the default DHCP range and DNS if dnsmasq rejects it.
    if ! dnsmasq --test -C /tmp/ghostlink.dnsmasq.conf >>"$LOG" 2>&1; then
        log "ERROR: dnsmasq config rejected at boot; using the default DHCP range and DNS"
        write_dnsmasq_conf "$IFACE" "172.32.0.100,172.32.0.120,255.255.0.0,12h" "1.1.1.1,9.9.9.9" \
                           "FALSE" "" "" >/dev/null
    fi
    rm -f /run/ghostlink-dnsmasq.pid
    setsid nohup dnsmasq -C /tmp/ghostlink.dnsmasq.conf -k >>"$LOG" 2>&1 </dev/null &
    log "dnsmasq on $IFACE (REDIRECT=${REDIRECT:-FALSE})"
    # A full boot is infrequent and deliberate, so always make sure a PC already
    # attached to usb0 gets a fresh lease on whatever subnet this boot came up with,
    # rather than possibly reusing a lease left over from a previous boot/mode.
    [ "$IFACE" = "usb0" ] && usb0_fresh_lease_bounce
fi

# -- sniffer + verdict stream (Python) ----------------------------------------
apply_record_dir
if truthy "${LOG_FLAG:-TRUE}"; then
    python3 "$ROOT/sniff.py" >>"$LOG" 2>&1 &
    log "sniff.py started"
fi

# -- webui (Python) -----------------------------------------------------------
if truthy "${WEBUI:-TRUE}"; then
    python3 "$ROOT/webui.py" >>"$LOG" 2>&1 &
    log "webui.py started"
fi

# -- userspace L2 forwarder (NET_MODE=FORWARD) --------------------------------
# Kernel bridge isn't supported on the stock Luckfox kernel, so a true inline
# MITM between usb0 and eth0 goes through a Python daemon that cross-copies
# AF_PACKET frames. It runs when eth0 has carrier (a LAN cable is plugged in)
# and either NET_MODE=FORWARD or AUTO_FORWARD is on (on by default). The
# link-watch loop further down keeps this in sync when the cable is unplugged.
NET_MODE=$(getcfg NET_MODE)
NET_MODE=${NET_MODE:-PASSIVE}
AUTO_FORWARD=$(getcfg AUTO_FORWARD)
AUTO_FORWARD=${AUTO_FORWARD:-TRUE}
if [ "$NET_MODE" = "NAT" ]; then
    # NAT replaces the bridge: the PC's traffic is translated, not copied onto the LAN.
    [ "$(cat /sys/class/net/eth0/carrier 2>/dev/null)" = "1" ] && { start_nat || true; }
elif { [ "$NET_MODE" = "FORWARD" ] || truthy "$AUTO_FORWARD"; } && [ "$(cat /sys/class/net/eth0/carrier 2>/dev/null)" = "1" ]; then
    ip link set eth0 up 2>/dev/null
    ip link set eth0 promisc on 2>/dev/null
    python3 "$ROOT/forwarder.py" eth0 usb0 >>"$LOG" 2>&1 &
    log "forwarder.py started (eth0 <-> usb0)"
fi

# LAN auto-detect: starts in the background and returns at once (see detect_and_bind_lan_ip).
{ detect_and_bind_lan_ip || true; }

# Start HTTPS interception server for REDIRECT_ALL
[ "$(getcfg REDIRECT_ALL)" = "TRUE" ] && { start_https || true; }

# -- link-state watcher: keeps the forwarder matching NET_MODE + eth0 carrier -
# Every 2 seconds the forwarder is made to match the state: running when a LAN
# cable is plugged in and NET_MODE=FORWARD or AUTO_FORWARD is on. It checks on every poll, not
# only when the carrier changes, so choosing FORWARD while the cable is already
# in starts the forwarder too. If it keeps dying, it is retried at most every
# 10 seconds so the log doesn't fill up. We can't trust `ip monitor link` on
# busybox, so this is a poll loop.
(
    prev_carrier="-"
    next_try=0
    while :; do
        sleep 2
        cur_carrier=$(cat /sys/class/net/eth0/carrier 2>/dev/null)
        if [ "$cur_carrier" != "$prev_carrier" ]; then
            log "eth0 carrier now ${cur_carrier:-unknown}"
            prev_carrier="$cur_carrier"
        fi
        mode=$(getcfg NET_MODE); mode=${mode:-PASSIVE}
        auto=$(getcfg AUTO_FORWARD); auto=${auto:-TRUE}
        if [ "$mode" = "NAT" ]; then
            # NAT mode: translate while the LAN cable is in, and never run the bridge with it.
            if pidfile_running /run/ghostlink-forwarder.pid "forwarder.py"; then
                kill_byname "python3 $ROOT/forwarder.py"
            fi
            if [ "$cur_carrier" = "1" ]; then
                pidfile_running /run/ghostlink-natd.pid "natd.py" || start_nat
            elif pidfile_running /run/ghostlink-natd.pid "natd.py"; then
                stop_nat
            fi
        elif { [ "$mode" = "FORWARD" ] || truthy "$auto"; } && [ "$cur_carrier" = "1" ]; then
            if ! pidfile_running /run/ghostlink-forwarder.pid "forwarder.py"; then
                now=$(date +%s)
                if [ "$now" -ge "$next_try" ]; then
                    next_try=$((now + 10))
                    ip link set eth0 up 2>/dev/null
                    ip link set eth0 promisc on 2>/dev/null
                    python3 "$ROOT/forwarder.py" eth0 usb0 >>"$LOG" 2>&1 &
                    log "forwarder.py started (mode=$mode auto=$auto carrier=$cur_carrier)"
                    sleep 1
                    if ! pidfile_running /run/ghostlink-forwarder.pid "forwarder.py"; then
                        log "forwarder.py exited at once: $(cat /run/ghostlink-forwarder.status 2>/dev/null)"
                    fi
                fi
            fi
        else
            if pidfile_running /run/ghostlink-natd.pid "natd.py"; then
                stop_nat
            fi
            if pidfile_running /run/ghostlink-forwarder.pid "forwarder.py"; then
                kill_byname "python3 $ROOT/forwarder.py"
                log "forwarder.py stopped (carrier=$cur_carrier mode=$mode auto=$auto)"
            fi
        fi
    done
) &
log "link watcher online"

# -- button daemon ------------------------------------------------------------
# Watches the on-board button (adc-keys driver). On press, fires the payload
# named by BUTTON_PAYLOAD / BUTTON_DOUBLE_TAP_PAYLOAD / BUTTON_HOLD_PAYLOAD
# from config.txt. Separate from the DuckyScript WAIT_FOR_BUTTON_PRESS
# command (which reads /dev/input/event0 directly from within a payload).
if truthy "$(getcfg BUTTON_ENABLED)"; then
    python3 "$ROOT/button.py" >>"$LOG" 2>&1 &
    log "button.py started"
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
# 4s (disk doesn't change fast enough for 2s polling) and nukes records+verdicts
# if free drops into emergency. Also trims any logs that have grown past LOG_MAX_KB.
# Writes a one-line warning to kmsg so the problem is visible even with no console.
(
    LOG_MAX_KB=256
    WARN=15
    CRIT=8
    PANIC=5
    log_check_counter=0
    while :; do
        free=$(timeout 2 df -Pm /root 2>/dev/null | awk 'NR==2 {print $4}')
        [ -n "$free" ] || { sleep 4; continue; }
        # Only check log sizes every other poll (every 8s) to reduce stat overhead.
        log_check_counter=$((log_check_counter + 1))
        if [ $((log_check_counter % 2)) -eq 0 ]; then
            for lf in "$ROOT/logs"/*.log /tmp/dnsmasq.log; do
                [ -f "$lf" ] || continue
                sz=$(stat -c %s "$lf" 2>/dev/null || echo 0)
                if [ "$sz" -gt $((LOG_MAX_KB * 1024 * 2)) ]; then
                    tail -c $((LOG_MAX_KB * 1024)) "$lf" > "$lf.tmp" 2>/dev/null && mv "$lf.tmp" "$lf"
                fi
            done
        fi
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
        sleep 4
    done
) &
log "disk guard watchdog online"

# -- settle message -----------------------------------------------------------
log "up. ssh root@172.32.0.93  |  http://172.32.0.93/"

# -- supervise ----------------------------------------------------------------
# Keep PID 1 of our tree alive. Children self-restart via systemd-light pattern.
while :; do
    sleep 30
    # dnsmasq serves DHCP and every spoofed name. If it dies, nothing else
    # notices, so it is respawned here like the other daemons.
    DN_NOW=$(getcfg DNSMASQ)  # read again: the web UI can switch dnsmasq off after boot
    if truthy "${DN_NOW:-TRUE}" && command -v dnsmasq >/dev/null 2>&1 \
       && [ -f /tmp/ghostlink.dnsmasq.conf ] && ! pidfile_running /run/ghostlink-dnsmasq.pid dnsmasq \
       && ! reload_in_progress; then
        setsid nohup dnsmasq -C /tmp/ghostlink.dnsmasq.conf -k >>"$LOG" 2>&1 </dev/null &
        log "dnsmasq respawned"
    fi
    if truthy "${WEBUI:-TRUE}" && ! proc_running webui.py; then
        python3 "$ROOT/webui.py" >>"$LOG" 2>&1 &
        log "webui respawned"
    fi
    if truthy "${LOG_FLAG:-TRUE}" && ! proc_running sniff.py; then
        python3 "$ROOT/sniff.py" >>"$LOG" 2>&1 &
        log "sniff respawned"
    fi
    if truthy "$(getcfg BUTTON_ENABLED)" && ! proc_running button.py; then
        python3 "$ROOT/button.py" >>"$LOG" 2>&1 &
        log "button respawned"
    fi
done

/*
 * Bridge setup. Four modes:
 *
 *   BRIDGE      - br0 = eth0 + usb0, no IP on br0 by default; the WAN side
 *                 learns DHCP from the real LAN. This is the pure-passthrough
 *                 behaviour that keeps us invisible on wire.
 *   NAT         - usb0 gets GATEWAY_IP, dnsmasq hands DHCP to the host, eth0
 *                 is upstream, nftables NATs the subnet out. The LAN sees
 *                 only this device, host-side thinks it's online normally.
 *   TRANSPARENT - no IP anywhere, bridge is up for sniffing. Deadnet works,
 *                 everything that needs an address does not.
 *   ISOLATE     - no forwarding. Host-only use (admin via usb0, no WAN).
 *
 * We use iproute2 commands rather than libnl, because the latter would pull
 * ~80KB extra into our binary and we have one-shot setup, not frequent churn.
 */

#include "bridge.h"
#include "../core/config.h"
#include "../common/log.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>
#include <unistd.h>
#include <fcntl.h>
#include <sys/stat.h>

static int sys(const char *cmd) {
    LOGD("bridge", "$ %s", cmd);
    int rc = system(cmd);
    if (rc != 0) LOGW("bridge", "cmd rc=%d: %s", rc, cmd);
    return rc;
}

static int ifpresent(const char *name) {
    char path[128];
    snprintf(path, sizeof(path), "/sys/class/net/%s", name);
    struct stat st;
    return stat(path, &st) == 0;
}

static int ipforward(int on) {
    int fd = open("/proc/sys/net/ipv4/ip_forward", O_WRONLY);
    if (fd < 0) {
        LOGW("bridge", "cannot open ip_forward (want %d)", on);
        return -1;
    }
    ssize_t n = write(fd, on ? "1" : "0", 1);
    close(fd);
    if (n != 1) {
        LOGW("bridge", "cannot write ip_forward (want %d)", on);
        return -1;
    }
    return 0;
}

int bridge_usb_up(void) { return ifpresent("usb0"); }

static void wait_for_usb0(int secs) {
    for (int i = 0; i < secs * 10 && !ifpresent("usb0"); i++) usleep(100 * 1000);
}

int bridge_start(void) {
    const char *mode = g_cfg.netmode;
    const char *wan  = "eth0";
    const char *lan  = "usb0";

    /* usb0 appears a moment after gadget bind; give it half a second */
    wait_for_usb0(5);

    if (!ifpresent(wan)) {
        LOGW("bridge", "%s missing — passthrough disabled", wan);
    }

    sys("ip link set lo up");
    if (ifpresent(wan)) sys("ip link set eth0 up");
    if (ifpresent(lan)) sys("ip link set usb0 up");

    if (!g_cfg.passthrough_lan) {
        LOGI("bridge", "passthrough disabled, only link-up");
        return 0;
    }

    if (!strcasecmp(mode, "BRIDGE")) {
        sys("ip link add name br0 type bridge 2>/dev/null; true");
        sys("ip link set br0 up");
        if (ifpresent(wan)) sys("ip link set eth0 master br0");
        if (ifpresent(lan)) sys("ip link set usb0 master br0");
        sys("echo 0 > /proc/sys/net/bridge/bridge-nf-call-iptables 2>/dev/null; true");
        if (g_cfg.static_ip[0]) {
            char c[256];
            snprintf(c, sizeof(c), "ip addr add %s/%s dev br0",
                     g_cfg.static_ip, g_cfg.static_netmask[0] ? g_cfg.static_netmask : "24");
            sys(c);
        } else {
            sys("udhcpc -i br0 -b -t 1 -T 1 -q 2>/dev/null &");
        }
    } else if (!strcasecmp(mode, "NAT")) {
        ipforward(1);
        if (ifpresent(lan)) {
            char c[256];
            snprintf(c, sizeof(c), "ip addr add %s/24 dev usb0", g_cfg.gateway_ip);
            sys(c);
        }
        if (ifpresent(wan)) {
            sys("udhcpc -i eth0 -b -t 1 -T 1 -q 2>/dev/null &");
        }
        sys("nft add table ip nat 2>/dev/null; true");
        sys("nft 'add chain ip nat postrouting { type nat hook postrouting priority 100; }' 2>/dev/null; true");
        sys("nft 'add rule ip nat postrouting oifname eth0 masquerade' 2>/dev/null; true");
    } else if (!strcasecmp(mode, "TRANSPARENT")) {
        sys("ip link add name br0 type bridge 2>/dev/null; true");
        sys("ip link set br0 up");
        if (ifpresent(wan)) sys("ip link set eth0 master br0");
        if (ifpresent(lan)) sys("ip link set usb0 master br0");
        /* no IP on br0 — pure passive sniff + inject */
    } else if (!strcasecmp(mode, "ISOLATE")) {
        ipforward(0);
        if (ifpresent(lan)) {
            char c[256];
            snprintf(c, sizeof(c), "ip addr add %s/24 dev usb0", g_cfg.gateway_ip);
            sys(c);
        }
    } else {
        LOGE("bridge", "NETMODE=%s not implemented, no bridge set up", mode);
        return -1;
    }

    if (g_cfg.assign_ip && ifpresent(lan) &&
        (!strcasecmp(mode, "NAT") || !strcasecmp(mode, "ISOLATE"))) {
        /* light-weight DHCP server for the host-side */
        char cmd[512];
        char rs[64], re[64];
        strncpy(rs, g_cfg.dhcp_range, sizeof(rs)-1); rs[sizeof(rs)-1] = 0;
        char *comma = strchr(rs, ',');
        if (comma) { *comma = 0; strncpy(re, comma+1, sizeof(re)-1); re[sizeof(re)-1] = 0; }
        else       { strncpy(re, "172.16.24.60", sizeof(re)); }
        snprintf(cmd, sizeof(cmd),
                 "udhcpd -S -f /etc/ghostlink/udhcpd.conf >/dev/null 2>&1 &");
        /* generate udhcpd.conf on the fly */
        mkdir("/etc/ghostlink", 0755);
        int conf_ok = 0;
        FILE *f = fopen("/etc/ghostlink/udhcpd.conf", "w");
        if (f) {
            int wr = fprintf(f,
                "start   %s\n"
                "end     %s\n"
                "interface usb0\n"
                "option subnet 255.255.255.0\n"
                "option router %s\n"
                "option dns %s\n"
                "option lease 86400\n"
                "lease_file /tmp/udhcpd.leases\n", rs, re,
                g_cfg.gateway_ip,
                g_cfg.dns_upstream[0] ? g_cfg.dns_upstream : "1.1.1.1");
            int cl = fclose(f);
            conf_ok = (wr >= 0 && cl == 0);
        }
        if (conf_ok) {
            sys(cmd);
        } else {
            LOGE("bridge", "udhcpd.conf not written, udhcpd not started");
        }
    }

    if (g_cfg.mac_clone && g_cfg.mac_clone_from[0]) {
        char cmd[256];
        snprintf(cmd, sizeof(cmd),
                 "mac=$(cat /sys/class/net/%s/address) && "
                 "ip link set eth0 address $mac",
                 g_cfg.mac_clone_from);
        sys(cmd);
    }

    LOGI("bridge", "mode=%s wan=%s lan=%s up", mode, wan, lan);
    return 0;
}

int bridge_stop(void) {
    sys("ip link del br0 2>/dev/null");
    sys("pkill -f 'udhcpc' 2>/dev/null");
    sys("pkill -f 'udhcpd' 2>/dev/null");
    return 0;
}

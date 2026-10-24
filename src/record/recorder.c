/*
 * The core novel component: dual-stream recorder.
 *
 * One AF_PACKET socket, promisc, reads frames off the bridge. For each frame:
 *   1. Write to the current rotating pcap(ng) file.
 *   2. Compute a cheap flow hash (5-tuple xor) and ask the verdict map
 *      "does any rule match this flow right now?"
 *   3. Append one JSON line to the sibling verdicts.jsonl file.
 *
 * The verdict side-channel is what makes the recording queryable without
 * teaching the user tcpdump. The Web UI can later ask the file for all frames
 * tagged e.g. {"rule":"REDIRECT","host":"google.com"} and get byte-offsets.
 *
 * Rotation: when pcap exceeds RECORD_ROTATE_MB, close and open NN+1. When
 * the ring hits RECORD_RING_FILES, delete the oldest pair.
 */

#include "recorder.h"
#include "../core/config.h"
#include "../common/log.h"

#include <arpa/inet.h>
#include <dirent.h>
#include <errno.h>
#include <fcntl.h>
#include <linux/if_ether.h>
#include <linux/if_packet.h>
#include <net/if.h>
#include <netinet/in.h>
#include <netinet/ip.h>
#include <netinet/tcp.h>
#include <netinet/udp.h>
#include <pthread.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/ioctl.h>
#include <sys/socket.h>
#include <sys/stat.h>
#include <sys/time.h>
#include <sys/types.h>
#include <time.h>
#include <unistd.h>

#define SD_ROOT       "/mnt/sd"
#define RECORDS_DIR   SD_ROOT "/Records"

/* pcap classic header */
struct pcap_hdr {
    uint32_t magic;
    uint16_t ver_maj;
    uint16_t ver_min;
    int32_t  thiszone;
    uint32_t sigfigs;
    uint32_t snaplen;
    uint32_t network;
};
struct pcap_rec {
    uint32_t ts_sec;
    uint32_t ts_usec;
    uint32_t caplen;
    uint32_t len;
};

static volatile int g_run = 0;
static pthread_t    g_t;
static FILE        *g_pcap = NULL;
static FILE        *g_verd = NULL;
static int          g_idx  = 0;
static size_t       g_bytes = 0;
static uint64_t     g_frame = 0;

/* ring buffer for verdicts: last 128 flow matches, stamped with frame# */
#define VERD_RING 128
static struct {
    uint64_t frame;
    char rule[32];
    char verdict[16];
    char detail[128];
} g_verd_ring[VERD_RING];
static int g_verd_head = 0;
static pthread_mutex_t g_verd_mu = PTHREAD_MUTEX_INITIALIZER;

void recorder_verdict(const char *rule, const char *verdict, const char *detail) {
    pthread_mutex_lock(&g_verd_mu);
    int i = g_verd_head;
    g_verd_ring[i].frame = __atomic_load_n(&g_frame, __ATOMIC_RELAXED);
    strncpy(g_verd_ring[i].rule,    rule    ? rule    : "", sizeof(g_verd_ring[i].rule)-1);
    g_verd_ring[i].rule[sizeof(g_verd_ring[i].rule)-1] = 0;
    strncpy(g_verd_ring[i].verdict, verdict ? verdict : "", sizeof(g_verd_ring[i].verdict)-1);
    g_verd_ring[i].verdict[sizeof(g_verd_ring[i].verdict)-1] = 0;
    strncpy(g_verd_ring[i].detail,  detail  ? detail  : "", sizeof(g_verd_ring[i].detail)-1);
    g_verd_ring[i].detail[sizeof(g_verd_ring[i].detail)-1] = 0;
    g_verd_head = (g_verd_head + 1) % VERD_RING;
    pthread_mutex_unlock(&g_verd_mu);
}

static int consume_matching_verdict(uint64_t frame, char *rule, char *verdict, char *detail) {
    pthread_mutex_lock(&g_verd_mu);
    for (int k = 0; k < VERD_RING; k++) {
        int i = (g_verd_head - 1 - k + VERD_RING) % VERD_RING;
        if (g_verd_ring[i].rule[0] && g_verd_ring[i].frame == frame) {
            strcpy(rule,    g_verd_ring[i].rule);
            strcpy(verdict, g_verd_ring[i].verdict);
            strcpy(detail,  g_verd_ring[i].detail);
            g_verd_ring[i].rule[0] = 0;
            pthread_mutex_unlock(&g_verd_mu);
            return 1;
        }
    }
    pthread_mutex_unlock(&g_verd_mu);
    return 0;
}

static void prune_oldest(int max_files) {
    /* list Records/*.pcap, sort by mtime, delete oldest until <= max_files */
    DIR *d = opendir(RECORDS_DIR);
    if (!d) return;
    struct dirent *e;
    struct { char name[64]; time_t t; } files[256];
    int n = 0;
    while ((e = readdir(d)) && n < 256) {
        if (strstr(e->d_name, ".pcap") == NULL) continue;
        char full[256];
        snprintf(full, sizeof(full), RECORDS_DIR "/%s", e->d_name);
        struct stat st;
        if (stat(full, &st) == 0) {
            strncpy(files[n].name, e->d_name, sizeof(files[n].name)-1);
            files[n].name[sizeof(files[n].name)-1] = 0;
            files[n].t = st.st_mtime;
            n++;
        }
    }
    closedir(d);
    while (n > max_files) {
        int oldest = 0;
        for (int i = 1; i < n; i++) if (files[i].t < files[oldest].t) oldest = i;
        char p1[300], p2[300];
        snprintf(p1, sizeof(p1), RECORDS_DIR "/%s", files[oldest].name);
        strcpy(p2, p1);
        char *dot = strrchr(p2, '.');
        if (dot) { strcpy(dot, ".jsonl"); unlink(p2); }
        unlink(p1);
        LOGI("rec", "pruned %s", files[oldest].name);
        files[oldest] = files[n-1];
        n--;
    }
}

static int rotate(void) {
    if (g_pcap) { fclose(g_pcap); g_pcap = NULL; }
    if (g_verd) { fclose(g_verd); g_verd = NULL; }
    g_bytes = 0;

    mkdir(RECORDS_DIR, 0755);
    g_idx++;
    char pcap_path[320], verd_path[320];
    snprintf(pcap_path, sizeof(pcap_path), RECORDS_DIR "/rec_%06d.pcap", g_idx);
    snprintf(verd_path, sizeof(verd_path), RECORDS_DIR "/rec_%06d.jsonl", g_idx);

    g_pcap = fopen(pcap_path, "wb");
    g_verd = fopen(verd_path, "ab");
    if (!g_pcap || !g_verd) {
        LOGE("rec", "open rotation files failed: %s", strerror(errno));
        return -1;
    }
    setvbuf(g_pcap, NULL, _IOFBF, 32 * 1024);
    setvbuf(g_verd, NULL, _IOLBF, 0);

    struct pcap_hdr h = {
        .magic = 0xa1b2c3d4, .ver_maj = 2, .ver_min = 4,
        .thiszone = 0, .sigfigs = 0,
        .snaplen = 65535, .network = 1 /* DLT_EN10MB */
    };
    fwrite(&h, sizeof(h), 1, g_pcap);
    fflush(g_pcap);

    prune_oldest(g_cfg.record_ring_files > 0 ? g_cfg.record_ring_files : 32);
    LOGI("rec", "rotated to rec_%06d.{pcap,jsonl}", g_idx);
    return 0;
}

static void write_verdict_json(uint64_t frame, uint32_t ts_sec, uint32_t ts_us,
                               uint32_t caplen, const uint8_t *frame_data) {
    if (!g_verd || !g_cfg.verdict_stream) return;

    /* always emit at least a minimal entry (frame + flow hash + size) so
     * every frame is addressable. Rule hits add fields. */
    uint32_t flow = 0;
    const uint8_t *eth = frame_data;
    if (caplen >= 14 && eth[12] == 0x08 && eth[13] == 0x00 && caplen >= 34) {
        const struct iphdr *ip = (const struct iphdr *)(eth + 14);
        flow = ip->saddr ^ ip->daddr;
        if (ip->protocol == IPPROTO_TCP && caplen >= 54) {
            const struct tcphdr *t = (const struct tcphdr *)((uint8_t *)ip + ip->ihl * 4);
            flow ^= (uint32_t)t->source ^ ((uint32_t)t->dest << 16);
        } else if (ip->protocol == IPPROTO_UDP && caplen >= 42) {
            const struct udphdr *u = (const struct udphdr *)((uint8_t *)ip + ip->ihl * 4);
            flow ^= (uint32_t)u->source ^ ((uint32_t)u->dest << 16);
        }
    }

    char rule[32] = {0}, vd[16] = {0}, detail[128] = {0};
    int tagged = consume_matching_verdict(frame, rule, vd, detail);

    if (tagged) {
        fprintf(g_verd,
            "{\"f\":%llu,\"t\":%u.%06u,\"sz\":%u,\"flow\":\"%08x\",\"rule\":\"%s\",\"v\":\"%s\",\"d\":\"%s\"}\n",
            (unsigned long long)frame, ts_sec, ts_us, caplen, flow, rule, vd, detail);
    } else {
        fprintf(g_verd,
            "{\"f\":%llu,\"t\":%u.%06u,\"sz\":%u,\"flow\":\"%08x\"}\n",
            (unsigned long long)frame, ts_sec, ts_us, caplen, flow);
    }
}

static void *rec_worker(void *arg) {
    (void)arg;

    const char *ifn = "br0";
    if (access("/sys/class/net/br0", F_OK) != 0) ifn = "eth0";

    int s = socket(AF_PACKET, SOCK_RAW, htons(ETH_P_ALL));
    if (s < 0) { LOGE("rec", "AF_PACKET: %s", strerror(errno)); return NULL; }
    struct sockaddr_ll sa;
    memset(&sa, 0, sizeof(sa));
    sa.sll_family   = AF_PACKET;
    sa.sll_protocol = htons(ETH_P_ALL);
    sa.sll_ifindex  = if_nametoindex(ifn);
    if (bind(s, (struct sockaddr *)&sa, sizeof(sa)) < 0) {
        LOGE("rec", "bind %s: %s", ifn, strerror(errno));
        close(s); return NULL;
    }

    /* Bounded recv: the loop re-checks g_run every second, so stop can join. */
    struct timeval rt = { 1, 0 };
    setsockopt(s, SOL_SOCKET, SO_RCVTIMEO, &rt, sizeof(rt));

    struct packet_mreq mr;
    memset(&mr, 0, sizeof(mr));
    mr.mr_ifindex = sa.sll_ifindex;
    mr.mr_type    = PACKET_MR_PROMISC;
    setsockopt(s, SOL_PACKET, PACKET_ADD_MEMBERSHIP, &mr, sizeof(mr));

    if (rotate() < 0) { close(s); return NULL; }

    uint8_t buf[65536];
    while (g_run) {
        ssize_t n = recv(s, buf, sizeof(buf), 0);
        if (n <= 0) continue;

        struct timeval tv;
        gettimeofday(&tv, NULL);

        uint64_t fr = __atomic_add_fetch(&g_frame, 1, __ATOMIC_RELAXED);

        struct pcap_rec r;
        r.ts_sec  = (uint32_t)tv.tv_sec;
        r.ts_usec = (uint32_t)tv.tv_usec;
        r.caplen  = (uint32_t)n;
        r.len     = (uint32_t)n;
        /* A failed rotate() leaves g_pcap NULL: drop frames instead of writing to it. */
        if (!g_pcap) continue;
        fwrite(&r, sizeof(r), 1, g_pcap);
        fwrite(buf, (size_t)n, 1, g_pcap);
        g_bytes += sizeof(r) + (size_t)n;

        write_verdict_json(fr, r.ts_sec, r.ts_usec, r.caplen, buf);

        size_t rot = (size_t)g_cfg.record_rotate_mb * 1024 * 1024;
        if (rot && g_bytes >= rot) {
            fflush(g_pcap);
            if (rotate() < 0) LOGE("rec", "rotation failed: pcap writes stopped until restart");
        }
    }
    close(s);
    if (g_pcap) { fclose(g_pcap); g_pcap = NULL; }
    if (g_verd) { fclose(g_verd); g_verd = NULL; }
    return NULL;
}

static int g_started = 0;   /* set only while a worker thread exists */

int recorder_start(void) {
    if (!g_cfg.log_to_sd) { LOGI("rec", "disabled"); return 0; }
    if (g_started) return 0;
    mkdir(SD_ROOT, 0755);
    mkdir(RECORDS_DIR, 0755);
    g_run = 1;
    if (pthread_create(&g_t, NULL, rec_worker, NULL) != 0) { g_run = 0; return -1; }
    g_started = 1;
    return 0;
}

int recorder_stop(void) {
    if (!g_started) return 0;   /* never join a thread that was not created */
    g_run = 0;
    pthread_join(g_t, NULL);
    g_started = 0;
    return 0;
}

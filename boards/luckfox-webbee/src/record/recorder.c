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
    /* list the .pcap files in RECORDS_DIR, drop the lowest rec_NNNNNN index until <= max_files. By index, not
     * mtime: a clock step must not make a new capture the "oldest" one. */
    enum { RING_MAX = 1024 };   /* scan limit; a larger RECORD_RING_FILES is capped */
    if (max_files > RING_MAX - 1) max_files = RING_MAX - 1;
    DIR *d = opendir(RECORDS_DIR);
    if (!d) return;
    struct dirent *e;
    static struct { char name[256]; int idx; } files[RING_MAX];
    int n = 0;
    while ((e = readdir(d)) && n < RING_MAX) {
        size_t nl = strlen(e->d_name);   /* exact ".pcap" suffix only */
        if (nl < 5 || strcmp(e->d_name + nl - 5, ".pcap") != 0) continue;
        char full[300];
        snprintf(full, sizeof(full), RECORDS_DIR "/%s", e->d_name);
        struct stat st;
        if (stat(full, &st) == 0) {
            int idx = 0;
            if (sscanf(e->d_name, "rec_%d.", &idx) != 1) idx = 0x7fffffff;   /* other names go last */
            strncpy(files[n].name, e->d_name, sizeof(files[n].name)-1);
            files[n].name[sizeof(files[n].name)-1] = 0;
            files[n].idx = idx;
            n++;
        }
    }
    closedir(d);
    while (n > max_files) {
        int oldest = 0;
        for (int i = 1; i < n; i++) if (files[i].idx < files[oldest].idx) oldest = i;
        char p1[300], p2[300];
        snprintf(p1, sizeof(p1), RECORDS_DIR "/%s", files[oldest].name);
        if (unlink(p1) != 0) {
            LOGE("rec", "cannot prune %s: %s", files[oldest].name, strerror(errno));
            break;   /* the capture is still on disk: stop here and do not count it as removed */
        }
        LOGI("rec", "pruned %s", files[oldest].name);
        strcpy(p2, p1);
        char *dot = strrchr(p2, '.');
        if (dot) { strcpy(dot, ".jsonl"); unlink(p2); }   /* companion, removed only after the capture */
        files[oldest] = files[n-1];
        n--;
    }
}

/* Highest rec_NNNNNN index already on the SD card. A reboot continues the
 * sequence from here, so an existing capture is never reused. */
static int highest_index_on_disk(void) {
    int best = 0;
    DIR *d = opendir(RECORDS_DIR);
    if (!d) return 0;
    struct dirent *e;
    while ((e = readdir(d))) {
        int idx = 0;
        if (sscanf(e->d_name, "rec_%d.", &idx) == 1 && idx > best) best = idx;
    }
    closedir(d);
    return best;
}

/* Last frame number in rec_NNNNNN.jsonl, so frame numbers stay unique across boots. */
static uint64_t last_frame_on_disk(int idx) {
    char p[320];
    snprintf(p, sizeof(p), RECORDS_DIR "/rec_%06d.jsonl", idx);
    FILE *f = fopen(p, "rb");
    if (!f) return 0;
    if (fseek(f, 0, SEEK_END) != 0) { fclose(f); return 0; }
    long size = ftell(f);
    if (size < 0 || fseek(f, size > 4096 ? size - 4096 : 0, SEEK_SET) != 0) { fclose(f); return 0; }
    char tail[4097];
    size_t got = fread(tail, 1, sizeof(tail) - 1, f);
    fclose(f);
    tail[got] = 0;
    const char *last = NULL;
    /* only a complete "f":N, field counts: a line cut by a power loss can end in a partial number */
    for (const char *q = tail; (q = strstr(q, "\"f\":")) != NULL; q += 4) {
        const char *d = q + 4;
        while (*d >= '0' && *d <= '9') d++;
        if (d > q + 4 && *d == ',') last = q;
    }
    return last ? (uint64_t)strtoull(last + 4, NULL, 10) : 0;
}

static int sd_mounted(void);

static int rotate(void) {
    /* never create a capture unless the card is mounted (see sd_mounted) */
    if (!sd_mounted()) {
        LOGE("rec", "%s is not a mounted SD card; rotation skipped", SD_ROOT);
        return -1;
    }
    if (g_pcap) { fclose(g_pcap); g_pcap = NULL; }
    if (g_verd) { fclose(g_verd); g_verd = NULL; }
    g_bytes = 0;

    mkdir(RECORDS_DIR, 0755);
    /* Prune before the new pair exists, so the ring can never delete the capture being written. */
    prune_oldest((g_cfg.record_ring_files > 0 ? g_cfg.record_ring_files : 32) - 1);
    g_idx++;
    char pcap_path[320], verd_path[320];
    snprintf(pcap_path, sizeof(pcap_path), RECORDS_DIR "/rec_%06d.pcap", g_idx);
    snprintf(verd_path, sizeof(verd_path), RECORDS_DIR "/rec_%06d.jsonl", g_idx);

    /* O_EXCL: an existing capture is never truncated. */
    int pfd = open(pcap_path, O_WRONLY | O_CREAT | O_EXCL, 0644);
    int pcap_errno = errno;   /* saved right after the open: access() and fopen() below can overwrite errno */
    int created_pcap = (pfd >= 0);
    g_pcap = (pfd >= 0) ? fdopen(pfd, "wb") : NULL;
    if (pfd >= 0 && !g_pcap) close(pfd);
    int verd_existed = (access(verd_path, F_OK) == 0);
    g_verd = fopen(verd_path, "ab");
    if (!g_pcap || !g_verd) {
        LOGE("rec", "open rotation files failed: %s", strerror(pcap_errno));
        /* close whichever half opened, and remove only the files this call created */
        if (g_pcap) { fclose(g_pcap); g_pcap = NULL; }
        if (g_verd) { fclose(g_verd); g_verd = NULL; }
        if (created_pcap) unlink(pcap_path);
        if (!verd_existed) unlink(verd_path);
        return -1;
    }
    setvbuf(g_pcap, NULL, _IOFBF, 32 * 1024);
    setvbuf(g_verd, NULL, _IOLBF, 0);

    struct pcap_hdr h = {
        .magic = 0xa1b2c3d4, .ver_maj = 2, .ver_min = 4,
        .thiszone = 0, .sigfigs = 0,
        .snaplen = 65535, .network = 1 /* DLT_EN10MB */
    };
    if (fwrite(&h, sizeof(h), 1, g_pcap) != 1 || fflush(g_pcap) != 0) {
        LOGE("rec", "pcap header write failed: %s", strerror(errno));
        fclose(g_pcap); g_pcap = NULL;
        fclose(g_verd); g_verd = NULL;
        unlink(pcap_path);
        if (!verd_existed) unlink(verd_path);
        return -1;
    }
    LOGI("rec", "rotated to rec_%06d.{pcap,jsonl}", g_idx);
    return 0;
}

/* Logs the first failed verdict write; later failures stay quiet. */
static void verdict_write_failed(void) {
    static int warned;
    if (!warned) { warned = 1; LOGE("rec", "verdict write failed: %s", strerror(errno)); }
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
        /* ports are the first 4 bytes of the L4 header, which starts at 14 + IHL*4 */
        size_t l4 = 14 + (size_t)ip->ihl * 4;
        if (ip->ihl >= 5 && caplen >= l4 + 4) {
            if (ip->protocol == IPPROTO_TCP) {
                const struct tcphdr *t = (const struct tcphdr *)(eth + l4);
                flow ^= (uint32_t)t->source ^ ((uint32_t)t->dest << 16);
            } else if (ip->protocol == IPPROTO_UDP) {
                const struct udphdr *u = (const struct udphdr *)(eth + l4);
                flow ^= (uint32_t)u->source ^ ((uint32_t)u->dest << 16);
            }
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
    if (ferror(g_verd)) verdict_write_failed();
}

/* Set by the recorder thread; recorder_start() waits for g_ready. */
static pthread_mutex_t g_ctl_mu = PTHREAD_MUTEX_INITIALIZER;   /* serializes recorder start and stop */
static int g_joinable = 0;   /* a worker was created and has not been joined yet */
static int g_ready = 0;      /* the worker has its socket and its first file */

/* Monotonic seconds for the retry gate: a wall-clock step must not stretch or cut the 10 s wait. */
static time_t mono_seconds(void) {
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return ts.tv_sec;
}

static void *rec_worker_body(void *arg) {
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
    /* index 0 would bind to every interface, so refuse it instead */
    if (sa.sll_ifindex == 0) { LOGE("rec", "no capture interface %s", ifn); close(s); return NULL; }
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
    if (setsockopt(s, SOL_PACKET, PACKET_ADD_MEMBERSHIP, &mr, sizeof(mr)) != 0)
        LOGW("rec", "promiscuous mode on %s: %s", ifn, strerror(errno));

    /* Continue the file index and frame numbers from what is already on the SD card. */
    g_idx = highest_index_on_disk();
    __atomic_store_n(&g_frame, last_frame_on_disk(g_idx), __ATOMIC_RELAXED);

    if (rotate() < 0) { close(s); return NULL; }
    __atomic_store_n(&g_ready, 1, __ATOMIC_RELEASE);   /* recorder_start() may report success now */

    uint8_t buf[65536];
    time_t last_try = 0;
    while (g_run) {
        /* A failed rotate() leaves no capture open: retry every 10 s, on every pass,
         * including the idle passes where recv() times out. */
        if (!g_pcap) {
            time_t now = mono_seconds();
            if (now - last_try >= 10) { last_try = now; rotate(); }
        }
        ssize_t n = recv(s, buf, sizeof(buf), MSG_TRUNC);   /* MSG_TRUNC: n is the real frame length */
        if (n <= 0) continue;
        size_t cap = (size_t)n < sizeof(buf) ? (size_t)n : sizeof(buf);

        struct timeval tv;
        gettimeofday(&tv, NULL);

        uint64_t fr = __atomic_add_fetch(&g_frame, 1, __ATOMIC_RELAXED);

        struct pcap_rec r;
        r.ts_sec  = (uint32_t)tv.tv_sec;
        r.ts_usec = (uint32_t)tv.tv_usec;
        r.caplen  = (uint32_t)cap;
        r.len     = (uint32_t)n;
        if (!g_pcap) continue;   /* no capture open: the frame is dropped */
        if (fwrite(&r, sizeof(r), 1, g_pcap) != 1 || fwrite(buf, cap, 1, g_pcap) != 1) {
            /* card full or removed: log once. Close the pair, so a half-written record ends its file. */
            static int warned;
            if (!warned) { warned = 1; LOGE("rec", "pcap write failed: %s", strerror(errno)); }
            fclose(g_pcap); g_pcap = NULL;
            if (g_verd) { fclose(g_verd); g_verd = NULL; }
            continue;
        }
        g_bytes += sizeof(r) + cap;

        write_verdict_json(fr, r.ts_sec, r.ts_usec, r.caplen, buf);

        size_t rot = (size_t)g_cfg.record_rotate_mb * 1024 * 1024;
        if (rot && g_bytes >= rot) {
            fflush(g_pcap);
            if (rotate() < 0) LOGE("rec", "rotation failed, will retry");
        }
    }
    close(s);
    if (g_pcap) { fclose(g_pcap); g_pcap = NULL; }
    if (g_verd) { fclose(g_verd); g_verd = NULL; }
    return NULL;
}

static int g_started = 0;   /* set only while a worker thread exists */

/* The thread clears g_started on every exit, including the early ones, so a
 * worker that dies at start (a failed rotate on a read-only card, say) no
 * longer makes the recorder look running. */
static void *rec_worker(void *arg) {
    void *r = rec_worker_body(arg);
    __atomic_store_n(&g_started, 0, __ATOMIC_RELEASE);
    return r;
}

/* True only when the SD card is mounted at SD_ROOT. Without this, captures
 * land on the root filesystem and fill it. */
static int sd_mounted(void) {
    FILE *f = fopen("/proc/mounts", "r");
    if (!f) return 0;
    char dev[128], mnt[256];
    int found = 0;
    while (!found && fscanf(f, "%127s %255s %*[^\n]", dev, mnt) == 2) {
        if (strcmp(mnt, SD_ROOT) == 0) found = 1;
    }
    fclose(f);
    return found;
}

static void stop_worker_locked(void);

static int recorder_start_locked(void) {
    if (!g_cfg.log_to_sd) { LOGI("rec", "disabled"); return 0; }
    if (__atomic_load_n(&g_started, __ATOMIC_ACQUIRE)) return 0;
    if (!sd_mounted()) {
        LOGE("rec", "%s is not a mounted SD card; recording not started", SD_ROOT);
        return -1;
    }
    if (g_joinable) { pthread_join(g_t, NULL); g_joinable = 0; }   /* reap the previous worker first */
    mkdir(SD_ROOT, 0755);
    mkdir(RECORDS_DIR, 0755);
    g_run = 1;
    __atomic_store_n(&g_ready, 0, __ATOMIC_RELEASE);
    __atomic_store_n(&g_started, 1, __ATOMIC_RELEASE);   /* set before the thread can run and clear it */
    if (pthread_create(&g_t, NULL, rec_worker, NULL) != 0) {
        __atomic_store_n(&g_started, 0, __ATOMIC_RELEASE);
        g_run = 0;
        return -1;
    }
    g_joinable = 1;
    /* success once the worker has its socket and first file; a worker that exits first is a failure */
    for (int i = 0; i < 300 && __atomic_load_n(&g_started, __ATOMIC_ACQUIRE) &&
                    !__atomic_load_n(&g_ready, __ATOMIC_ACQUIRE); i++)
        usleep(10 * 1000);
    if (!__atomic_load_n(&g_ready, __ATOMIC_ACQUIRE)) {
        stop_worker_locked();
        LOGE("rec", "recording did not start (see the lines above)");
        return -1;
    }
    return 0;
}

/* Stops the worker and joins it. The caller holds g_ctl_mu. */
static void stop_worker_locked(void) {
    g_run = 0;
    if (g_joinable) { pthread_join(g_t, NULL); g_joinable = 0; }   /* join only a thread that was created */
    __atomic_store_n(&g_started, 0, __ATOMIC_RELEASE);
}

int recorder_stop(void) {
    pthread_mutex_lock(&g_ctl_mu);
    stop_worker_locked();
    pthread_mutex_unlock(&g_ctl_mu);
    return 0;
}

/* The supervisor and the PANIC path both start and stop the recorder; g_ctl_mu makes them take turns. */
int recorder_start(void) {
    pthread_mutex_lock(&g_ctl_mu);
    int rc = recorder_start_locked();
    pthread_mutex_unlock(&g_ctl_mu);
    return rc;
}

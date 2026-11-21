/*
 * GhostLink main. Fast-boot path on an RV1106 (1.2GHz Cortex-A7):
 *
 *   t=0.00s  init exec'd by /etc/init.d/S99ghost
 *   t=0.01s  mount SD card from /dev/mmcblk1p1
 *   t=0.02s  parse config.txt
 *   t=0.03s  log init to serial + /Logs/ghostlink.log
 *   t=0.04s  USB gadget composite (NCM + HID) via configfs
 *   t=0.08s  bridge up (br0 or NAT depending on NETMODE)
 *   t=0.09s  spawn: recorder, httpd, deadnet, redirect, hid
 *   t=0.10s  button watcher + LED
 *   t=0.11s  returns to supervisor loop
 *
 * The whole thing is a single binary. We fork() nothing we don't need to.
 */

#include "config.h"
#include "supervisor.h"
#include "../common/log.h"
#include "../net/gadget.h"
#include "../net/bridge.h"
#include "../net/deadnet.h"
#include "../net/redirect.h"
#include "../record/recorder.h"
#include "../web/httpd.h"
#include "../hid/hid.h"

#include <errno.h>
#include <fcntl.h>
#include <signal.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <linux/reboot.h>
#include <sys/mount.h>
#include <sys/reboot.h>
#include <sys/stat.h>
#include <sys/types.h>
#include <sys/wait.h>
#include <unistd.h>

#define SD_DEV   "/dev/mmcblk1p1"
#define SD_MNT   "/mnt/sd"
#define CFG_PATH SD_MNT "/config.txt"

static volatile sig_atomic_t g_running = 1;

static void on_sig(int s) {
    (void)s;
    g_running = 0;
}

/* Mount SD. Try FAT first, then ext4. mkdir everything we need. */
static int mount_sd(void) {

    struct stat st;
    if (stat(SD_DEV, &st) != 0) {
        LOGW("boot", "no %s — running without SD (defaults only)", SD_DEV);
        return -1;
    }

    mkdir("/mnt", 0755);   /* only with a card present, so nothing is created on the root filesystem otherwise */
    mkdir(SD_MNT, 0755);
    if (mount(SD_DEV, SD_MNT, "vfat", 0, "iocharset=utf8") == 0) {
        LOGI("boot", "SD mounted (vfat)");
        return 0;
    }
    if (mount(SD_DEV, SD_MNT, "exfat", 0, NULL) == 0) {
        LOGI("boot", "SD mounted (exfat)");
        return 0;
    }
    if (mount(SD_DEV, SD_MNT, "ext4", 0, NULL) == 0) {
        LOGI("boot", "SD mounted (ext4)");
        return 0;
    }
    LOGW("boot", "SD mount failed: %s", strerror(errno));
    return -1;
}

/* Create skeleton on a blank SD so a cold boot leaves usable folders. */
static void ensure_sd_skeleton(void) {
    const char *dirs[] = {
        "Records", "Payloads", "Languages", "WebUI", "Scripts",
        "Loot", "Spoofhosts", "Honeypot", "Firmware", "Logs",
        "Rules", "Modules", NULL
    };
    char p[128];
    for (int i = 0; dirs[i]; i++) {
        snprintf(p, sizeof(p), "%s/%s", SD_MNT, dirs[i]);
        mkdir(p, 0755);
    }
    /* if no config.txt, drop a stub so the Web UI has something to edit */
    if (access(CFG_PATH, F_OK) != 0) {
        FILE *f = fopen(CFG_PATH, "w");
        if (f) {
            fprintf(f, "# fresh config - edit via WebUI or SSH\n"
                       "USB=TRUE\nPASSTHROUGH_LAN=TRUE\nLOG_TO_SD=TRUE\n"
                       "WEBUI=TRUE\nHID_ENABLED=TRUE\n");
            fclose(f);
        }
    }
}

/* Button monitor on PA7/PB2 — reads /sys/class/gpio/... pulled low on press.
 * Tap counts within TAP_WINDOW_MS fire the mapped payloads. Long-hold runs
 * panic. On an unknown board file this just quietly idles. */
#include <pthread.h>
#include <time.h>
static pthread_t g_btn_t;
static volatile int g_btn_run = 0;

static long now_ms(void) {
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return ts.tv_sec * 1000L + ts.tv_nsec / 1000000L;
}

static int read_button(void) {
    static int fd = -1;
    static long next_try = 0;
    if (fd < 0) {
        /* Try a couple of plausible paths. The node can appear after boot, so retry every 5 s. */
        long now = now_ms();
        if (now >= next_try) {
            next_try = now + 5000;
            fd = open("/sys/class/gpio/BACK/value", O_RDONLY);
            if (fd < 0) fd = open("/dev/input/button", O_RDONLY);
        }
        if (fd < 0) return 1;
    }
    char c = 1;
    lseek(fd, 0, SEEK_SET);
    read(fd, &c, 1);
    return (c == '0') ? 0 : 1;
}

static void *btn_worker(void *arg) {
    (void)arg;
    int taps = 0;
    long first_tap = 0, last_edge = 0;
    int was_down = 0;
    int panic_fired = 0;   /* the scrub runs once per press, even if reboot() returns */
    long press_start = 0;

    while (g_btn_run) {
        int v = read_button();  /* 0 = pressed, 1 = up */
        long t = now_ms();

        if (!v && !was_down) {
            was_down = 1;
            press_start = t;
            if (!first_tap) first_tap = t;
            taps++;
            last_edge = t;
        } else if (v && was_down) {
            was_down = 0;
            panic_fired = 0;   /* the next long hold may fire again */
        }

        /* long hold -> panic */
        if (was_down && !panic_fired && g_cfg.panic_hold_s > 0 &&
            (t - press_start) > (long)g_cfg.panic_hold_s * 1000) {
            panic_fired = 1;
            LOGW("btn", "PANIC hold — scrub + reboot");
            if (g_cfg.panic_scrub) {
                recorder_stop();   /* no capture writer may race the scrub */
                system("find /mnt/sd/Records -type f -exec shred -u -n 1 {} \\; 2>/dev/null");
                system("rm -rf /mnt/sd/Records /mnt/sd/Loot /mnt/sd/Logs/*.log");
            }
            sync();
            reboot(RB_AUTOBOOT);
            /* reboot() returned, so it failed: put recording back the way it was */
            if (g_cfg.log_to_sd) recorder_start();
        }

        /* tap window closed -> dispatch */
        if (taps > 0 && (t - last_edge) > (long)g_cfg.payload_tap_window_ms) {
            LOGI("btn", "%d tap(s)", taps);
            if (taps == 1 && g_cfg.payload_switch1[0]) {
                char full[256];
                snprintf(full, sizeof(full), "/mnt/sd%s", g_cfg.payload_switch1);
                if (access(full, X_OK) == 0) {
                    char cmd[300]; snprintf(cmd, sizeof(cmd), "sh %s &", full); system(cmd);
                }
            } else if (taps == 2 && g_cfg.payload_switch2[0]) {
                char full[256];
                snprintf(full, sizeof(full), "/mnt/sd%s", g_cfg.payload_switch2);
                char cmd[300]; snprintf(cmd, sizeof(cmd), "sh %s &", full); system(cmd);
            } else if (taps == 3 && g_cfg.payload_switch3[0]) {
                char full[256];
                snprintf(full, sizeof(full), "/mnt/sd%s", g_cfg.payload_switch3);
                char cmd[300]; snprintf(cmd, sizeof(cmd), "sh %s &", full); system(cmd);
            } else if (taps == g_cfg.factory_reset_taps) {
                LOGW("btn", "factory reset gesture");
                system("rm -f /mnt/sd/config.txt && sync && reboot");
            } else if (!strcasecmp(g_cfg.hid_trigger, "BUTTON") &&
                       g_cfg.hid_payload[0]) {
                char full[256];
                if (g_cfg.hid_payload[0] == '/')
                    snprintf(full, sizeof(full), "/mnt/sd%s", g_cfg.hid_payload);
                else
                    snprintf(full, sizeof(full), "/mnt/sd/Payloads/%s", g_cfg.hid_payload);
                hid_run_payload(full);
            }
            taps = 0;
            first_tap = 0;
        }

        usleep(25 * 1000);
    }
    return NULL;
}

/* Service table */
static svc_t SVC_RECORDER = { "recorder", recorder_start, recorder_stop, 1, &g_cfg.log_to_sd, 0 };
static svc_t SVC_HTTPD    = { "httpd",    httpd_start,    httpd_stop,    1, &g_cfg.webui, 0 };
static svc_t SVC_DEADNET  = { "deadnet",  deadnet_start,  deadnet_stop,  1, &g_cfg.deadnet, 0 };
static svc_t SVC_REDIRECT = { "redirect", redirect_start, redirect_stop, 1, &g_cfg.redirect, 0 };
static svc_t SVC_HID      = { "hid",      hid_start,      hid_stop,      1, &g_cfg.hid_enabled, 0 };

/* LOG_LEVEL and the log file. Runs at boot and after every config change. */
static void apply_log_settings(void) {
    log_level_t ll = LL_INFO;
    if      (!strcasecmp(g_cfg.log_level, "DEBUG")) ll = LL_DEBUG;
    else if (!strcasecmp(g_cfg.log_level, "WARN"))  ll = LL_WARN;
    else if (!strcasecmp(g_cfg.log_level, "ERROR")) ll = LL_ERROR;

    char logfile[256];
    snprintf(logfile, sizeof(logfile), "%s%s", SD_MNT, g_cfg.log_to_sd_file);
    log_init(ll, logfile, g_cfg.log_to_serial);
}

int main(int argc, char **argv) {
    (void)argc; (void)argv;
    signal(SIGINT,  on_sig);
    signal(SIGTERM, on_sig);
    signal(SIGPIPE, SIG_IGN);

    long t0 = now_ms();
    /* ultra-early serial so a boot hang shows a hint */
    log_init(LL_INFO, NULL, 1);
    LOGI("boot", "GhostLink firmware starting");

    /* the SD card is optional: without one, nothing is created on the root filesystem */
    if (mount_sd() == 0) ensure_sd_skeleton();

    cfg_set_default(&g_cfg);   /* defaults first, so an unreadable config still leaves usable settings */
    cfg_load(CFG_PATH);
    apply_log_settings();

    /* hostname / mdns name */
    if (g_cfg.hostname[0]) sethostname(g_cfg.hostname, strlen(g_cfg.hostname));

    /* USB first — bridge needs usb0 */
    if (gadget_start() != 0) LOGE("boot", "USB gadget failed to start; no USB until fixed");

    /* passthrough */
    if (bridge_start() != 0) LOGE("boot", "bridge failed to start");

    /* supervisor */
    supervisor_init();
    supervisor_register(&SVC_RECORDER);
    supervisor_register(&SVC_HTTPD);
    supervisor_register(&SVC_DEADNET);
    supervisor_register(&SVC_REDIRECT);
    supervisor_register(&SVC_HID);
    supervisor_start_all();

    /* button */
    g_btn_run = 1;
    int btn_rc = pthread_create(&g_btn_t, NULL, btn_worker, NULL);
    if (btn_rc != 0) LOGE("btn", "button thread not started: %s", strerror(btn_rc));

    LOGI("boot", "GhostLink ready in %ldms", now_ms() - t0);

    /* reconcile loop — watches cfg generation and starts/stops services */
    uint64_t last_gen = 0;
    while (g_running) {
        uint64_t g = __atomic_load_n(&g_cfg_gen, __ATOMIC_ACQUIRE);
        if (g != last_gen) {
            last_gen = g;
            LOGI("boot", "cfg generation %llu — reconcile", (unsigned long long)g);
            apply_log_settings();   /* LOG_LEVEL and the log file follow the config without a reboot */
            if (g_cfg.hostname[0]) sethostname(g_cfg.hostname, strlen(g_cfg.hostname));
            supervisor_reconcile();
        }
        sleep(1);
    }

    LOGI("boot", "shutting down");
    g_btn_run = 0;
    if (btn_rc == 0) pthread_join(g_btn_t, NULL);   /* joining a thread that never started is undefined */
    supervisor_stop_all();
    bridge_stop();
    gadget_stop();
    log_close();
    return 0;
}

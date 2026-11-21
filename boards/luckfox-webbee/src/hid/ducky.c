/*
 * DuckyScript interpreter. Subset consistent with Hak5 1.0/2.0:
 *   STRING <text>           - type text, resolve each char via layout
 *   DELAY <ms>              - sleep
 *   ENTER                   - press return
 *   GUI r / WINDOWS r       - Win+r
 *   CTRL / ALT / SHIFT      - modifier combos: "CTRL ALT DEL", "GUI SPACE", ...
 *   TAB / ESC / UP/DOWN/LEFT/RIGHT / F1..F12
 *   REPEAT <n>              - repeat last line n times
 *   REM                     - comment
 *   DEFAULTDELAY <ms>       - between every subsequent command
 */

#include "hid.h"
#include "layouts.h"
#include "../core/config.h"
#include "../common/log.h"

#include <ctype.h>
#include <errno.h>
#include <fcntl.h>
#include <pthread.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>
#include <sys/stat.h>
#include <time.h>
#include <unistd.h>

#define HID_DEV "/dev/hidg0"

static int hid_fd = -1;
static int layout_loaded = 0;   /* set once layout_load has succeeded */
static pthread_mutex_t hid_payload_mtx = PTHREAD_MUTEX_INITIALIZER;   /* one payload at a time */

static int hid_open(void) {
    if (hid_fd >= 0) return 0;
    hid_fd = open(HID_DEV, O_RDWR);
    if (hid_fd < 0) { LOGW("hid", "open %s: %s", HID_DEV, strerror(errno)); return -1; }
    return 0;
}

static int hid_write_err = 0;   /* set when a write() to the HID device fails; reset per payload */

static void hid_press(uint8_t mod, uint8_t key) {
    if (hid_fd < 0) { hid_write_err = 1; return; }
    uint8_t rep[8] = { mod, 0, key, 0, 0, 0, 0, 0 };
    if (write(hid_fd, rep, 8) != 8) hid_write_err = 1;
    uint8_t zero[8] = {0};
    if (write(hid_fd, zero, 8) != 8) hid_write_err = 1;
}

static void hid_combo(uint8_t mod, uint8_t key1, uint8_t key2) {
    if (hid_fd < 0) { hid_write_err = 1; return; }
    uint8_t rep[8] = { mod, 0, key1, key2, 0, 0, 0, 0 };
    if (write(hid_fd, rep, 8) != 8) hid_write_err = 1;
    uint8_t zero[8] = {0};
    if (write(hid_fd, zero, 8) != 8) hid_write_err = 1;
}

static void msleep(int ms) {
    struct timespec ts = { ms / 1000, (ms % 1000) * 1000000L };
    nanosleep(&ts, NULL);
}

/* keyname -> HID */
static uint8_t key_of(const char *name, uint8_t *mod_out) {
    *mod_out = 0;
    static const struct { const char *n; uint8_t k; uint8_t m; } tbl[] = {
        {"ENTER", 0x28, 0},  {"RETURN", 0x28, 0},
        {"ESC", 0x29, 0},    {"ESCAPE", 0x29, 0},
        {"TAB", 0x2B, 0},    {"SPACE", 0x2C, 0},
        {"BACKSPACE", 0x2A, 0}, {"DELETE", 0x4C, 0}, {"DEL", 0x4C, 0},
        {"UP", 0x52, 0}, {"DOWN", 0x51, 0}, {"LEFT", 0x50, 0}, {"RIGHT", 0x4F, 0},
        {"HOME", 0x4A, 0}, {"END", 0x4D, 0},
        {"PAGEUP", 0x4B, 0}, {"PAGEDOWN", 0x4E, 0},
        {"INSERT", 0x49, 0},
        {"F1", 0x3A, 0}, {"F2", 0x3B, 0}, {"F3", 0x3C, 0}, {"F4", 0x3D, 0},
        {"F5", 0x3E, 0}, {"F6", 0x3F, 0}, {"F7", 0x40, 0}, {"F8", 0x41, 0},
        {"F9", 0x42, 0}, {"F10", 0x43, 0}, {"F11", 0x44, 0}, {"F12", 0x45, 0},
        {"PRINTSCREEN", 0x46, 0}, {"CAPSLOCK", 0x39, 0},
        {NULL, 0, 0}
    };
    for (int i = 0; tbl[i].n; i++) {
        if (!strcasecmp(tbl[i].n, name)) { *mod_out = tbl[i].m; return tbl[i].k; }
    }
    /* single char */
    if (strlen(name) == 1) {
        const layout_entry_t *e = layout_find_ch(name[0]);
        if (e) { *mod_out = e->mod; return e->key; }
    }
    const layout_entry_t *e = layout_find(name);
    if (e) { *mod_out = e->mod; return e->key; }
    return 0;
}

static uint8_t mod_bit(const char *tok) {
    if (!strcasecmp(tok, "CTRL") || !strcasecmp(tok, "CONTROL")) return 0x01;
    if (!strcasecmp(tok, "SHIFT"))                                return 0x02;
    if (!strcasecmp(tok, "ALT"))                                  return 0x04;
    if (!strcasecmp(tok, "GUI") || !strcasecmp(tok, "WINDOWS") ||
        !strcasecmp(tok, "WIN") || !strcasecmp(tok, "SUPER") ||
        !strcasecmp(tok, "COMMAND"))                              return 0x08;
    return 0;
}

static void type_string(const char *s, int inter_ms) {
    for (; *s; s++) {
        const layout_entry_t *e = layout_find_ch(*s);
        if (e) hid_press(e->mod, e->key);
        if (inter_ms) msleep(inter_ms);
    }
}

int hid_run_payload(const char *path) {
    pthread_mutex_lock(&hid_payload_mtx);
    hid_write_err = 0;
    if (hid_open() != 0) { pthread_mutex_unlock(&hid_payload_mtx); return -1; }
    FILE *f = fopen(path, "r");
    if (!f) { LOGW("hid", "payload %s: %s", path, strerror(errno)); pthread_mutex_unlock(&hid_payload_mtx); return -1; }

    /* load layout; if this load fails, keep using a table loaded earlier */
    char lp[128];
    snprintf(lp, sizeof(lp), "/mnt/sd/Languages/%s.json", g_cfg.hid_layout);
    if (layout_load(lp) == 0) {
        layout_loaded = 1;
    } else if (!layout_loaded) {
        LOGE("hid", "layout %s failed and no layout table is loaded; payload not run", lp);
        fclose(f);
        pthread_mutex_unlock(&hid_payload_mtx);
        return -1;
    }

    int default_delay = 0;
    char line[2048];
    char last_line[2048] = {0};

    int first_line = 1;
    while (fgets(line, sizeof(line), f)) {
        size_t L = strlen(line);
        while (L && (line[L-1] == '\n' || line[L-1] == '\r')) line[--L] = 0;
        /* skip a UTF-8 byte-order mark at the very start of the file, then leading spaces and tabs */
        char *p = line;
        if (first_line && L >= 3 && !memcmp(p, "\xEF\xBB\xBF", 3)) p += 3;
        first_line = 0;
        while (*p == ' ' || *p == '\t') p++;
        if (p != line) memmove(line, p, strlen(p) + 1);
        if (!line[0]) continue;

        if (!strncasecmp(line, "REM ", 4) || line[0] == '#') continue;

        if (!strncasecmp(line, "DELAY ", 6)) {
            msleep(atoi(line + 6));
            continue;
        }
        if (!strncasecmp(line, "DEFAULTDELAY ", 13)) {
            default_delay = atoi(line + 13);
            continue;
        }
        if (!strncasecmp(line, "STRING ", 7)) {
            type_string(line + 7, 0);
            if (default_delay) msleep(default_delay);
            strncpy(last_line, line, sizeof(last_line)-1); last_line[sizeof(last_line)-1]=0;
            continue;
        }
        if (!strncasecmp(line, "REPEAT ", 7)) {
            int n = atoi(line + 7);
            for (int i = 0; i < n && last_line[0]; i++) {
                /* re-interpret */
                FILE *mem = fmemopen(last_line, strlen(last_line), "r");
                if (mem) {
                    char b[2048];
                    if (fgets(b, sizeof(b), mem)) {
                        /* recursive-light: pretend file has just this line */
                    }
                    fclose(mem);
                }
                /* simpler: just re-dispatch exactly what we would do */
                /* NB: for brevity we handle STRING repeat only. */
                if (!strncasecmp(last_line, "STRING ", 7)) type_string(last_line + 7, 0);
            }
            if (default_delay) msleep(default_delay);
            continue;
        }

        /* key combo path: tokens separated by spaces */
        uint8_t mod = 0, keys[2] = {0, 0}; int ki = 0;
        int have_mod_word = 0;   /* keys after a modifier word ignore their layout entry's mod bits */
        char *save = NULL;
        char tmp[sizeof(line)];
        strncpy(tmp, line, sizeof(tmp)-1); tmp[sizeof(tmp)-1] = 0;
        for (char *tok = strtok_r(tmp, " \t", &save); tok; tok = strtok_r(NULL, " \t", &save)) {
            uint8_t m = mod_bit(tok);
            if (m) { mod |= m; have_mod_word = 1; continue; }
            uint8_t extra_mod;
            uint8_t k = key_of(tok, &extra_mod);
            if (k) {
                if (!have_mod_word) mod |= extra_mod;
                if (ki < 2) keys[ki++] = k;
            }
        }
        if (ki > 0) {
            hid_combo(mod, keys[0], keys[1]);
            if (default_delay) msleep(default_delay);
            strncpy(last_line, line, sizeof(last_line)-1); last_line[sizeof(last_line)-1]=0;
        }
    }
    fclose(f);
    int failed = hid_write_err;   /* read while locked: the next payload resets the flag */
    pthread_mutex_unlock(&hid_payload_mtx);
    if (failed) return -1;
    return 0;
}

int hid_start(void) {
    if (!g_cfg.hid_enabled) return 0;
    if (hid_open() < 0) {
        LOGW("hid", "/dev/hidg0 missing — HID disabled this boot");
        return -1;
    }
    LOGI("hid", "ready, layout=%s trigger=%s", g_cfg.hid_layout, g_cfg.hid_trigger);
    /* auto-run on boot? */
    if (!strcasecmp(g_cfg.hid_trigger, "BOOT") && g_cfg.hid_payload[0]) {
        msleep(g_cfg.hid_delay_ms);
        char full[256];
        if (g_cfg.hid_payload[0] == '/')
            snprintf(full, sizeof(full), "/mnt/sd%s", g_cfg.hid_payload);
        else
            snprintf(full, sizeof(full), "/mnt/sd/Payloads/%s", g_cfg.hid_payload);
        hid_run_payload(full);
    }
    return 0;
}

int hid_stop(void) {
    if (hid_fd >= 0) { close(hid_fd); hid_fd = -1; }
    return 0;
}

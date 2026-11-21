/*
 * Keyboard layout loader. Reads /Languages/<layout>.json in the same format
 * used by github.com/hak5/usbrubberducky-payloads/languages — a JSON object
 * mapping character / keyname -> HID key array.
 *
 * We parse a limited subset (just enough): {"a":[0,4], "A":[2,4], ...}.
 * Modifier byte is index 0 (shift=0x02, altgr=0x40, etc), keycode byte is 1.
 * Dead keys / multi-codepoint entries use additional pairs after.
 */

#include "layouts.h"
#include "../common/log.h"

#include <ctype.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static layout_entry_t g_table[LAYOUT_MAX];
static int            g_n = 0;
static char           g_name[16] = {0};

/* dumb JSON extractor — reads one top-level object with string-key -> array value */
int layout_load(const char *path) {
    FILE *f = fopen(path, "r");
    if (!f) { LOGW("hid", "layout %s: no file", path); return -1; }
    char *buf = malloc(65536);
    if (!buf) { fclose(f); return -1; }
    size_t n = fread(buf, 1, 65535, f);
    buf[n] = 0;
    fclose(f);

    g_n = 0;
    const char *p = buf;
    while (*p && g_n < LAYOUT_MAX) {
        /* find next "key": */
        while (*p && *p != '"') p++;
        if (!*p) break;
        p++;
        /* key text, with JSON escapes decoded: \" and \\ stand for the quote and the backslash */
        char kbuf[32]; size_t klen = 0; int bad_key = 0;
        while (*p && *p != '"') {
            char dec = *p++;
            if (dec == '\\' && *p) {
                char esc = *p++;
                switch (esc) {
                    case 'n': dec = '\n'; break;
                    case 't': dec = '\t'; break;
                    case 'r': dec = '\r'; break;
                    case 'b': dec = '\b'; break;
                    case 'f': dec = '\f'; break;
                    case 'u': {   /* \uXXXX: only the ASCII range can match a one-byte key */
                        unsigned code = 0; int ok = 1;
                        for (int k = 0; k < 4; k++) {
                            char h = *p;
                            if (!isxdigit((unsigned char)h)) { ok = 0; break; }
                            code = code * 16 + (unsigned)(isdigit((unsigned char)h) ? h - '0' : tolower((unsigned char)h) - 'a' + 10);
                            p++;
                        }
                        dec = (ok && code < 0x80) ? (char)code : 0;
                        break;
                    }
                    default: dec = esc; break;   /* \" \\ \/ stand for themselves */
                }
                if (dec == 0) { bad_key = 1; continue; }   /* a non-ASCII or malformed escape cannot match a key: the entry is skipped */
            }
            if (klen < sizeof(kbuf) - 1) kbuf[klen++] = dec;
        }
        if (!*p) break;
        kbuf[klen] = 0;
        p++;
        /* skip spaces, colon */
        while (*p == ' ' || *p == ':' || *p == '\t') p++;
        if (*p != '[') continue;
        p++;
        int vals[8]; int vn = 0;
        while (*p && *p != ']') {
            while (*p == ' ' || *p == ',') p++;
            if (*p == ']') break;
            vals[vn++] = (int)strtol(p, (char **)&p, 0);
            if (vn >= 8) break;
        }
        if (*p == ']') p++;

        if (bad_key) continue;   /* bad key escape flagged above: skip the entry, store nothing */
        layout_entry_t *e = &g_table[g_n++];
        if (klen >= sizeof(e->name)) klen = sizeof(e->name)-1;
        memcpy(e->name, kbuf, klen); e->name[klen] = 0;
        e->mod = (vn > 0) ? (uint8_t)vals[0] : 0;
        e->key = (vn > 1) ? (uint8_t)vals[1] : 0;
    }
    free(buf);

    const char *slash = strrchr(path, '/');
    const char *n2 = slash ? slash+1 : path;
    strncpy(g_name, n2, sizeof(g_name)-1); g_name[sizeof(g_name)-1] = 0;
    char *dot = strrchr(g_name, '.'); if (dot) *dot = 0;

    LOGI("hid", "layout '%s' loaded, %d entries", g_name, g_n);
    return 0;
}

const layout_entry_t *layout_find(const char *name) {
    for (int i = 0; i < g_n; i++) {
        if (strcmp(g_table[i].name, name) == 0) return &g_table[i];
    }
    return NULL;
}

/* Translate a single char using layout table */
const layout_entry_t *layout_find_ch(char c) {
    char s[2] = { c, 0 };
    return layout_find(s);
}

const char *layout_name(void) { return g_name[0] ? g_name : "us"; }

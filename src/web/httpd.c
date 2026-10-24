/*
 * A tiny self-contained HTTP(S-less) server for the WebUI + /api/*.
 * ~500 lines, single-threaded accept + per-request thread. Enough for one
 * admin at a time, which is all we ever expect.
 *
 * Features:
 *   - Static files from /mnt/sd/WebUI (user override) then embedded fallback.
 *   - /api/status      - firmware + config snapshot (JSON)
 *   - /api/config      - GET (current), POST (hot-reload after save)
 *   - /api/flows       - live flow table
 *   - /api/verdicts    - stream of recent verdicts (SSE)
 *   - /api/records     - list rotated pcap files
 *   - /api/query       - search verdict stream (host=, rule=)
 *   - /api/hid         - POST {"payload": "..."} to fire a ducky payload
 *   - /api/reboot      - POST reboot the device
 *   - Basic auth via WEBUI_USER/WEBUI_PASS or Bearer API_TOKEN
 */

#include "httpd.h"
#include "../core/config.h"
#include "../common/log.h"
#include "../hid/hid.h"

#include <arpa/inet.h>
#include <ctype.h>
#include <dirent.h>
#include <errno.h>
#include <fcntl.h>
#include <netinet/in.h>
#include <pthread.h>
#include <stdint.h>
#include <stdio.h>
#include <sys/reboot.h>
#include <linux/reboot.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>
#include <sys/socket.h>
#include <sys/stat.h>
#include <sys/types.h>
#include <unistd.h>

extern const char *EMBEDDED_INDEX_HTML;  /* defined in pages_embed.c */
extern const char *EMBEDDED_APP_JS;
extern const char *EMBEDDED_STYLE_CSS;

static volatile int g_run = 0;
static int          g_listen = -1;
static pthread_t    g_t;

/* Largest request body accepted. The shipped config.txt is about 9 KB. */
#define REQ_BODY_MAX (64 * 1024)

/* base64 encode (no newlines) */
static void b64enc(const uint8_t *in, size_t n, char *out) {
    static const char tbl[] = "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789+/";
    size_t i = 0, j = 0;
    while (i + 3 <= n) {
        uint32_t v = (in[i]<<16) | (in[i+1]<<8) | in[i+2];
        out[j++] = tbl[(v>>18) & 63];
        out[j++] = tbl[(v>>12) & 63];
        out[j++] = tbl[(v>>6)  & 63];
        out[j++] = tbl[ v      & 63];
        i += 3;
    }
    if (i < n) {
        uint32_t v = in[i] << 16;
        if (i+1 < n) v |= in[i+1] << 8;
        out[j++] = tbl[(v>>18) & 63];
        out[j++] = tbl[(v>>12) & 63];
        out[j++] = (i+1 < n) ? tbl[(v>>6) & 63] : '=';
        out[j++] = '=';
    }
    out[j] = 0;
}

static int url_decode(char *s) {
    char *d = s;
    while (*s) {
        if (*s == '+') { *d++ = ' '; s++; }
        else if (*s == '%' && isxdigit((unsigned char)s[1]) && isxdigit((unsigned char)s[2])) {
            char hex[3] = { s[1], s[2], 0 };
            *d++ = (char)strtol(hex, NULL, 16);
            s += 3;
        } else {
            *d++ = *s++;
        }
    }
    *d = 0;
    return 0;
}

static int auth_ok(const char *req) {
    /* Bearer token takes precedence */
    const char *at = strcasestr(req, "Authorization:");
    if (!at) return 0;
    at += 14; while (*at == ' ') at++;
    if (!strncasecmp(at, "Bearer ", 7)) {
        at += 7;
        const char *end = at;
        while (*end && *end != '\r' && *end != '\n') end++;
        size_t L = (size_t)(end - at);
        if (g_cfg.api_token[0] &&
            L == strlen(g_cfg.api_token) &&
            !memcmp(at, g_cfg.api_token, L)) return 1;
    }
    if (!strncasecmp(at, "Basic ", 6) && g_cfg.webui_pass[0]) {
        char expect[128];
        snprintf(expect, sizeof(expect), "%s:%s", g_cfg.webui_user, g_cfg.webui_pass);
        char enc[200];
        b64enc((const uint8_t *)expect, strlen(expect), enc);
        const char *given = at + 6;
        const char *end = given;
        while (*end && *end != '\r' && *end != '\n' && *end != ' ') end++;
        size_t L = (size_t)(end - given);
        if (L == strlen(enc) && !memcmp(given, enc, L)) return 1;
    }
    return 0;
}

static void send_hdr(int c, int code, const char *msg, const char *ctype, size_t clen,
                     const char *extra) {
    char h[512];
    int n = snprintf(h, sizeof(h),
        "HTTP/1.1 %d %s\r\n"
        "Server: GhostLink\r\n"
        "Content-Type: %s\r\n"
        "Content-Length: %zu\r\n"
        "Cache-Control: no-store\r\n"
        "%s\r\n",
        code, msg, ctype, clen, extra ? extra : "");
    write(c, h, (size_t)n);
}

static void send_json(int c, int code, const char *body) {
    size_t n = strlen(body);
    send_hdr(c, code, code == 200 ? "OK" : "ERR", "application/json", n, NULL);
    write(c, body, n);
}

static void send_text(int c, int code, const char *body) {
    size_t n = strlen(body);
    send_hdr(c, code, code == 200 ? "OK" : "ERR", "text/plain", n, NULL);
    write(c, body, n);
}

static void send_file(int c, const char *path, const char *ctype) {
    int fd = open(path, O_RDONLY);
    if (fd < 0) { send_text(c, 404, "not found\n"); return; }
    struct stat st;
    if (fstat(fd, &st) < 0) { close(fd); send_text(c, 500, "stat\n"); return; }
    send_hdr(c, 200, "OK", ctype, (size_t)st.st_size, NULL);
    char buf[8192];
    ssize_t n;
    while ((n = read(fd, buf, sizeof(buf))) > 0) write(c, buf, (size_t)n);
    close(fd);
}

static void send_401(int c) {
    const char *b = "auth required\n";
    size_t n = strlen(b);
    send_hdr(c, 401, "Unauthorized", "text/plain", n,
             "WWW-Authenticate: Basic realm=\"GhostLink\"\r\n");
    write(c, b, n);
}

/* ---------- route handlers ---------- */

/* Escapes src for a JSON string literal. Only whole escapes are written, so
 * the result is valid JSON even when it is cut to fit cap. */
static void json_esc(char *dst, size_t cap, const char *src) {
    size_t o = 0;
    for (const unsigned char *p = (const unsigned char *)src; *p && o + 7 < cap; p++) {
        if (*p == '"' || *p == '\\') { dst[o++] = '\\'; dst[o++] = (char)*p; }
        else if (*p < 0x20) o += (size_t)snprintf(dst + o, cap - o, "\\u%04x", (unsigned)*p);
        else dst[o++] = (char)*p;
    }
    dst[o] = 0;
}

static void r_status(int c) {
    char name[384], host[384], netmode[128], usb[128];
    json_esc(name, sizeof(name), g_cfg.device_name);
    json_esc(host, sizeof(host), g_cfg.hostname);
    json_esc(netmode, sizeof(netmode), g_cfg.netmode);
    json_esc(usb, sizeof(usb), g_cfg.usb_mode);
    char body[2048];
    int n = snprintf(body, sizeof(body),
        "{\"name\":\"%s\",\"hostname\":\"%s\","
        "\"netmode\":\"%s\",\"passthrough\":%s,\"usb_mode\":\"%s\","
        "\"recording\":%s,\"verdict_stream\":%s,"
        "\"deadnet\":%s,\"redirect\":%s,\"honeypot\":%s,\"hid\":%s,"
        "\"cfg_gen\":%llu}",
        name, host, netmode,
        g_cfg.passthrough_lan ? "true":"false",
        usb,
        g_cfg.log_to_sd ? "true":"false",
        g_cfg.verdict_stream ? "true":"false",
        g_cfg.deadnet ? "true":"false",
        g_cfg.redirect ? "true":"false",
        g_cfg.honeypot ? "true":"false",
        g_cfg.hid_enabled ? "true":"false",
        (unsigned long long)g_cfg_gen);
    (void)n;
    send_json(c, 200, body);
}

static void r_config_get(int c) {
    /* Dump config.txt verbatim */
    FILE *f = fopen("/mnt/sd/config.txt", "r");
    if (!f) { send_text(c, 404, "no config.txt\n"); return; }
    fseek(f, 0, SEEK_END);
    long L = ftell(f);
    fseek(f, 0, SEEK_SET);
    char *buf = malloc((size_t)L + 1);
    fread(buf, 1, (size_t)L, f);
    buf[L] = 0;
    fclose(f);
    send_hdr(c, 200, "OK", "text/plain", (size_t)L, NULL);
    write(c, buf, (size_t)L);
    free(buf);
}

static void r_config_post(int c, const char *body, size_t len) {
    /* An empty body must never replace a working config. */
    if (!body || len == 0) { send_json(c, 400, "{\"ok\":false}"); return; }
    const char *tmp = "/mnt/sd/config.txt.new";
    FILE *f = fopen(tmp, "w");
    if (!f) { send_text(c, 500, "write fail\n"); return; }
    int ok = (fwrite(body, 1, len, f) == len);
    if (fflush(f) != 0) ok = 0;
    if (fsync(fileno(f)) != 0) ok = 0;
    if (fclose(f) != 0) ok = 0;
    if (!ok) { unlink(tmp); send_json(c, 500, "{\"ok\":false}"); return; }
    /* rename only after the complete body is on disk */
    if (rename(tmp, "/mnt/sd/config.txt") != 0) {
        unlink(tmp);
        send_json(c, 500, "{\"ok\":false}");
        return;
    }
    cfg_load("/mnt/sd/config.txt");
    send_json(c, 200, "{\"ok\":true}");
}

static void r_records(int c) {
    char body[8192]; int off = 0;
    off += snprintf(body + off, sizeof(body) - off, "[");
    DIR *d = opendir("/mnt/sd/Records");
    int first = 1;
    if (d) {
        struct dirent *e;
        while ((e = readdir(d))) {
            if (!strstr(e->d_name, ".pcap")) continue;
            char full[384];
            snprintf(full, sizeof(full), "/mnt/sd/Records/%s", e->d_name);
            struct stat st;
            if (stat(full, &st) == 0) {
                off += snprintf(body + off, sizeof(body) - off,
                    "%s{\"name\":\"%s\",\"size\":%lld,\"mtime\":%lld}",
                    first ? "" : ",", e->d_name,
                    (long long)st.st_size, (long long)st.st_mtime);
                first = 0;
                if (off > (int)sizeof(body) - 128) break;
            }
        }
        closedir(d);
    }
    snprintf(body + off, sizeof(body) - off, "]");
    send_json(c, 200, body);
}

static void r_query(int c, const char *qs) {
    /* Return up to N verdict lines matching key=value filters */
    char host[128] = {0}, rule[32] = {0};
    char tmp[256]; strncpy(tmp, qs ? qs : "", sizeof(tmp)-1); tmp[sizeof(tmp)-1] = 0;
    url_decode(tmp);
    for (char *p = tmp, *tok; (tok = strtok_r(p, "&", &p));) {
        char *eq = strchr(tok, '=');
        if (!eq) continue;
        *eq = 0;
        if (!strcmp(tok, "host")) { strncpy(host, eq+1, sizeof(host)-1); host[sizeof(host)-1]=0; }
        if (!strcmp(tok, "rule")) { strncpy(rule, eq+1, sizeof(rule)-1); rule[sizeof(rule)-1]=0; }
    }

    /* Chunked reply: no Content-Length, the chunk sizes frame the body. */
    static const char chunked_hdr[] =
        "HTTP/1.1 200 OK\r\n"
        "Server: GhostLink\r\n"
        "Content-Type: application/json\r\n"
        "Transfer-Encoding: chunked\r\n"
        "Cache-Control: no-store\r\n"
        "\r\n";
    write(c, chunked_hdr, sizeof(chunked_hdr) - 1);
    /* write chunked JSON array */
    write(c, "1\r\n[\r\n", 6);
    int emitted = 0;

    DIR *d = opendir("/mnt/sd/Records");
    if (d) {
        struct dirent *e;
        while ((e = readdir(d))) {
            if (!strstr(e->d_name, ".jsonl")) continue;
            char full[384];
            snprintf(full, sizeof(full), "/mnt/sd/Records/%s", e->d_name);
            FILE *f = fopen(full, "r");
            if (!f) continue;
            char line[1024];
            while (fgets(line, sizeof(line), f) && emitted < 500) {
                if (host[0] && !strstr(line, host)) continue;
                if (rule[0] && !strstr(line, rule)) continue;
                size_t L = strlen(line);
                while (L && (line[L - 1] == '\n' || line[L - 1] == '\r')) line[--L] = 0;
                if (!L) continue;
                /* comma before every element except the first: valid JSON */
                char out[1100];
                int ol = snprintf(out, sizeof(out), "%s%s\n", emitted ? "," : "", line);
                char chunk[32];
                int ch = snprintf(chunk, sizeof(chunk), "%x\r\n", (unsigned)ol);
                write(c, chunk, (size_t)ch);
                write(c, out, (size_t)ol);
                write(c, "\r\n", 2);
                emitted++;
            }
            fclose(f);
            if (emitted >= 500) break;
        }
        closedir(d);
    }
    /* close array (overwrite last comma logically with "]") */
    write(c, "1\r\n]\r\n0\r\n\r\n", 11);
}

static void r_hid(int c, const char *body) {
    if (!body || !*body) { send_text(c, 400, "no body\n"); return; }
    /* body: "/Payloads/x.ducky" or raw payload. Treat as path first. */
    char path[256];
    if (body[0] == '/') snprintf(path, sizeof(path), "/mnt/sd%s", body);
    else                snprintf(path, sizeof(path), "/mnt/sd/Payloads/%s", body);
    if (hid_run_payload(path) == 0) send_json(c, 200, "{\"ok\":true}");
    else                            send_json(c, 500, "{\"ok\":false}");
}

static void r_reboot(int c) {
    send_json(c, 200, "{\"ok\":true}");
    close(c);
    sync();
    reboot(RB_AUTOBOOT);
}

/* Content-Length from the header block, or 0 when absent. Values above
 * REQ_BODY_MAX come back as REQ_BODY_MAX + 1 so the caller can refuse them. */
static size_t content_length(const char *req) {
    const char *p = strcasestr(req, "\r\nContent-Length:");
    if (!p) return 0;
    p += 17;
    while (*p == ' ' || *p == '\t') p++;
    char *end = NULL;
    unsigned long long v = strtoull(p, &end, 10);
    if (end == p) return 0;
    return v > REQ_BODY_MAX ? (size_t)REQ_BODY_MAX + 1 : (size_t)v;
}

/* ---------- request dispatch ---------- */

static void *req_thread(void *arg) {
    int c = (int)(intptr_t)arg;
    char buf[8192];
    size_t n = 0;
    /* Read until the header block ends or the buffer is full. */
    while (n < sizeof(buf) - 1) {
        ssize_t r = recv(c, buf + n, sizeof(buf) - 1 - n, 0);
        if (r <= 0) break;
        n += (size_t)r;
        buf[n] = 0;
        if (strstr(buf, "\r\n\r\n")) break;
    }
    if (n == 0) { close(c); return NULL; }
    buf[n] = 0;

    /* parse request line */
    char method[8]  = {0};
    char path[1024] = {0};
    sscanf(buf, "%7s %1023s", method, path);

    if (!auth_ok(buf) && strcmp(path, "/favicon.ico") != 0) {
        send_401(c);
        close(c); return NULL;
    }

    char *qs = strchr(path, '?');
    if (qs) { *qs = 0; qs++; }

    /* body: exactly Content-Length bytes, read in full */
    char *body = NULL;
    size_t blen = 0;
    const char *hdr_end = strstr(buf, "\r\n\r\n");
    if (hdr_end) {
        size_t want = content_length(buf);
        if (want > REQ_BODY_MAX) { send_text(c, 413, "body too large\n"); close(c); return NULL; }
        size_t have = n - (size_t)(hdr_end + 4 - buf);
        if (have > want) have = want;
        body = malloc(want + 1);
        if (!body) { send_text(c, 500, "out of memory\n"); close(c); return NULL; }
        memcpy(body, hdr_end + 4, have);
        while (have < want) {
            ssize_t r = recv(c, body + have, want - have, 0);
            if (r <= 0) break;
            have += (size_t)r;
        }
        if (have < want) { free(body); send_text(c, 400, "short body\n"); close(c); return NULL; }
        body[want] = 0;
        blen = want;
    }

    if      (!strcmp(path, "/api/status"))  r_status(c);
    else if (!strcmp(path, "/api/config") && !strcmp(method, "GET"))  r_config_get(c);
    else if (!strcmp(path, "/api/config") && !strcmp(method, "POST")) r_config_post(c, body, blen);
    else if (!strcmp(path, "/api/records"))  r_records(c);
    else if (!strcmp(path, "/api/query"))    r_query(c, qs);
    else if (!strcmp(path, "/api/hid") && !strcmp(method, "POST"))    r_hid(c, body);
    else if (!strcmp(path, "/api/reboot") && !strcmp(method, "POST")) r_reboot(c);
    else if (!strcmp(path, "/") || !strcmp(path, "/index.html")) {
        /* prefer user override */
        if (access("/mnt/sd/WebUI/index.html", R_OK) == 0) {
            send_file(c, "/mnt/sd/WebUI/index.html", "text/html");
        } else {
            send_hdr(c, 200, "OK", "text/html", strlen(EMBEDDED_INDEX_HTML), NULL);
            write(c, EMBEDDED_INDEX_HTML, strlen(EMBEDDED_INDEX_HTML));
        }
    } else if (!strcmp(path, "/app.js")) {
        if (access("/mnt/sd/WebUI/app.js", R_OK) == 0) send_file(c, "/mnt/sd/WebUI/app.js", "application/javascript");
        else {
            send_hdr(c, 200, "OK", "application/javascript", strlen(EMBEDDED_APP_JS), NULL);
            write(c, EMBEDDED_APP_JS, strlen(EMBEDDED_APP_JS));
        }
    } else if (!strcmp(path, "/style.css")) {
        if (access("/mnt/sd/WebUI/style.css", R_OK) == 0) send_file(c, "/mnt/sd/WebUI/style.css", "text/css");
        else {
            send_hdr(c, 200, "OK", "text/css", strlen(EMBEDDED_STYLE_CSS), NULL);
            write(c, EMBEDDED_STYLE_CSS, strlen(EMBEDDED_STYLE_CSS));
        }
    } else {
        send_text(c, 404, "not found\n");
    }
    free(body);
    close(c);
    return NULL;
}

static void *accept_loop(void *arg) {
    int lfd = (int)(intptr_t)arg;   /* this start's socket, not the global */
    while (g_run) {
        struct sockaddr_in a;
        socklen_t al = sizeof(a);
        int c = accept(lfd, (struct sockaddr *)&a, &al);
        if (c < 0) { if (errno == EINTR) continue; break; }
        pthread_t t;
        pthread_create(&t, NULL, req_thread, (void *)(intptr_t)c);
        pthread_detach(t);
    }
    return NULL;
}

int httpd_start(void) {
    if (!g_cfg.webui) return 0;
    /* No credentials means no WebUI: a blank password must never be accepted. */
    if (!g_cfg.webui_pass[0] && !g_cfg.api_token[0]) {
        LOGE("web", "WebUI not started: set WEBUI_PASS or API_TOKEN in config.txt");
        return -1;
    }
    g_listen = socket(AF_INET, SOCK_STREAM, 0);
    int one = 1;
    setsockopt(g_listen, SOL_SOCKET, SO_REUSEADDR, &one, sizeof(one));

    struct sockaddr_in a;
    memset(&a, 0, sizeof(a));
    a.sin_family = AF_INET;
    a.sin_port   = htons((uint16_t)g_cfg.webui_port);
    inet_pton(AF_INET, g_cfg.webui_bind, &a.sin_addr);
    if (bind(g_listen, (struct sockaddr *)&a, sizeof(a)) < 0) {
        LOGE("web", "bind %s:%d: %s", g_cfg.webui_bind, g_cfg.webui_port, strerror(errno));
        close(g_listen); g_listen = -1; return -1;
    }
    listen(g_listen, 16);
    g_run = 1;
    pthread_create(&g_t, NULL, accept_loop, (void *)(intptr_t)g_listen);
    LOGI("web", "listening on %s:%d", g_cfg.webui_bind, g_cfg.webui_port);
    return 0;
}

int httpd_stop(void) {
    g_run = 0;
    if (g_listen >= 0) {
        /* shutdown() wakes the accept() in accept_loop; close() alone does not. */
        shutdown(g_listen, SHUT_RDWR);
        close(g_listen);
        g_listen = -1;
        pthread_join(g_t, NULL);
    }
    return 0;
}

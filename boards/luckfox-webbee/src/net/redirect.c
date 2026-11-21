/*
 * Redirect engine. Two mechanisms depending on REDIRECT_PROTO:
 *
 *  DNS_ONLY  - answer A/AAAA for REDIRECT_FROM with the IP of REDIRECT_TO.
 *              Works for everything, HTTPS included (client will see cert
 *              mismatch; this is the "steer" primitive, not a strip).
 *  HTTP      - run a tiny HTTP server on 127.0.0.1:8080 that returns 302 to
 *              REDIRECT_TO; nftables REDIRECTs matched traffic there.
 *  AUTO      - DNS answers point to our own IP, we serve 302 for cleartext
 *              and let TLS fail loud (user will notice, which is often the point).
 */

#include "redirect.h"
#include "../core/config.h"
#include "../common/log.h"

#include <arpa/inet.h>
#include <ctype.h>
#include <errno.h>
#include <netdb.h>
#include <netinet/in.h>
#include <pthread.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/socket.h>
#include <sys/time.h>
#include <unistd.h>

static volatile int g_run = 0;
static pthread_t    g_dns_t, g_http_t;

static int in_from_list(const char *host) {
    if (!g_cfg.redirect_from[0]) return 0;
    char buf[512];
    strncpy(buf, g_cfg.redirect_from, sizeof(buf)-1);
    buf[sizeof(buf)-1] = 0;
    char *tok, *save = NULL;
    for (tok = strtok_r(buf, ",", &save); tok; tok = strtok_r(NULL, ",", &save)) {
        /* trim spaces and tabs around the token */
        while (*tok == ' ' || *tok == '\t') tok++;
        size_t tl = strlen(tok);
        while (tl && (tok[tl-1] == ' ' || tok[tl-1] == '\t')) tok[--tl] = 0;
        /* strip scheme, case-insensitive */
        if (!strncasecmp(tok, "http://", 7))  tok += 7;
        if (!strncasecmp(tok, "https://", 8)) tok += 8;
        if (!*tok) continue;
        if (!strcasecmp(tok, host)) return 1;
        size_t L = strlen(tok), H = strlen(host);
        if (tok[0] == '*' && tok[1] == '.' && tok[2]) {
            /* wildcard *.google.com: google.com itself, or any name ending in .google.com */
            const char *base = tok + 2;
            size_t B = L - 2;
            if (!strcasecmp(host, base)) return 1;
            if (B < H && !strcasecmp(host + H - B, base) && host[H - B - 1] == '.') return 1;
            continue;
        }
        /* suffix match: google.com also matches www.google.com */
        if (L < H && !strcasecmp(host + H - L, tok) && host[H - L - 1] == '.') return 1;
    }
    return 0;
}

/* Minimal DNS. We implement just enough to parse ONE question and craft one answer. */
static int dns_parse_qname(const uint8_t *buf, size_t len, size_t off, char *out, size_t olen) {
    size_t pos = off, n = 0;
    while (pos < len && buf[pos]) {
        uint8_t L = buf[pos++];
        if (L > 63 || pos + L > len) return -1;
        if (n) { if (n >= olen) return -1; out[n++] = '.'; }
        for (int i = 0; i < L; i++) {
            if (n >= olen) return -1;
            out[n++] = buf[pos++];
        }
    }
    if (n >= olen) return -1;
    out[n] = 0;
    return (int)(pos + 1 - off); /* consumed */
}

static void *dns_worker(void *arg) {
    (void)arg;
    int s = socket(AF_INET, SOCK_DGRAM, 0);
    if (s < 0) { LOGE("redir", "dns sock: %s", strerror(errno)); return NULL; }
    int one = 1;
    setsockopt(s, SOL_SOCKET, SO_REUSEADDR, &one, sizeof(one));

    struct sockaddr_in a;
    memset(&a, 0, sizeof(a));
    a.sin_family = AF_INET;
    a.sin_port   = htons(53);
    a.sin_addr.s_addr = INADDR_ANY;
    if (bind(s, (struct sockaddr *)&a, sizeof(a)) < 0) {
        LOGE("redir", "bind :53 %s — redirect DNS disabled", strerror(errno));
        close(s); return NULL;
    }

    /* resolve REDIRECT_TO ip once */
    uint32_t target_ip = 0;
    const char *to = g_cfg.redirect_to;
    if (!strncasecmp(to, "http://",  7)) to += 7;
    if (!strncasecmp(to, "https://", 8)) to += 8;
    char host_only[128];
    strncpy(host_only, to, sizeof(host_only)-1); host_only[sizeof(host_only)-1] = 0;
    char *slash = strchr(host_only, '/'); if (slash) *slash = 0;
    struct in_addr ina;
    if (inet_aton(host_only, &ina)) target_ip = ina.s_addr;
    else {
        struct hostent *he = gethostbyname(host_only);
        if (he && he->h_addrtype == AF_INET)
            memcpy(&target_ip, he->h_addr_list[0], 4);
    }
    if (!target_ip) {
        LOGW("redir", "REDIRECT_TO '%s' not resolvable, dns worker exiting", g_cfg.redirect_to);
        close(s); return NULL;
    }

    LOGI("redir", "DNS redirector up, target=%s", inet_ntoa(*(struct in_addr *)&target_ip));

    uint8_t buf[1500];
    while (g_run) {
        struct sockaddr_in from;
        socklen_t fl = sizeof(from);
        ssize_t n = recvfrom(s, buf, sizeof(buf), 0, (struct sockaddr *)&from, &fl);
        if (n < 12) continue;
        if (buf[2] & 0x80) continue;     /* QR=1: a response, never answer one */

        /* parse first question */
        char qname[256];
        int consumed = dns_parse_qname(buf, (size_t)n, 12, qname, sizeof(qname));
        if (consumed < 0) continue;
        size_t qoff = 12 + consumed;
        if (qoff + 4 > (size_t)n) continue;
        uint16_t qtype = (uint16_t)(buf[qoff] << 8 | buf[qoff+1]);
        /* uint16_t qclass = (uint16_t)(buf[qoff+2] << 8 | buf[qoff+3]); */

        if (!in_from_list(qname)) {
            /* forward: do nothing — real DNS from upstream will handle it.
               Simplest: just drop. For a nicer deployment, forward to upstream. */
            continue;
        }

        if (qtype != 1 && qtype != 28) continue; /* A or AAAA */

        /* craft minimal response reusing the request */
        uint8_t resp[600];
        size_t rlen = qoff + 4;
        memcpy(resp, buf, rlen);
        resp[8] = 0; resp[9] = 0; resp[10] = 0; resp[11] = 0;  /* NSCOUNT=0, ARCOUNT=0: do not echo the query's EDNS OPT */
        resp[2] = 0x81; resp[3] = 0x80;  /* flags: QR=1, RD=1, RA=1 */
        resp[6] = 0; resp[7] = 1;        /* ANCOUNT=1 */

        /* answer: ptr to qname, type, class, TTL=60, rdlen, rdata */
        resp[rlen++] = 0xC0; resp[rlen++] = 0x0C;    /* name ptr -> qname */
        if (qtype == 1) {
            resp[rlen++] = 0; resp[rlen++] = 1;        /* type A */
            resp[rlen++] = 0; resp[rlen++] = 1;        /* class IN */
            resp[rlen++] = 0; resp[rlen++] = 0; resp[rlen++] = 0; resp[rlen++] = 60;
            resp[rlen++] = 0; resp[rlen++] = 4;
            memcpy(resp + rlen, &target_ip, 4); rlen += 4;
        } else {
            /* AAAA: return IPv4-mapped v6 */
            resp[rlen++] = 0; resp[rlen++] = 28;
            resp[rlen++] = 0; resp[rlen++] = 1;
            resp[rlen++] = 0; resp[rlen++] = 0; resp[rlen++] = 0; resp[rlen++] = 60;
            resp[rlen++] = 0; resp[rlen++] = 16;
            memset(resp + rlen, 0, 10); rlen += 10;
            resp[rlen++] = 0xFF; resp[rlen++] = 0xFF;
            memcpy(resp + rlen, &target_ip, 4); rlen += 4;
        }

        sendto(s, resp, rlen, 0, (struct sockaddr *)&from, fl);
        LOGD("redir", "DNS hit %s -> %s", qname, inet_ntoa(*(struct in_addr *)&target_ip));
    }
    close(s);
    return NULL;
}

static void *http_worker(void *arg) {
    (void)arg;
    int s = socket(AF_INET, SOCK_STREAM, 0);
    if (s < 0) return NULL;
    int one = 1;
    setsockopt(s, SOL_SOCKET, SO_REUSEADDR, &one, sizeof(one));
    struct sockaddr_in a;
    memset(&a, 0, sizeof(a));
    a.sin_family = AF_INET;
    a.sin_addr.s_addr = htonl(INADDR_ANY);
    a.sin_port = htons(8080);
    if (bind(s, (struct sockaddr *)&a, sizeof(a)) < 0 || listen(s, 16) < 0) {
        LOGW("redir", ":8080 bind: %s", strerror(errno));
        close(s); return NULL;
    }
    LOGI("redir", "HTTP 302-er up on :8080");

    while (g_run) {
        int c = accept(s, NULL, NULL);
        if (c < 0) continue;
        /* idle client must not hold the single HTTP worker: 3 s read timeout */
        struct timeval tv = { 3, 0 };
        setsockopt(c, SOL_SOCKET, SO_RCVTIMEO, &tv, sizeof(tv));
        char req[1024]; int n = read(c, req, sizeof(req)-1);
        if (n < 0) { close(c); continue; }
        req[n] = 0;
        /* get path */
        char *sp1 = strchr(req, ' ');
        char path[512] = "/";
        if (sp1) {
            sp1++;
            size_t L = strcspn(sp1, " \r\n");   /* path ends at the first space, CR or LF */
            if (L && L < sizeof(path)) { memcpy(path, sp1, L); path[L] = 0; }
        }

        char loc[255 + 511 + 1];   /* REDIRECT_TO (255) + path (511) + NUL */
        if (g_cfg.redirect_preserve_path && path[0] == '/' && path[1]) {
            snprintf(loc, sizeof(loc), "%s%s", g_cfg.redirect_to, path);
        } else {
            snprintf(loc, sizeof(loc), "%s", g_cfg.redirect_to);
        }
        /* ensure scheme in Location */
        char full[7 + 255 + 511 + 1];   /* "http://" + loc */
        if (!strncasecmp(g_cfg.redirect_to, "http://", 7) || !strncasecmp(g_cfg.redirect_to, "https://", 8))
            snprintf(full, sizeof(full), "%s", loc);
        else
            snprintf(full, sizeof(full), "http://%s", loc);

        char resp[900];
        int rlen = snprintf(resp, sizeof(resp),
            "HTTP/1.1 302 Found\r\n"
            "Location: %s\r\n"
            "Content-Length: 0\r\n"
            "Connection: close\r\n\r\n",
            full);
        write(c, resp, (size_t)rlen);
        close(c);
    }
    close(s);
    return NULL;
}

int redirect_start(void) {
    if (!g_cfg.redirect) { LOGI("redir", "disabled"); return 0; }
    if (!g_cfg.redirect_from[0] || !g_cfg.redirect_to[0]) {
        LOGW("redir", "REDIRECT=TRUE but FROM/TO empty");
        return -1;
    }
    g_run = 1;
    const char *proto = g_cfg.redirect_proto;
    int do_dns = !strcasecmp(proto, "DNS_ONLY") || !strcasecmp(proto, "AUTO");
    int do_http = !strcasecmp(proto, "HTTP") || !strcasecmp(proto, "AUTO");
    if (do_dns)  pthread_create(&g_dns_t,  NULL, dns_worker,  NULL);
    if (do_http) pthread_create(&g_http_t, NULL, http_worker, NULL);
    return 0;
}

int redirect_stop(void) { g_run = 0; return 0; }

/*
 * A ~200-line service supervisor. Not a replacement for systemd — the point is
 * that boot is <1s because we never fork a shell, never pipe through logs,
 * never wait on a socket. Each registered service is a C function that returns
 * fast; long-running work happens in a pthread the service owns.
 */

#include "supervisor.h"
#include "../common/log.h"

#include <stdlib.h>
#include <string.h>

#define MAX_SVC 32
static svc_t *g_svc[MAX_SVC];
static int    g_svc_n = 0;

int supervisor_init(void) {
    g_svc_n = 0;
    return 0;
}

int supervisor_register(svc_t *s) {
    if (g_svc_n >= MAX_SVC) return -1;
    g_svc[g_svc_n++] = s;
    s->running = 0;
    return 0;
}

static int should_run(const svc_t *s) {
    if (!s->needs) return 1;
    return (s->gate && *s->gate);
}

int supervisor_start_all(void) {
    int ok = 0;
    for (int i = 0; i < g_svc_n; i++) {
        svc_t *s = g_svc[i];
        if (!should_run(s)) continue;
        LOGI("svc", "start %s", s->name);
        int r = s->start ? s->start() : 0;
        if (r == 0) { s->running = 1; ok++; }
        else        LOGE("svc", "%s start failed: %d", s->name, r);
    }
    LOGI("svc", "%d of %d services up", ok, g_svc_n);
    return ok;
}

int supervisor_stop_all(void) {
    for (int i = g_svc_n - 1; i >= 0; i--) {
        svc_t *s = g_svc[i];
        if (!s->running) continue;
        if (s->stop) s->stop();
        s->running = 0;
    }
    return 0;
}

int supervisor_reconcile(void) {
    int changes = 0;
    for (int i = 0; i < g_svc_n; i++) {
        svc_t *s = g_svc[i];
        int want = should_run(s);
        if (want && !s->running) {
            LOGI("svc", "reconcile: starting %s", s->name);
            if (s->start && s->start() == 0) { s->running = 1; changes++; }
        } else if (!want && s->running) {
            LOGI("svc", "reconcile: stopping %s", s->name);
            if (s->stop) s->stop();
            s->running = 0;
            changes++;
        }
    }
    return changes;
}

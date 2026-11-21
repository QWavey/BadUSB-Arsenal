/*
 * Service supervisor: starts registered services in order, stops them in
 * reverse, and reconciles them when config changes. Each service is a C
 * function that returns quickly; long-running work runs in a pthread the
 * service owns. Some services still call system() (bridge.c, main.c), so this
 * file does not by itself avoid the shell.
 */

#include "supervisor.h"
#include "../common/log.h"

#include <stdlib.h>
#include <string.h>

#define MAX_SVC 32
static svc_t *g_svc[MAX_SVC];
static int    g_svc_n = 0;

int supervisor_stop_all(void);

int supervisor_init(void) {
    supervisor_stop_all();   /* a re-init must not orphan running services */
    g_svc_n = 0;
    return 0;
}

int supervisor_register(svc_t *s) {
    if (g_svc_n >= MAX_SVC) return -1;
    for (int i = 0; i < g_svc_n; i++)
        if (g_svc[i] == s) return -1;   /* already registered: a second entry would be counted twice by start_all */
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
        if (s->running) { ok++; continue; }   /* already up: a second start_all must not start it twice */
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
    /* stop first, in reverse registration order as stop_all does; then start, in order */
    for (int i = g_svc_n - 1; i >= 0; i--) {
        svc_t *s = g_svc[i];
        if (s->running && !should_run(s)) {
            LOGI("svc", "reconcile: stopping %s", s->name);
            if (s->stop) s->stop();
            s->running = 0;
            changes++;
        }
    }
    for (int i = 0; i < g_svc_n; i++) {
        svc_t *s = g_svc[i];
        if (!s->running && should_run(s)) {
            LOGI("svc", "reconcile: starting %s", s->name);
            int r = s->start ? s->start() : 0;
            if (r == 0) { s->running = 1; changes++; }
            else        LOGE("svc", "%s start failed: %d", s->name, r);
        }
    }
    return changes;
}

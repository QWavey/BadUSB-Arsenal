#include "log.h"
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include <sys/time.h>
#include <pthread.h>

static log_level_t g_lvl = LL_INFO;
static FILE *g_file = NULL;
static int g_serial = 1;
static pthread_mutex_t g_mu = PTHREAD_MUTEX_INITIALIZER;

static const char *lvl_tag[] = { "DBG", "INF", "WRN", "ERR" };

void log_init(log_level_t lvl, const char *file_path, int to_serial) {
    g_lvl = lvl;
    g_serial = to_serial;
    if (file_path && file_path[0]) {
        g_file = fopen(file_path, "a");
        if (g_file) setvbuf(g_file, NULL, _IOLBF, 0);
    }
}

void log_set_level(log_level_t lvl) { g_lvl = lvl; }

void log_write(log_level_t lvl, const char *tag, const char *fmt, ...) {
    if (lvl < g_lvl) return;

    struct timeval tv;
    gettimeofday(&tv, NULL);
    struct tm tm;
    localtime_r(&tv.tv_sec, &tm);

    char head[64];
    snprintf(head, sizeof(head), "%02d:%02d:%02d.%03ld %s [%s] ",
             tm.tm_hour, tm.tm_min, tm.tm_sec, tv.tv_usec / 1000,
             lvl_tag[lvl], tag ? tag : "-");

    char body[1024];
    va_list ap;
    va_start(ap, fmt);
    vsnprintf(body, sizeof(body), fmt, ap);
    va_end(ap);

    pthread_mutex_lock(&g_mu);
    if (g_serial) {
        fputs(head, stderr);
        fputs(body, stderr);
        fputc('\n', stderr);
    }
    if (g_file) {
        fputs(head, g_file);
        fputs(body, g_file);
        fputc('\n', g_file);
    }
    pthread_mutex_unlock(&g_mu);
}

void log_close(void) {
    pthread_mutex_lock(&g_mu);
    if (g_file) { fclose(g_file); g_file = NULL; }
    pthread_mutex_unlock(&g_mu);
}

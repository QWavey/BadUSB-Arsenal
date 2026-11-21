#include "log.h"
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include <sys/time.h>
#include <errno.h>
#include <fcntl.h>
#include <unistd.h>
#include <pthread.h>

static log_level_t g_lvl = LL_INFO;
static FILE *g_file = NULL;
static int g_file_err = 0;   /* the first failed file write has been reported */
static int g_serial = 1;
static pthread_mutex_t g_mu = PTHREAD_MUTEX_INITIALIZER;

static const char *lvl_tag[] = { "DBG", "INF", "WRN", "ERR" };

void log_init(log_level_t lvl, const char *file_path, int to_serial) {
    pthread_mutex_lock(&g_mu);
    g_lvl = lvl;
    g_serial = to_serial;
    if (g_file) { fclose(g_file); g_file = NULL; }   /* a second init must not leak the first handle */
    if (file_path && file_path[0]) {
        /* O_CLOEXEC: payloads started with system() must not inherit it */
        int fd = open(file_path, O_WRONLY | O_CREAT | O_APPEND | O_CLOEXEC, 0666);
        if (fd >= 0) {
            g_file = fdopen(fd, "a");
            if (g_file) {
                setvbuf(g_file, NULL, _IOLBF, 0);
                g_file_err = 0;
            } else {
                close(fd);   /* fdopen failed: nothing else owns the descriptor, so close it here */
            }
        } else {
            /* log_init returns nothing, so the failure goes to the serial console here */
            fprintf(stderr, "log: cannot open %s: %s (file log off)\n", file_path, strerror(errno));
        }
    }
    pthread_mutex_unlock(&g_mu);
}

void log_set_level(log_level_t lvl) {
    pthread_mutex_lock(&g_mu);
    g_lvl = lvl;
    pthread_mutex_unlock(&g_mu);
}

void log_write(log_level_t lvl, const char *tag, const char *fmt, ...) {
    pthread_mutex_lock(&g_mu);
    int drop = (lvl < g_lvl);
    pthread_mutex_unlock(&g_mu);
    if (drop) return;
    if ((unsigned)lvl > (unsigned)LL_ERROR) return;   /* not a level that has a tag */

    struct timeval tv;
    gettimeofday(&tv, NULL);
    struct tm tm;
    localtime_r(&tv.tv_sec, &tm);

    char head[64];
    snprintf(head, sizeof(head), "%02d:%02d:%02d.%03ld %s [%.32s] ",
             tm.tm_hour, tm.tm_min, tm.tm_sec, tv.tv_usec / 1000,
             lvl_tag[lvl], tag ? tag : "-");

    char body[1024];
    va_list ap;
    va_start(ap, fmt);
    int need = vsnprintf(body, sizeof(body), fmt, ap);
    va_end(ap);
    if (need < 0) body[0] = '\0';
    if (need >= (int)sizeof(body)) {   /* too long: mark the cut so it is visible */
        static const char cut[] = " ...[cut]";
        memcpy(body + sizeof(body) - sizeof(cut), cut, sizeof(cut));
    }
    /* one record per line: a CR or LF in the text would start a fake record */
    for (char *p = body; *p; p++)
        if (*p == '\n' || *p == '\r') *p = ' ';

    pthread_mutex_lock(&g_mu);
    if (g_serial) {
        fputs(head, stderr);
        fputs(body, stderr);
        fputc('\n', stderr);
    }
    if (g_file) {
        /* no short-circuit: all three writes are tried, so the newline is always attempted */
        int failed = 0;
        failed |= (fputs(head, g_file) == EOF);
        failed |= (fputs(body, g_file) == EOF);
        failed |= (fputc('\n', g_file) == EOF);
        if (failed) {
            if (!g_file_err) {   /* report once: a full card would otherwise drop lines without a word */
                g_file_err = 1;
                fprintf(stderr, "log: write to the log file failed: %s\n", strerror(errno));
            }
        }
    }
    pthread_mutex_unlock(&g_mu);
}

void log_close(void) {
    pthread_mutex_lock(&g_mu);
    if (g_file) { fclose(g_file); g_file = NULL; }
    pthread_mutex_unlock(&g_mu);
}

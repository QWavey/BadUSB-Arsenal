#ifndef GHOST_LOG_H
#define GHOST_LOG_H

#include <stdio.h>
#include <time.h>

typedef enum { LL_DEBUG = 0, LL_INFO = 1, LL_WARN = 2, LL_ERROR = 3 } log_level_t;

void log_init(log_level_t lvl, const char *file_path, int to_serial);
void log_set_level(log_level_t lvl);
void log_write(log_level_t lvl, const char *tag, const char *fmt, ...)
    __attribute__((format(printf, 3, 4)));
void log_close(void);

#define LOGD(tag, ...) log_write(LL_DEBUG, (tag), __VA_ARGS__)
#define LOGI(tag, ...) log_write(LL_INFO,  (tag), __VA_ARGS__)
#define LOGW(tag, ...) log_write(LL_WARN,  (tag), __VA_ARGS__)
#define LOGE(tag, ...) log_write(LL_ERROR, (tag), __VA_ARGS__)

#endif

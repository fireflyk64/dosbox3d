#include <stdio.h>
#include <stdlib.h>
#include <stdarg.h>
#include <sys/time.h>
#include "wcnet_log.h"

static int g_level = -1;

int wclog_level() {
    if (g_level < 0) {
        const char *env = getenv("WCNET_LOG");
        g_level = env ? atoi(env) : 1;
    }
    return g_level;
}

void wclog(int level, const char *fmt, ...) {
    if (level > wclog_level()) {
        return;
    }
    // WCNET_LOG_WALL=1: each line with the wall clock (seconds, to the millisecond), to put two machines' logs side by side
    static int wall = -1;
    if (wall < 0) {
        const char *env = getenv("WCNET_LOG_WALL");
        wall = env && env[0] == '1';
    }
    va_list ap;
    va_start(ap, fmt);
    if (wall) {
        struct timeval tv;
        gettimeofday(&tv, NULL);
        fprintf(stderr, "wcnet %ld.%03ld: ", (long)(tv.tv_sec % 100000), (long)(tv.tv_usec / 1000));
    } else {
        fputs("wcnet: ", stderr);
    }
    vfprintf(stderr, fmt, ap);
    fputc('\n', stderr);
    va_end(ap);
}

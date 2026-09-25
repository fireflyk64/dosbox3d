#include <stdio.h>
#include <stdlib.h>
#include <stdarg.h>
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
    va_list ap;
    va_start(ap, fmt);
    fputs("wcnet: ", stderr);
    vfprintf(stderr, fmt, ap);
    fputc('\n', stderr);
    va_end(ap);
}

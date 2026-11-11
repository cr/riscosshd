#include <stdio.h>
#include <stdarg.h>
#include <time.h>
#include "log.h"

int log_verbose = 0;
static void (*log_sink)(const char *line);

void log_set_sink(void (*sink)(const char *line))
{
    log_sink = sink;
}

static void vlog(const char *fmt, va_list ap)
{
    char line[512];
    time_t now = time(NULL);
    size_t n = strftime(line, sizeof line, "%H:%M:%S ", localtime(&now));
    vsnprintf(line + n, sizeof line - n, fmt, ap);
    if (log_sink) {
        log_sink(line);
    } else {
        fprintf(stderr, "sshd: %s\n", line);
        fflush(stderr);
    }
}

void log_msg(const char *fmt, ...)
{
    va_list ap;
    va_start(ap, fmt);
    vlog(fmt, ap);
    va_end(ap);
}

void log_debug(const char *fmt, ...)
{
    va_list ap;
    if (!log_verbose)
        return;
    va_start(ap, fmt);
    vlog(fmt, ap);
    va_end(ap);
}

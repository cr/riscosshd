#include <stdio.h>
#include <stdarg.h>
#include <time.h>
#include "log.h"

int log_verbose = 0;

static void vlog(const char *fmt, va_list ap)
{
    char stamp[32];
    time_t now = time(NULL);
    strftime(stamp, sizeof stamp, "%H:%M:%S", localtime(&now));
    fprintf(stderr, "%s sshd: ", stamp);
    vfprintf(stderr, fmt, ap);
    fputc('\n', stderr);
    fflush(stderr);
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

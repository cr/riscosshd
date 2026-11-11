#include <stdio.h>
#include <stdarg.h>
#include <time.h>
#include <string.h>
#include "log.h"
#include "riscos.h"

int log_verbose = 0;
static void (*log_sink)(const char *line);
static const char *log_file;

void log_set_sink(void (*sink)(const char *line))
{
    log_sink = sink;
}

void log_set_file(const char *path)
{
    log_file = path;
}

void log_sanitise(char *s)
{
    for (; *s; s++)
        if ((unsigned char)*s < 0x20 || *s == 0x7f)
            *s = '?';
}

static void vlog(const char *fmt, va_list ap)
{
    char line[512];
    time_t now = time(NULL);
    size_t n = strftime(line, sizeof line, "%Y-%m-%d %H:%M:%S ", localtime(&now));
    vsnprintf(line + n, sizeof line - n, fmt, ap);
    log_sanitise(line);                 /* user names, commands, comments... */
    if (log_file) {
        size_t l = strlen(line);
        line[l] = '\n';
        ro_append_file(log_file, line, l + 1);
        line[l] = '\0';
    }
    if (log_sink) {
        log_sink(line + 11);            /* the window shows the time only */
    } else {
        fprintf(stderr, "sshd: %s\n", line + 11);
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

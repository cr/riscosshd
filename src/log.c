#include <stdio.h>
#include <stdarg.h>
#include <time.h>
#include <string.h>
#include "log.h"
#include "riscos.h"

int log_verbose = 0;
static void (*log_sink)(const char *line);
static const char *log_file;
static int log_stderr;

/* Lines logged before a sink exists (startup: configuration, listening
 * port) are kept and replayed into the sink, so the log window is not
 * empty when it first opens. */
#define EARLY_LINES 32
static char early[EARLY_LINES][160];
static int early_count;

void log_set_sink(void (*sink)(const char *line))
{
    int i;
    log_sink = sink;
    if (sink)
        for (i = 0; i < early_count; i++)
            sink(early[i]);
    early_count = 0;
}

void log_set_stderr(int on)
{
    log_stderr = on;
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
        if (early_count < EARLY_LINES)
            snprintf(early[early_count++], sizeof early[0], "%s", line + 11);
        if (log_stderr) {               /* text mode: this is the TaskWindow */
            fprintf(stderr, "sshd: %s\n", line + 11);
            fflush(stderr);
        }
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

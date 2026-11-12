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
            sink(early[i] + 11);        /* the window shows the time only */
    early_count = 0;
}

void log_set_stderr(int on)
{
    log_stderr = on;
}

void log_set_file(const char *path)
{
    int i;
    log_file = path;
    /* the start-up lines logged before the file was known */
    for (i = 0; path && i < early_count; i++) {
        char line[200];
        int n = snprintf(line, sizeof line, "%s\n", early[i]);
        if (n > 0)
            ro_append_file(path, line, (size_t)n);
    }
}

void log_sanitise(char *s)
{
    for (; *s; s++) {
        unsigned char ch = (unsigned char)*s;
        /* C0, DEL and C1 (0x80-0x9f: CSI and friends in a Unix terminal) */
        if (ch < 0x20 || (ch >= 0x7f && ch < 0xa0))
            *s = '?';
    }
}

static void vlog(const char *fmt, va_list ap)
{
    char line[512];
    time_t now = time(NULL);
    struct tm *tm = localtime(&now);
    size_t n = tm ? strftime(line, sizeof line, "%Y-%m-%d %H:%M:%S ", tm) : 0;
    if (n != 20)                        /* no clock: keep the layout the window relies on */
        n = (size_t)snprintf(line, sizeof line, "0000-00-00 00:00:00 ");
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
            snprintf(early[early_count++], sizeof early[0], "%s", line);
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

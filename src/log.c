#include <stdio.h>
#include <stdarg.h>
#include <time.h>
#include <string.h>
#include "log.h"
#include "riscos.h"

int log_verbose = 0;
static void (*notify)(void);
static int to_stderr;
static const char *log_file;
static size_t log_file_limit;

/* The last LOG_LINES lines, oldest at ring[first]. */
static char ring[LOG_LINES][LOG_COLS];
static int first, count;

int log_count(void)
{
    return count;
}

const char *log_line(int i)
{
    return ring[(first + i) % LOG_LINES];
}

void log_clear(void)
{
    first = count = 0;
}

const char *log_file_name(void)
{
    return log_file;
}

void log_set_notify(void (*fn)(void))
{
    notify = fn;
}

void log_set_stderr(int on)
{
    to_stderr = on;
}

static int append_line(const char *line)
{
    char buf[LOG_COLS + 1];
    int n = snprintf(buf, sizeof buf, "%s\n", line);
    return ro_append_file(log_file, buf, (size_t)n, log_file_limit);
}

int log_set_file(const char *path, size_t limit, const char **why)
{
    int i;
    log_file = path;
    log_file_limit = limit;
    for (i = 0; i < count; i++) {          /* what was logged before the file was known */
        if (append_line(log_line(i)) < 0) {
            *why = ro_append_error;
            log_file = NULL;
            return -1;
        }
    }
    if (count == 0 && ro_append_file(path, "", 0, limit) < 0) {
        *why = ro_append_error;
        log_file = NULL;
        return -1;
    }
    return 0;
}

/* Length of a valid UTF-8 sequence starting at s, or 0. */
static int utf8_len(const unsigned char *s)
{
    int n = s[0] >= 0xc2 && s[0] <= 0xdf ? 2 : s[0] >= 0xe0 && s[0] <= 0xef ? 3 :
            s[0] >= 0xf0 && s[0] <= 0xf4 ? 4 : 0;
    for (int i = 1; i < n; i++)
        if (s[i] < 0x80 || s[i] > 0xbf)
            return 0;
    return n;
}

void log_sanitise(char *s)
{
    while (*s) {
        unsigned char ch = (unsigned char)*s;
        int n = ch >= 0x80 ? utf8_len((unsigned char *)s) : 0;
        if (n) {                           /* UTF-8 text (a key comment, say) stays */
            s += n;
            continue;
        }
        /* C0, DEL and stray C1 bytes (0x80-0x9f: CSI and friends in a
         * Unix terminal); other top-bit bytes are Latin-1 text */
        if (ch < 0x20 || (ch >= 0x7f && ch < 0xa0))
            *s = '?';
        s++;
    }
}

static void vlog(const char *fmt, va_list ap)
{
    char *line;
    time_t now = time(NULL);
    struct tm *tm = localtime(&now);
    size_t n;
    if (count < LOG_LINES)
        line = ring[(first + count++) % LOG_LINES];
    else {
        line = ring[first];                /* overwrite the oldest */
        first = (first + 1) % LOG_LINES;
    }
    n = tm ? strftime(line, LOG_COLS, "%Y-%m-%d %H:%M:%S ", tm) : 0;
    if (n != LOG_TIME_AT + 9)              /* no clock: keep the layout */
        n = (size_t)snprintf(line, LOG_COLS, "0000-00-00 00:00:00 ");
    vsnprintf(line + n, LOG_COLS - n, fmt, ap);
    log_sanitise(line);                    /* user names, commands, comments... */
    if (log_file)
        append_line(line);
    if (to_stderr) {
        fprintf(stderr, "sshd: %s\n", line + LOG_TIME_AT);
        fflush(stderr);
    }
    if (notify)
        notify();
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

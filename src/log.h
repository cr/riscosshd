#ifndef LOG_H
#define LOG_H
/* Log lines are "YYYY-MM-DD HH:MM:SS text".  The last LOG_LINES are kept
 * in memory for the log window (which shows them from the time on) and
 * are written to the log file once one is set; in text mode they are also
 * printed to stderr.  A desktop task never prints, as its stderr is the
 * screen. */
#define LOG_LINES 400
#define LOG_COLS 256              /* longest line kept, date included */
#define LOG_TIME_AT 11            /* offset of the time: "YYYY-MM-DD " */

extern int log_verbose;
void log_msg(const char *fmt, ...);
void log_debug(const char *fmt, ...);     /* only when log_verbose */

/* Lines held (up to LOG_LINES) and line i of them, 0 the oldest. */
int log_count(void);
const char *log_line(int i);
/* Called after each new line, e.g. to update the log window. */
void log_set_notify(void (*fn)(void));
/* Print lines to stderr as well (text mode). */
void log_set_stderr(int on);
/* Append every line to this RISC OS file from now on, starting with the
 * lines already held; the file starts afresh once it grows past limit
 * bytes.  Returns 0, or -1 with the reason in *why if it cannot be
 * written. */
int log_set_file(const char *path, size_t limit, const char **why);
/* Replace control characters in s with '?' (user-supplied text). */
void log_sanitise(char *s);
#endif

#ifndef LOG_H
#define LOG_H
extern int log_verbose;
/* Where finished log lines go.  Default: stderr.  The Wimp front end
 * installs a sink that appends to the log window instead, because a Wimp
 * task writing to stderr draws over the desktop. */
void log_set_sink(void (*sink)(const char *line));
void log_msg(const char *fmt, ...);
void log_debug(const char *fmt, ...);
#endif

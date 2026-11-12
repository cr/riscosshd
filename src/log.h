#ifndef LOG_H
#define LOG_H
extern int log_verbose;
/* Where finished log lines go.  Default: stderr.  The Wimp front end
 * installs a sink that appends to the log window instead, because a Wimp
 * task writing to stderr draws over the desktop. */
void log_set_sink(void (*sink)(const char *line));
/* Print lines to stderr while no sink is set (text mode); off by default,
 * since a desktop task's stderr is the VDU. */
void log_set_stderr(int on);
/* Also append every line to this RISC OS file (NULL: none).  The file is
 * truncated at startup if it has grown beyond a few hundred KB. */
void log_set_file(const char *path);
/* Replace control characters in s with '?' (user-supplied text). */
void log_sanitise(char *s);
void log_msg(const char *fmt, ...);
void log_debug(const char *fmt, ...);
#endif

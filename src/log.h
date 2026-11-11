#ifndef LOG_H
#define LOG_H
extern int log_verbose;
void log_msg(const char *fmt, ...);
void log_debug(const char *fmt, ...);
#endif

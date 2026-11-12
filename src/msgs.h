/* Desktop texts from <SSHd$Dir>.Messages through MessageTrans, so that
 * the menu, windows and interactive help can be translated without a
 * rebuild.  Log lines stay in English, as they are what gets reported. */
#ifndef SSHD_MSGS_H
#define SSHD_MSGS_H
#include <stddef.h>
/* Open the Messages file; texts fall back to their token if it is
 * missing. */
void msgs_open(void);
void msgs_close(void);
/* The text for token into buf, with %0 replaced by arg (may be NULL). */
void msgs_get(const char *token, char *buf, size_t len, const char *arg);
#endif

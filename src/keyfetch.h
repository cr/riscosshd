/* Fetch public keys from another machine over SSH (reverse ssh-copy-id):
 * connect as a client, confirm the remote host key fingerprint, log in
 * with a password, print the id_*.pub files in ~/.ssh there and import every line. */
#ifndef KEYFETCH_H
#define KEYFETCH_H
#include <stddef.h>
/* confirm() is shown the fingerprint and returns 1 to continue.  Returns
 * the number of keys added, or -1 with a message in msg (which also gets
 * a summary on success). */
int keyfetch_run(const char *userhost, const char *password,
                 int (*confirm)(const char *fingerprint), char *msg, size_t msglen);
#endif

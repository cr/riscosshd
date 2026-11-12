/* Fetch public keys from another machine over SSH (reverse ssh-copy-id):
 * connect as a client, confirm the remote host key fingerprint, log in
 * with a password, print ~/.ssh/id_ed25519.pub, id_ecdsa.pub and
 * id_rsa.pub there and import the keys they contain. */
#ifndef SSHD_KEYFETCH_H
#define SSHD_KEYFETCH_H
#include <stddef.h>
/* userhost is "user@host" or "user@host:port".  confirm() is shown the
 * fingerprint, host and port and returns 1 to continue.  Returns the
 * number of keys added, or -1; msg always gets a summary for the user. */
int keyfetch_run(const char *userhost, const char *password,
                 int (*confirm)(const char *fingerprint, const char *host, unsigned port),
                 char *msg, size_t msglen);
#endif

#ifndef AUTH_H
#define AUTH_H
#include <libssh/libssh.h>
/* Load authorized keys (OpenSSH authorized_keys format) from a RISC OS path.
 * Returns the number of keys loaded, or -1 if the file is missing. */
int auth_load_keys(const char *path);
/* Returns 1 if pubkey matches one of the loaded keys. */
int auth_key_allowed(ssh_key pubkey);
void auth_free(void);
#endif

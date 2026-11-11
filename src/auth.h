#ifndef AUTH_H
#define AUTH_H
#include <stddef.h>
#include <libssh/libssh.h>
/* Load authorized keys (OpenSSH authorized_keys format) from a RISC OS path.
 * Returns the number of keys loaded, or -1 if the file is missing. */
int auth_load_keys(const char *path);
/* Returns 1 if pubkey matches one of the loaded keys. */
int auth_key_allowed(ssh_key pubkey);
/* Parse one "type base64 [comment]" line, add it and append it to the
 * authorized keys file.  Returns 0, or -1 with a message in err. */
int auth_add_key_line(const char *line, char *err, size_t errlen);
int auth_count(void);
/* Human-readable list of the loaded keys ("ssh-ed25519 laptop, ...") */
void auth_describe(char *buf, size_t len);
void auth_free(void);
#endif

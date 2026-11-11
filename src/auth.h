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
 * authorized keys file.  A key that is already authorized is refused.
 * Returns 0, or -1 with a message in err. */
int auth_add_key_line(const char *line, char *err, size_t errlen);
int auth_count(void);
/* One-line description of key i: "type comment ...last chars of key" */
void auth_describe_one(int i, char *buf, size_t len);
/* Remove key i and rewrite the authorized keys file (comment lines in the
 * file are not preserved).  Returns 0, or -1 with a message in err. */
int auth_remove_key(int i, char *err, size_t errlen);
void auth_free(void);
#endif

#ifndef SSHD_HOSTKEY_H
#define SSHD_HOSTKEY_H
#include <libssh/libssh.h>
/* Load the host key from path (any type libssh reads), or generate an
 * ed25519 key and save it there if the file does not exist.  Returns NULL
 * on failure. */
ssh_key hostkey_load_or_create(const char *path);
/* "ED25519 SHA256:..." of the loaded host key, as clients show it; "" if
 * none is loaded. */
const char *hostkey_fingerprint(void);
#endif

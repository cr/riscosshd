#ifndef HOSTKEY_H
#define HOSTKEY_H
#include <libssh/libssh.h>
/* Load the host key from path (any type libssh reads), or generate an
 * ed25519 key and save it there if the file does not exist.  Returns NULL
 * on failure. */
ssh_key hostkey_load_or_create(const char *path);
/* "ssh-ed25519 AAAA..." of the loaded host key, for display. */
const char *hostkey_public_line(void);
#endif

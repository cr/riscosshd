/* SPDX-License-Identifier: MIT */
#ifndef SSHD_AUTH_H
#define SSHD_AUTH_H
#include <stddef.h>
#include <libssh/libssh.h>
/* Authorised keys: OpenSSH authorized_keys lines without options, i.e.
 * "type base64 [comment]", one per line. */

/* Load the keys from a RISC OS path.  Returns the number loaded, or -1 if
 * the file is missing. */
int auth_load_keys(const char *path);
/* Returns 1 if pubkey matches one of the loaded keys. */
int auth_key_allowed(ssh_key pubkey);
/* Add every key in text, which is split into lines in place (blank lines
 * and # comments are skipped), and append the new ones to the file in
 * one write.  Counts go to *added, *duplicate and *invalid; when nothing
 * was added, err says why the last refused line was refused.  Returns 0,
 * or -1 with a message in err if the file could not be written (the keys
 * still apply until the next start). */
int auth_add_keys(char *text, int *added, int *duplicate, int *invalid,
                  char *err, size_t errlen);
int auth_count(void);
/* One-line description of key i: "ED25519 SHA256:... comment", the
 * fingerprint as ssh-keygen -l shows it. */
void auth_describe_one(int i, char *buf, size_t len);
/* Remove key i and rewrite the authorised keys file (comment lines in the
 * file are not preserved).  Returns 0, or -1 with a message in err. */
int auth_remove_key(int i, char *err, size_t errlen);
void auth_free(void);
#endif

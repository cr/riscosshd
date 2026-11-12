/* A public key as people compare it: "ED25519 SHA256:..." in the form
 * OpenSSH prints on a first connection and ssh-keygen -l shows. */
#ifndef SSHD_KEYFP_H
#define SSHD_KEYFP_H
#include <stdio.h>
#include <libssh/libssh.h>

static inline const char *key_kind(ssh_key k)
{
    switch (ssh_key_type(k)) {
    case SSH_KEYTYPE_ED25519:    return "ED25519";
    case SSH_KEYTYPE_ECDSA_P256:
    case SSH_KEYTYPE_ECDSA_P384:
    case SSH_KEYTYPE_ECDSA_P521: return "ECDSA";
    case SSH_KEYTYPE_RSA:        return "RSA";
    default:                     return ssh_key_type_to_char(ssh_key_type(k));
    }
}

/* "ED25519 SHA256:abc..." into buf; "(unknown)" if it cannot be computed. */
static inline void key_fingerprint(ssh_key k, char *buf, size_t len)
{
    unsigned char *hash = NULL;
    size_t hlen = 0;
    char *fp = NULL;
    if (k && ssh_get_publickey_hash(k, SSH_PUBLICKEY_HASH_SHA256, &hash, &hlen) == 0)
        fp = ssh_get_fingerprint_hash(SSH_PUBLICKEY_HASH_SHA256, hash, hlen);
    snprintf(buf, len, "%s %s", k ? key_kind(k) : "", fp ? fp : "(unknown)");
    ssh_string_free_char(fp);
    ssh_clean_pubkey_hash(&hash);
}
#endif

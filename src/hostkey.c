#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <libssh/libssh.h>

#include "hostkey.h"
#include "keyfp.h"
#include "riscos.h"
#include "log.h"

static char fingerprint[128] = "";

const char *hostkey_fingerprint(void)
{
    return fingerprint;
}

static void remember_public(ssh_key key)
{
    key_fingerprint(key, fingerprint, sizeof fingerprint);
    log_msg("host key %s: clients show this on their first connection", fingerprint);
}

ssh_key hostkey_load_or_create(const char *path)
{
    ssh_key key = NULL;
    char *pem = ro_load_file(path, NULL);

    if (pem) {
        int rc = ssh_pki_import_privkey_base64(pem, NULL, NULL, NULL, &key);
        free(pem);
        if (rc != SSH_OK) {
            log_msg("host key %s is unreadable; delete it to generate a new one", path);
            return NULL;
        }
        remember_public(key);
        return key;
    }

    log_msg("no host key at %s, generating an ed25519 key", path);
    if (ssh_pki_generate(SSH_KEYTYPE_ED25519, 0, &key) != SSH_OK) {
        log_msg("host key generation failed");
        return NULL;
    }
    if (ssh_pki_export_privkey_base64(key, NULL, NULL, NULL, &pem) != SSH_OK) {
        log_msg("cannot export generated host key");
        ssh_key_free(key);
        return NULL;
    }
    {
        const char *err = ro_save_file(path, FILETYPE_TEXT, pem, strlen(pem));
        ssh_string_free_char(pem);
        if (err) {
            log_msg("cannot save host key to %s: %s", path, err);
            ssh_key_free(key);
            return NULL;
        }
    }
    log_msg("new host key generated");
    remember_public(key);
    return key;
}

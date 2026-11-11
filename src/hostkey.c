#include <stdlib.h>
#include <string.h>
#include <libssh/libssh.h>

#include "hostkey.h"
#include "riscos.h"
#include "log.h"

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
        return key;
    }

    log_msg("no host key at %s, generating ed25519 key (needs CryptRandom)", path);
    if (ssh_pki_generate(SSH_KEYTYPE_ED25519, 0, &key) != SSH_OK) {
        log_msg("key generation failed (is the CryptRandom module loaded?)");
        return NULL;
    }
    if (ssh_pki_export_privkey_base64(key, NULL, NULL, NULL, &pem) != SSH_OK) {
        log_msg("cannot export generated host key");
        ssh_key_free(key);
        return NULL;
    }
    {
        const char *err = ro_save_file(path, 0xfff, pem, strlen(pem));
        ssh_string_free_char(pem);
        if (err) {
            log_msg("cannot save host key to %s: %s", path, err);
            ssh_key_free(key);
            return NULL;
        }
    }
    {
        char *pub = NULL;
        if (ssh_pki_export_pubkey_base64(key, &pub) == SSH_OK) {
            log_msg("host key fingerprint source: ssh-ed25519 %s", pub);
            ssh_string_free_char(pub);
        }
    }
    return key;
}

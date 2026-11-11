#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <ctype.h>
#include <libssh/libssh.h>

#include "auth.h"
#include "riscos.h"
#include "log.h"

static ssh_key *keys;
static int nkeys;

static void add_key(ssh_key k)
{
    ssh_key *n = realloc(keys, sizeof *keys * (size_t)(nkeys + 1));
    if (!n)
        return;
    keys = n;
    keys[nkeys++] = k;
}

/* Parse one "type base64 [comment]" line (options prefixes are not supported). */
static void parse_line(char *line)
{
    char *type, *b64, *save = NULL;
    enum ssh_keytypes_e kt;
    ssh_key k = NULL;

    while (isspace((unsigned char)*line))
        line++;
    if (*line == '\0' || *line == '#')
        return;
    type = strtok_r(line, " \t", &save);
    b64 = type ? strtok_r(NULL, " \t\r\n", &save) : NULL;
    if (!type || !b64)
        return;
    kt = ssh_key_type_from_name(type);
    if (kt == SSH_KEYTYPE_UNKNOWN) {
        log_msg("authorized_keys: unknown key type '%s'", type);
        return;
    }
    if (ssh_pki_import_pubkey_base64(b64, kt, &k) != SSH_OK) {
        log_msg("authorized_keys: cannot parse %s key", type);
        return;
    }
    add_key(k);
}

int auth_load_keys(const char *path)
{
    char *data = ro_load_file(path, NULL), *p, *nl;
    if (!data)
        return -1;
    for (p = data; p && *p; p = nl) {
        nl = strchr(p, '\n');
        if (nl)
            *nl++ = '\0';
        parse_line(p);
    }
    free(data);
    return nkeys;
}

int auth_key_allowed(ssh_key pubkey)
{
    for (int i = 0; i < nkeys; i++)
        if (ssh_key_cmp(pubkey, keys[i], SSH_KEY_CMP_PUBLIC) == 0)
            return 1;
    return 0;
}

void auth_free(void)
{
    for (int i = 0; i < nkeys; i++)
        ssh_key_free(keys[i]);
    free(keys);
    keys = NULL;
    nkeys = 0;
}

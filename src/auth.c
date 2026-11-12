#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <ctype.h>
#include <libssh/libssh.h>

#include "auth.h"
#include "riscos.h"
#include "log.h"

struct akey {
    ssh_key key;
    char *type;
    char *b64;
    char *comment;
};

static struct akey *keys;
static int nkeys;
static const char *keys_path;

static int add_key(ssh_key k, const char *type, const char *b64, const char *comment)
{
    struct akey *n = realloc(keys, sizeof *keys * (size_t)(nkeys + 1));
    if (!n)
        return -1;
    keys = n;
    keys[nkeys].key = k;
    keys[nkeys].type = strdup(type);
    keys[nkeys].b64 = strdup(b64);
    keys[nkeys].comment = strdup(comment ? comment : "");
    if (!keys[nkeys].type || !keys[nkeys].b64 || !keys[nkeys].comment) {
        free(keys[nkeys].type);
        free(keys[nkeys].b64);
        free(keys[nkeys].comment);
        return -1;
    }
    log_sanitise(keys[nkeys].comment);   /* shown in icons and dialogue boxes */
    nkeys++;
    return 0;
}

/* Parse one "type base64 [comment]" line (options prefixes are not
 * supported).  Returns 1 if a key was added, 0 for blank/comment lines,
 * -1 on a parse error with a message in err. */
static int parse_line(const char *src, char *err, size_t errlen)
{
    char *line, *type, *b64, *comment, *save = NULL;
    enum ssh_keytypes_e kt;
    ssh_key k = NULL;
    int rc;

    while (isspace((unsigned char)*src))
        src++;
    if (*src == '\0' || *src == '#')
        return 0;
    line = strdup(src);
    if (!line) {
        snprintf(err, errlen, "out of memory");
        return -1;
    }
    type = strtok_r(line, " \t\r\n", &save);
    b64 = type ? strtok_r(NULL, " \t\r\n", &save) : NULL;
    comment = b64 ? strtok_r(NULL, "\r\n", &save) : NULL;
    while (comment && isspace((unsigned char)*comment))
        comment++;
    if (!type || !b64) {
        snprintf(err, errlen, "expected 'type base64 [comment]'");
        free(line);
        return -1;
    }
    kt = ssh_key_type_from_name(type);
    if (kt == SSH_KEYTYPE_UNKNOWN) {
        snprintf(err, errlen, "unknown key type '%s'", type);
        free(line);
        return -1;
    }
    if (ssh_pki_import_pubkey_base64(b64, kt, &k) != SSH_OK) {
        snprintf(err, errlen, "cannot decode %s key", type);
        free(line);
        return -1;
    }
    if (auth_key_allowed(k)) {
        snprintf(err, errlen, "that key is already authorized");
        ssh_key_free(k);
        free(line);
        return -1;
    }
    rc = add_key(k, type, b64, comment);
    free(line);
    if (rc < 0) {
        snprintf(err, errlen, "out of memory");
        ssh_key_free(k);
        return -1;
    }
    return 1;
}

int auth_load_keys(const char *path)
{
    char err[128];
    char *data = ro_load_file(path, NULL), *p, *nl;
    keys_path = path;
    if (!data)
        return -1;
    for (p = data; p && *p; p = nl) {
        nl = strchr(p, '\n');
        if (nl)
            *nl++ = '\0';
        if (parse_line(p, err, sizeof err) < 0)
            log_msg("authkeys: skipped line: %s", err);
    }
    free(data);
    return nkeys;
}

int auth_add_key_line(const char *line, char *err, size_t errlen)
{
    char *data;
    size_t len = 0, add;
    const char *e;
    int rc = parse_line(line, err, errlen);

    if (rc < 0)
        return -1;
    if (rc == 0) {
        snprintf(err, errlen, "no key on that line");
        return -1;
    }
    if (!keys_path) {
        snprintf(err, errlen, "no configuration directory to save it in");
        return -1;
    }
    /* Append to the file: existing content + line + newline. */
    data = ro_load_file(keys_path, &len);
    add = strlen(line) + 2;
    data = realloc(data, len + add + 1);
    if (!data) {
        snprintf(err, errlen, "out of memory");
        return -1;
    }
    if (len > 0 && data[len - 1] != '\n')
        data[len++] = '\n';
    memcpy(data + len, line, strlen(line));
    len += strlen(line);
    data[len++] = '\n';
    e = ro_save_file(keys_path, 0xfff, data, len);
    free(data);
    if (e) {
        snprintf(err, errlen, "key added for this session only; cannot save: %s", e);
        return -1;
    }
    log_msg("added %s key %s", keys[nkeys - 1].type, keys[nkeys - 1].comment);
    return 0;
}

int auth_count(void)
{
    return nkeys;
}

void auth_describe_one(int i, char *buf, size_t len)
{
    size_t bl;
    if (i < 0 || i >= nkeys) {
        buf[0] = '\0';
        return;
    }
    bl = strlen(keys[i].b64);
    snprintf(buf, len, "%s %.100s ...%s", keys[i].type,
             keys[i].comment[0] ? keys[i].comment : "(no comment)",
             keys[i].b64 + (bl > 12 ? bl - 12 : 0));
}

static const char *save_all(void)
{
    size_t len = 0, cap = 256;
    char *data = malloc(cap);
    const char *e;
    if (!data)
        return "out of memory";
    data[0] = '\0';
    for (int i = 0; i < nkeys; i++) {
        size_t need = strlen(keys[i].type) + strlen(keys[i].b64) + strlen(keys[i].comment) + 4;
        if (len + need > cap) {
            char *n;
            while (len + need > cap)
                cap *= 2;
            n = realloc(data, cap);
            if (!n) {
                free(data);
                return "out of memory";
            }
            data = n;
        }
        len += (size_t)snprintf(data + len, cap - len, "%s %s%s%s\n", keys[i].type, keys[i].b64,
                                keys[i].comment[0] ? " " : "", keys[i].comment);
    }
    e = ro_save_file(keys_path, 0xfff, data, len);
    free(data);
    return e;
}

int auth_remove_key(int i, char *err, size_t errlen)
{
    const char *e;
    if (i < 0 || i >= nkeys) {
        snprintf(err, errlen, "no such key");
        return -1;
    }
    log_msg("removed %s key %s", keys[i].type, keys[i].comment);
    ssh_key_free(keys[i].key);
    free(keys[i].type);
    free(keys[i].b64);
    free(keys[i].comment);
    memmove(&keys[i], &keys[i + 1], sizeof *keys * (size_t)(nkeys - i - 1));
    nkeys--;
    e = keys_path ? save_all() : NULL;
    if (e) {
        snprintf(err, errlen, "removed for this session only; cannot save: %s", e);
        return -1;
    }
    return 0;
}

int auth_key_allowed(ssh_key pubkey)
{
    for (int i = 0; i < nkeys; i++)
        if (ssh_key_cmp(pubkey, keys[i].key, SSH_KEY_CMP_PUBLIC) == 0)
            return 1;
    return 0;
}

void auth_free(void)
{
    for (int i = 0; i < nkeys; i++) {
        ssh_key_free(keys[i].key);
        free(keys[i].type);
        free(keys[i].b64);
        free(keys[i].comment);
    }
    free(keys);
    keys = NULL;
    nkeys = 0;
}

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <ctype.h>
#include <libssh/libssh.h>

#include "auth.h"
#include "keyfp.h"
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

static void free_akey(struct akey *k)
{
    ssh_key_free(k->key);
    free(k->type);
    free(k->b64);
    free(k->comment);
}

static int add_key(ssh_key k, const char *type, const char *b64, const char *comment)
{
    struct akey *n = realloc(keys, sizeof *keys * (size_t)(nkeys + 1)), a;
    if (!n)
        return -1;
    keys = n;
    a.key = k;
    a.type = strdup(type);
    a.b64 = strdup(b64);
    a.comment = strdup(comment ? comment : "");
    if (!a.type || !a.b64 || !a.comment) {
        a.key = NULL;                       /* the caller still owns k */
        free_akey(&a);
        return -1;
    }
    log_sanitise(a.comment);                /* shown in icons and dialogue boxes */
    keys[nkeys++] = a;
    return 0;
}

#define LINE_ADDED      1
#define LINE_EMPTY      0                   /* blank or a # comment */
#define LINE_INVALID  (-1)
#define LINE_DUPLICATE (-2)

/* Parse one "type base64 [comment]" line and add the key; err says why
 * when the result is LINE_INVALID or LINE_DUPLICATE. */
static int parse_line(const char *src, char *err, size_t errlen)
{
    char *line, *type, *b64, *comment, *save = NULL;
    enum ssh_keytypes_e kt;
    ssh_key k = NULL;
    int rc = LINE_INVALID;

    while (isspace((unsigned char)*src))
        src++;
    if (*src == '\0' || *src == '#')
        return LINE_EMPTY;
    line = strdup(src);
    if (!line) {
        snprintf(err, errlen, "out of memory");
        return LINE_INVALID;
    }
    type = strtok_r(line, " \t\r\n", &save);
    b64 = type ? strtok_r(NULL, " \t\r\n", &save) : NULL;
    comment = b64 ? strtok_r(NULL, "\r\n", &save) : NULL;
    while (comment && isspace((unsigned char)*comment))
        comment++;
    if (!type || !b64)
        snprintf(err, errlen, "expected 'type base64 [comment]'");
    else if ((kt = ssh_key_type_from_name(type)) == SSH_KEYTYPE_UNKNOWN)
        snprintf(err, errlen, "unknown key type '%s'", type);
    else if (ssh_pki_import_pubkey_base64(b64, kt, &k) != SSH_OK)
        snprintf(err, errlen, "cannot decode %s key", type);
    else if (auth_key_allowed(k)) {
        snprintf(err, errlen, "that key is already authorized");
        rc = LINE_DUPLICATE;
    } else if (add_key(k, type, b64, comment) < 0)
        snprintf(err, errlen, "out of memory");
    else {
        k = NULL;                           /* now owned by keys[] */
        rc = LINE_ADDED;
    }
    ssh_key_free(k);
    free(line);
    return rc;
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

/* Append text (whole lines) to the keys file. */
static int append_to_file(const char *text, size_t n, char *err, size_t errlen)
{
    char *data;
    size_t len = 0;
    const char *e;
    if (!keys_path) {
        snprintf(err, errlen, "added for this session only: no configuration directory");
        return -1;
    }
    data = ro_load_file(keys_path, &len);
    {
        char *grown = realloc(data, len + n + 2);
        if (!grown) {
            free(data);
            snprintf(err, errlen, "added for this session only: out of memory saving it");
            return -1;
        }
        data = grown;
    }
    if (len > 0 && data[len - 1] != '\n')
        data[len++] = '\n';
    memcpy(data + len, text, n);
    e = ro_save_file(keys_path, 0xfff, data, len + n);
    free(data);
    if (e) {
        snprintf(err, errlen, "added for this session only; cannot save: %s", e);
        return -1;
    }
    return 0;
}

int auth_add_key_line(const char *line, char *err, size_t errlen)
{
    char buf[1400];
    int rc = parse_line(line, err, errlen), n;
    if (rc == LINE_EMPTY)
        snprintf(err, errlen, "no key on that line");
    if (rc != LINE_ADDED)
        return -1;
    log_msg("added %s key %s", keys[nkeys - 1].type, keys[nkeys - 1].comment);
    n = snprintf(buf, sizeof buf, "%s\n", line);
    if (n < 0 || (size_t)n >= sizeof buf) {
        snprintf(err, errlen, "added for this session only: line too long to save");
        return -1;
    }
    return append_to_file(buf, (size_t)n, err, errlen);
}

int auth_add_keys(const char *text, int *added, int *duplicate, int *invalid,
                  char *err, size_t errlen)
{
    char *copy = strdup(text), *p, *nl, *out;
    size_t olen = 0;
    int rc = 0;
    *added = *duplicate = *invalid = 0;
    out = malloc(strlen(text) + 2);
    if (!copy || !out) {
        free(copy);
        free(out);
        snprintf(err, errlen, "out of memory");
        return -1;
    }
    for (p = copy; p && *p; p = nl) {
        char why[128];
        size_t l;
        nl = strchr(p, '\n');
        if (nl)
            *nl++ = '\0';
        l = strlen(p);
        if (l && p[l - 1] == '\r')         /* CRLF files */
            p[l - 1] = '\0';
        switch (parse_line(p, why, sizeof why)) {
        case LINE_ADDED:
            log_msg("added %s key %s", keys[nkeys - 1].type, keys[nkeys - 1].comment);
            olen += (size_t)sprintf(out + olen, "%s\n", p);
            (*added)++;
            break;
        case LINE_DUPLICATE: (*duplicate)++; break;
        case LINE_INVALID:   (*invalid)++; break;
        default: break;
        }
    }
    if (olen)
        rc = append_to_file(out, olen, err, errlen);
    free(copy);
    free(out);
    return rc;
}

int auth_count(void)
{
    return nkeys;
}

void auth_describe_one(int i, char *buf, size_t len)
{
    char fp[128];
    if (i < 0 || i >= nkeys) {
        buf[0] = '\0';
        return;
    }
    key_fingerprint(keys[i].key, fp, sizeof fp);
    snprintf(buf, len, "%s %s", fp, keys[i].comment[0] ? keys[i].comment : "(no comment)");
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
    free_akey(&keys[i]);
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
    for (int i = 0; i < nkeys; i++)
        free_akey(&keys[i]);
    free(keys);
    keys = NULL;
    nkeys = 0;
}

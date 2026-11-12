#include <stdbool.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <libssh/libssh.h>

#include "keyfetch.h"
#include "keyfp.h"
#include "auth.h"
#include "config.h"
#include "log.h"

#define REMOTE_CMD "cat ~/.ssh/id_ed25519.pub ~/.ssh/id_ecdsa.pub ~/.ssh/id_rsa.pub 2>/dev/null"
#define MAX_OUTPUT 16384

/* "user@host", "user@host:port" or "user@[v6addr]:port"; the user name
 * ends at the last '@', as in OpenSSH.  Returns 0, or -1 if malformed or
 * too long. */
static int parse_target(const char *in, char *user, size_t ul, char *host, size_t hl,
                        unsigned *port)
{
    const char *at = strrchr(in, '@'), *h, *end;
    size_t n;
    *port = 22;
    if (!at || at == in || (size_t)(at - in) >= ul)
        return -1;
    memcpy(user, in, (size_t)(at - in));
    user[at - in] = '\0';
    h = at + 1;
    if (*h == '[') {                       /* [IPv6 address] */
        end = strchr(h, ']');
        if (!end)
            return -1;
        h++;
        n = (size_t)(end - h);
        end++;
    } else {
        end = strchr(h, ':');
        if (end && strchr(end + 1, ':'))   /* a bare IPv6 address: no port */
            end = NULL;
        n = end ? (size_t)(end - h) : strlen(h);
        if (!end)
            end = h + n;
    }
    if (n == 0 || n >= hl)
        return -1;
    memcpy(host, h, n);
    host[n] = '\0';
    if (*end == ':') {
        char *e;
        unsigned long p = strtoul(end + 1, &e, 10);
        if (!end[1] || *e || p == 0 || p > 65535)
            return -1;
        *port = (unsigned)p;
    } else if (*end) {
        return -1;
    }
    return 0;
}

static int fail(char *msg, size_t len, const char *what, ssh_session s)
{
    snprintf(msg, len, "%s: %s", what, s ? ssh_get_error(s) : "error");
    log_msg("key fetch: %s", msg);
    return -1;
}

/* Keyboard-interactive: answer the first round of prompts with the
 * password.  A second round means the server rejected it and is asking
 * again; give up rather than burn through its retry delays. */
static int auth_kbdint(ssh_session s, const char *password)
{
    int rounds = 0;
    int rc = ssh_userauth_kbdint(s, NULL, NULL);
    while (rc == SSH_AUTH_INFO) {
        int n = ssh_userauth_kbdint_getnprompts(s);
        if (n == 0) {                 /* informational round, no prompts */
            rc = ssh_userauth_kbdint(s, NULL, NULL);
            continue;
        }
        if (rounds++ > 0)
            return SSH_AUTH_DENIED;
        for (int i = 0; i < n; i++)
            if (ssh_userauth_kbdint_setanswer(s, (unsigned)i, password) < 0)
                return SSH_AUTH_ERROR;
        rc = ssh_userauth_kbdint(s, NULL, NULL);
    }
    return rc;
}

int keyfetch_run(const char *userhost, const char *password,
                 int (*confirm)(const char *fingerprint, const char *host, unsigned port),
                 char *msg, size_t msglen)
{
    char user[64] = "", host[256] = "", err[160];
    unsigned port = 22;
    long timeout = 15;
    bool no = false;
    ssh_session s;
    ssh_channel ch = NULL;
    ssh_key key = NULL;
    unsigned char *hash = NULL;
    size_t hlen = 0, olen = 0;
    char *out = NULL;
    int rc, ok, added = 0, dup = 0, bad = 0, result = -1;

    if (!crypto_ready) {
        /* after a failed start libssh may hold a freed, unseeded random
         * generator: a key exchange on it would be predictable */
        snprintf(msg, msglen, "Import is unavailable: the server did not start (see the log)");
        return -1;
    }
    if (parse_target(userhost, user, sizeof user, host, sizeof host, &port) < 0) {
        snprintf(msg, msglen, "Enter the account as user@host or user@host:port");
        return -1;
    }

    s = ssh_new();
    if (!s) {
        snprintf(msg, msglen, "Out of memory");
        return -1;
    }
    ssh_options_set(s, SSH_OPTIONS_HOST, host);
    ssh_options_set(s, SSH_OPTIONS_USER, user);
    ssh_options_set(s, SSH_OPTIONS_PORT, &port);
    ssh_options_set(s, SSH_OPTIONS_TIMEOUT, &timeout);
    /* No ssh_config: libssh would read one from the disc, which any SFTP
     * client could have written to redirect this connection. */
    ssh_options_set(s, SSH_OPTIONS_PROCESS_CONFIG, &no);
    log_msg("key fetch: connecting to %s@%s:%u", user, host, port);
    if (ssh_connect(s) != SSH_OK) {
        fail(msg, msglen, "Cannot connect", s);
        goto out;
    }

    /* Trust-on-first-use with an explicit prompt: the keys we are about to
     * import grant login here, so the user must vouch for the peer. */
    if (ssh_get_server_publickey(s, &key) != SSH_OK ||
        ssh_get_publickey_hash(key, SSH_PUBLICKEY_HASH_SHA256, &hash, &hlen) != 0) {
        fail(msg, msglen, "Cannot read the remote host key", s);
        goto out;
    }
    {
        char fpr[128];                     /* "ED25519 SHA256:...", as ssh-keygen -l shows */
        key_fingerprint(key, fpr, sizeof fpr);
        ok = confirm(fpr, host, port);
    }
    if (!ok) {
        snprintf(msg, msglen, "Cancelled: host key not accepted");
        goto out;
    }

    /* Ask which methods the server offers and use exactly one, so a wrong
     * password costs one server-side delay rather than several. */
    ssh_userauth_none(s, NULL);
    {
        int methods = ssh_userauth_list(s, NULL);
        if (methods & SSH_AUTH_METHOD_PASSWORD)
            rc = ssh_userauth_password(s, NULL, password);
        else if (methods & SSH_AUTH_METHOD_INTERACTIVE)
            rc = auth_kbdint(s, password);
        else
            rc = SSH_AUTH_DENIED;
    }
    if (rc != SSH_AUTH_SUCCESS) {
        snprintf(msg, msglen, "Login as %s@%s failed: wrong password, or the machine does not allow password logins", user, host);
        log_msg("key fetch: %s", msg);
        goto out;
    }

    ch = ssh_channel_new(s);
    if (!ch || ssh_channel_open_session(ch) != SSH_OK ||
        ssh_channel_request_exec(ch, REMOTE_CMD) != SSH_OK) {
        fail(msg, msglen, "Cannot run the remote command", s);
        goto out;
    }
    out = malloc(MAX_OUTPUT + 1);
    if (!out) {
        snprintf(msg, msglen, "Out of memory");
        goto out;
    }
    for (;;) {
        int n = ssh_channel_read(ch, out + olen, (uint32_t)(MAX_OUTPUT - olen), 0);
        if (n <= 0 || olen >= MAX_OUTPUT)
            break;
        olen += (size_t)n;
    }
    out[olen] = '\0';
    ssh_channel_send_eof(ch);
    ssh_channel_close(ch);

    if (auth_add_keys(out, &added, &dup, &bad, err, sizeof err) < 0)
        snprintf(msg, msglen, "%d key%s imported, but %s", added, added == 1 ? "" : "s", err);
    else if (added == 0 && dup > 0)
        snprintf(msg, msglen, "The keys of %s@%s are already authorized", user, host);
    else if (added == 0)
        snprintf(msg, msglen, "No keys found on %s (looked for ~/.ssh/id_ed25519.pub, id_ecdsa.pub, id_rsa.pub)", host);
    else
        snprintf(msg, msglen, "%d key%s imported from %s@%s%s%s", added, added == 1 ? "" : "s", user, host,
                 dup ? ", others already authorized" : "", bad ? " (some lines were not keys)" : "");
    log_msg("key fetch: %s", msg);
    result = added;
out:
    free(out);
    ssh_clean_pubkey_hash(&hash);
    ssh_key_free(key);
    if (ch)
        ssh_channel_free(ch);
    ssh_disconnect(s);
    ssh_free(s);
    return result;
}

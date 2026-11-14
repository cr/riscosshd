/* SPDX-License-Identifier: MIT */
/* ssh-copy-id backend.
 *
 * ssh-copy-id (OpenSSH) installs keys by running a POSIX shell script on
 * the server, "exec sh -c 'cd; umask 077; ... cat >> .ssh/authorized_keys'",
 * with the public keys on its standard input.  RISC OS has no sh, so the
 * server recognises that command instead of running it: the input is read
 * to EOF and added to the authorised keys like a key pasted into the SSH
 * Keys window, and the exit status tells ssh-copy-id how it went.
 */
#include <stdlib.h>
#include <string.h>

#include "backend.h"
#include "auth.h"
#include "log.h"

#define COPYID_MAX (64 * 1024)   /* far more than any set of public keys */

struct copyid {
    struct buf in;
    int overflow;
};

int copyid_command(const char *command)
{
    /* The script is built by ssh-copy-id's installkeys_sh(): one line, a
     * "set -x;" first with its -x option.  These three parts are stable
     * across OpenSSH releases; nothing else asks for them together. */
    return strncmp(command, "exec sh -c '", 12) == 0 &&
           strstr(command, "authorized_keys") != NULL &&
           strstr(command, "cat >>") != NULL;
}

static int copyid_start(struct chan_ctx *c, const char *command)
{
    struct copyid *st = calloc(1, sizeof *st);
    (void)command;
    if (!st) {
        chan_refuse(c, "SSHd: out of memory");
        return BACKEND_REFUSED;
    }
    c->backend_data = st;
    log_msg("%s: ssh-copy-id: receiving keys", c->owner_name);
    return 0;
}

static void copyid_data(struct chan_ctx *c, const char *data, size_t len)
{
    struct copyid *st = c->backend_data;
    if (st->overflow || st->in.len + len > COPYID_MAX) {
        st->overflow = 1;
        return;
    }
    if (buf_append(&st->in, data, len) < 0)
        st->overflow = 1;
}

static void copyid_eof(struct chan_ctx *c)
{
    struct copyid *st = c->backend_data;
    char err[160], *text;
    int added = 0, dup = 0, bad = 0, rc;

    if (st->overflow) {
        log_msg("%s: ssh-copy-id: refused, over %d bytes of keys", c->owner_name, COPYID_MAX);
        chan_refuse(c, "SSHd: too much input for a set of public keys");
        return;
    }
    text = malloc(st->in.len + 1);
    if (!text) {
        chan_refuse(c, "SSHd: out of memory");
        return;
    }
    memcpy(text, buf_data(&st->in), st->in.len);
    text[st->in.len] = '\0';
    rc = auth_add_keys(text, &added, &dup, &bad, err, sizeof err);
    free(text);
    log_msg("%s: ssh-copy-id: %d added, %d already there, %d not a key", c->owner_name, added, dup, bad);
    if (rc < 0) {
        /* added, but only until SSHd quits */
        chan_output_err(c, "SSHd: ");
        chan_refuse(c, err);
    } else if (added == 0 && dup == 0) {
        chan_output_err(c, "SSHd: no key added: ");
        chan_refuse(c, err);
    } else {
        chan_finish(c, 0);
    }
}

static void copyid_stop(struct chan_ctx *c)
{
    struct copyid *st = c->backend_data;
    if (st) {
        buf_free(&st->in);
        free(st);
    }
    c->backend_data = NULL;
}

const struct backend_ops backend_copyid = {
    copyid_start, copyid_data, copyid_eof, NULL, copyid_stop
};

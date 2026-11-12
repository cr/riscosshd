/* SPDX-License-Identifier: MIT */
/* Session backends.
 *
 * A backend owns what happens on one SSH channel once the client has asked
 * for "exec", "shell" or the "sftp" subsystem.  The server core (server.c)
 * handles transport, authentication, channel bookkeeping and pumping
 * output to the client; a backend only produces bytes (chan_output()) and
 * consumes input (data()).
 *
 * Backends today:
 *   backend_taskwindow  exec and shell on the desktop: a TaskWindow child
 *                  task with Message_TaskWindow_Input/Output relayed over
 *                  Wimp messages.
 *   backend_sftp   libssh's built-in SFTP server on the channel.
 *   backend_exec   text mode (-t) only: one *command through OS_CLI with
 *                  output captured to a file, then the channel closes.
 *   backend_shell  text mode only: a line-mode "*" prompt on the same
 *                  capture mechanism, one OS_CLI call per line.
 */
#ifndef SSHD_BACKEND_H
#define SSHD_BACKEND_H

#include <stddef.h>
#include <libssh/libssh.h>
#include <libssh/callbacks.h>
#include "buf.h"

struct chan_ctx;

/* start() results besides 0 (accepted). */
#define BACKEND_UNAVAILABLE (-1)  /* cannot serve this here: try another backend */
#define BACKEND_REFUSED     (-2)  /* refused; the reason is already on stderr */

struct backend_ops {
    const char *name;
    /* Begin the session.  command is NULL for a shell request.  Returns 0,
     * BACKEND_UNAVAILABLE or BACKEND_REFUSED. */
    int (*start)(struct chan_ctx *c, const char *command);
    /* Input from the client.  Return the number of bytes consumed. */
    int (*data)(struct chan_ctx *c, const char *buf, size_t len);
    /* Optional: the client sent EOF on the channel. */
    void (*eof)(struct chan_ctx *c);
    /* Optional: called on each pass of the main loop until the session
     * is finished. */
    void (*poll)(struct chan_ctx *c);
    /* Optional: the channel is going away; release backend state. */
    void (*stop)(struct chan_ctx *c);
};

struct chan_ctx {
    ssh_session session;
    ssh_channel channel;
    const char *owner_name;  /* user name, for log lines */

    int pty;                 /* client requested a pty: raw mode, CRLF output */
    int vdu_after_lf;        /* the last VDU byte sent was a LF (see chan_output_vdu) */
    unsigned char vdu[10];   /* a VDU code and its parameters so far */
    int vdu_have, vdu_need;

    const struct backend_ops *backend;
    void *backend_data;

    struct buf out, err;     /* waiting to be sent: stdout and stderr */

    int finished;            /* backend done; close once output drained */
    int exit_status;
    struct chan_ctx *retired_next;   /* server.c: list of replaced channels */
    /* libssh keeps a pointer to this in the channel until ssh_channel_free,
     * so each channel has its own, freed only after the channel. */
    struct ssh_channel_callbacks_struct cb;
};

/* Queue bytes for the client.  Returns 0 or -1 on allocation failure. */
int chan_output(struct chan_ctx *c, const char *data, size_t len);
int chan_output_str(struct chan_ctx *c, const char *s);
/* Queue text for the client's stderr (refusals, RISC OS errors). */
int chan_output_err(struct chan_ctx *c, const char *s);
/* Queue RISC OS VDU output.  RISC OS ends lines with LF CR: the CR after
 * a LF is dropped, and in pty mode each LF becomes CRLF while any other CR
 * (a line redrawing itself) passes through; in pipe mode every CR goes.
 * VDU control codes become ANSI sequences for a pty where a terminal has
 * an equivalent; otherwise they and their parameters are dropped. */
int chan_output_vdu(struct chan_ctx *c, const char *data, size_t len);
/* Mark the session finished with the given exit status. */
void chan_finish(struct chan_ctx *c, int status);
/* Answer a request with msg on stderr and exit status 1. */
void chan_refuse(struct chan_ctx *c, const char *msg);

/* Run a *command and queue its output on the channel.  Shared by the exec
 * and shell backends.  Returns the exit status. */
int backend_run_cli(struct chan_ctx *c, const char *command);

extern const struct backend_ops backend_exec;
extern const struct backend_ops backend_shell;
extern const struct backend_ops backend_sftp;

#endif

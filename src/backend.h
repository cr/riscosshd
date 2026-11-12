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
#ifndef BACKEND_H
#define BACKEND_H

#include <stddef.h>
#include <libssh/libssh.h>
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

    const struct backend_ops *backend;
    void *backend_data;

    struct buf out, err;     /* waiting to be sent: stdout and stderr */

    int finished;            /* backend done; close once output drained */
    int exit_status;
};

/* Queue bytes for the client.  Returns 0 or -1 on allocation failure. */
int chan_output(struct chan_ctx *c, const char *data, size_t len);
int chan_output_str(struct chan_ctx *c, const char *s);
/* Queue text for the client's stderr (refusals, RISC OS errors). */
int chan_output_err(struct chan_ctx *c, const char *s);
/* Queue captured RISC OS VDU output: drops CRs, and in pty mode turns each
 * LF into CRLF. */
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

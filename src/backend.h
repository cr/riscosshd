/* Session backends.
 *
 * A backend owns what happens on one SSH channel once the client has asked
 * for "exec" or "shell".  The server core (server.c) handles transport,
 * authentication, channel bookkeeping and pumping output to the client; a
 * backend only produces bytes (chan_output()) and consumes input (data()).
 *
 * Backends today:
 *   backend_exec   runs one *command through OS_CLI, output captured to a
 *                  file and returned, then the channel closes.
 *   backend_shell  a line-mode "*" prompt built on the same capture
 *                  mechanism, one OS_CLI call per line.
 *   backend_sftp   libssh's built-in SFTP server on the channel.
 *   backend_taskwindow  a real interactive session: a TaskWindow child
 *                  task with Message_TaskWindow_Input/Output relayed over
 *                  Wimp messages.  Used for shell requests when the server
 *                  runs as a Wimp task; backend_shell is the text-mode
 *                  fallback.
 */
#ifndef BACKEND_H
#define BACKEND_H

#include <stddef.h>
#include <libssh/libssh.h>

struct chan_ctx;

struct backend_ops {
    const char *name;
    /* Begin the session.  command is NULL for a shell request.  Return 0
     * to accept, -1 to refuse the request. */
    int (*start)(struct chan_ctx *c, const char *command);
    /* Input from the client.  Return the number of bytes consumed. */
    int (*data)(struct chan_ctx *c, const char *buf, size_t len);
    /* Client sent EOF on the channel. */
    void (*eof)(struct chan_ctx *c);
    /* Called once per main loop iteration while the channel is open. */
    void (*poll)(struct chan_ctx *c);
    /* Channel is going away; release backend state. */
    void (*stop)(struct chan_ctx *c);
};

struct chan_ctx {
    ssh_session session;
    ssh_channel channel;
    struct session_ctx *owner;
    const char *owner_name;  /* user name, for log lines */

    int pty;                 /* client requested a pty: raw mode, CRLF output */
    int cols, rows;
    char term[32];

    const struct backend_ops *backend;
    void *backend_data;

    /* Output waiting to be sent to the client. */
    char *out;
    size_t out_len, out_cap;

    int finished;            /* backend done; close once output drained */
    int exit_status;
    int closed;              /* exit status / EOF / close already sent */
};

/* Queue bytes for the client.  Returns 0 or -1 on allocation failure. */
int chan_output(struct chan_ctx *c, const char *data, size_t len);
int chan_output_str(struct chan_ctx *c, const char *s);
/* Queue captured RISC OS VDU output: drops CRs, and in pty mode turns each
 * LF into CRLF. */
int chan_output_vdu(struct chan_ctx *c, const char *data, size_t len);
/* Mark the session finished with the given exit status. */
void chan_finish(struct chan_ctx *c, int status);

/* Run a *command and queue its output on the channel.  Shared by the exec
 * and shell backends.  Returns the exit status. */
int backend_run_cli(struct chan_ctx *c, const char *command);

extern const struct backend_ops backend_exec;
extern const struct backend_ops backend_shell;
extern const struct backend_ops backend_sftp;

#endif

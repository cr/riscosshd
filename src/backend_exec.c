/* exec backend: one command through OS_CLI, output captured, channel closed.
 * Also hosts backend_run_cli(), the capture mechanism the shell backend
 * reuses.
 */
#include <stdlib.h>
#include <string.h>

#include "backend.h"
#include "config.h"
#include "riscos.h"
#include "log.h"

int backend_run_cli(struct chan_ctx *c, const char *command)
{
    char *out = NULL, *err = NULL;
    size_t out_len = 0;
    int status;

    log_debug("run: %s", command);
    status = ro_run_command(command, cfg.capture_path, &out, &out_len, &err);
    if (out) {
        /* Drop VDU control codes (paged-mode toggles, colour changes and
         * the like) that mean nothing to a terminal; keep tab, LF, CR. */
        size_t w = 0;
        for (size_t i = 0; i < out_len; i++) {
            unsigned char ch = (unsigned char)out[i];
            if (ch >= 0x20 || ch == '\t' || ch == '\n' || ch == '\r')
                out[w++] = out[i];
        }
        chan_output_vdu(c, out, w);
        free(out);
    }
    if (err) {
        /* RISC OS errors go to the client as a line on the same stream;
         * stderr is kept for a later split of the capture. */
        chan_output_str(c, c->pty ? "\r\n" : "\n");
        chan_output_str(c, "Error: ");
        chan_output_str(c, err);
        chan_output_str(c, c->pty ? "\r\n" : "\n");
        free(err);
    }
    return status;
}

static int exec_start(struct chan_ctx *c, const char *command)
{
    int status;
    if (!command)
        return -1;
    status = backend_run_cli(c, command);
    chan_finish(c, status);
    return 0;
}

static int exec_data(struct chan_ctx *c, const char *buf, size_t len)
{
    (void)c; (void)buf;
    return (int)len; /* commands take no stdin for now */
}

static void exec_noop(struct chan_ctx *c) { (void)c; }

const struct backend_ops backend_exec = {
    "exec", exec_start, exec_data, exec_noop, exec_noop, exec_noop
};

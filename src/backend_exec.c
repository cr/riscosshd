/* SPDX-License-Identifier: MIT */
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
        chan_output_vdu(c, out, out_len);
        free(out);
    }
    if (err) {
        chan_output_err(c, "Error: ");
        chan_output_err(c, err);
        chan_output_err(c, "\r\n");
        free(err);
    }
    return status;
}

static int exec_start(struct chan_ctx *c, const char *command)
{
    int status;
    if (!command)
        return BACKEND_UNAVAILABLE;
    status = backend_run_cli(c, command);
    chan_finish(c, status);
    return 0;
}

const struct backend_ops backend_exec = {
    "exec", exec_start, NULL, NULL, NULL, NULL
};

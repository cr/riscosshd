/* SPDX-License-Identifier: MIT */
/* shell backend: a minimal line-mode "*" prompt.  Each line is run through
 * OS_CLI with output captured, exactly like exec.  With a pty the client
 * terminal is in raw mode, so we echo and handle backspace ourselves.
 * Used only in text mode (-t); on the desktop backend_taskwindow serves
 * shell requests.
 */
#include <stdlib.h>
#include <string.h>

#include "backend.h"
#include "log.h"

#define LINE_MAX_LEN 1024

struct shell_state {
    char line[LINE_MAX_LEN];
    size_t len;
};

static void prompt(struct chan_ctx *c)
{
    chan_output_str(c, "*");
}

static void newline(struct chan_ctx *c)
{
    chan_output_str(c, c->pty ? "\r\n" : "\n");
}

static int shell_start(struct chan_ctx *c, const char *command)
{
    struct shell_state *s;
    (void)command;
    s = calloc(1, sizeof *s);
    if (!s) {
        chan_refuse(c, "Out of memory");
        return BACKEND_REFUSED;
    }
    c->backend_data = s;
    chan_output_str(c, "RISC OS command line over SSH. Type *commands; 'exit' or Ctrl-D to quit.");
    newline(c);
    prompt(c);
    return 0;
}

static void run_line(struct chan_ctx *c, struct shell_state *s)
{
    char *cmd = s->line;
    s->line[s->len] = '\0';
    s->len = 0;
    while (*cmd == ' ' || *cmd == '*')
        cmd++;
    if (*cmd == '\0') {
        prompt(c);
        return;
    }
    if (strcmp(cmd, "exit") == 0 || strcmp(cmd, "quit") == 0 || strcmp(cmd, "logout") == 0) {
        chan_finish(c, 0);
        return;
    }
    backend_run_cli(c, cmd);
    prompt(c);
}

static void shell_data(struct chan_ctx *c, const char *buf, size_t len)
{
    struct shell_state *s = c->backend_data;
    size_t i;

    for (i = 0; i < len && !c->finished; i++) {
        unsigned char ch = (unsigned char)buf[i];
        if (ch == '\r' || ch == '\n') {
            if (c->pty)
                newline(c);
            run_line(c, s);
            /* a pty client sends CR; swallow a following LF */
            if (ch == '\r' && i + 1 < len && buf[i + 1] == '\n')
                i++;
        } else if (ch == 0x04 && s->len == 0) {      /* Ctrl-D */
            if (c->pty)
                newline(c);
            chan_finish(c, 0);
        } else if (ch == 0x03) {                     /* Ctrl-C: drop line */
            s->len = 0;
            if (c->pty) {
                chan_output_str(c, "^C");
                newline(c);
                prompt(c);
            }
        } else if (ch == 0x7f || ch == 0x08) {        /* backspace */
            if (s->len > 0) {
                s->len--;
                if (c->pty)
                    chan_output_str(c, "\b \b");
            }
        } else if (ch >= 0x20 && s->len < LINE_MAX_LEN - 1) {
            s->line[s->len++] = (char)ch;
            if (c->pty)
                chan_output(c, (const char *)&ch, 1);
        }
    }
}

static void shell_eof(struct chan_ctx *c)
{
    struct shell_state *s = c->backend_data;
    if (s->len > 0 && !c->pty)
        run_line(c, s);           /* piped input without trailing newline */
    chan_finish(c, 0);
}

static void shell_stop(struct chan_ctx *c)
{
    free(c->backend_data);
}

const struct backend_ops backend_shell = {
    shell_start, shell_data, shell_eof, NULL, shell_stop
};

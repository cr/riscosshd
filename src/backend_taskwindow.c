/* TaskWindow backend.
 *
 * RISC OS has no pty, but the TaskWindow module gives a command-line
 * child task whose VDU output is delivered to a controlling Wimp task as
 * Message_TaskWindow_Output and whose keyboard input is fed with
 * Message_TaskWindow_Input.  That is what Edit's task windows use; here
 * the controlling task is sshd and the "window" is an SSH channel.
 *
 *   shell request      -> Wimp_StartTask("TaskWindow -task &us -txt &id")
 *   exec request       -> the command goes into a scratch Obey file that
 *                         sets SSHd$Rc<id> to 1 before it and 0 after it, run
 *                         with "TaskWindow \"Obey <file>\" ... -quit": an
 *                         error aborts the Obey file and leaves 1, which
 *                         becomes the SSH exit status.  Quotes are fine;
 *                         control characters and over 1000 characters are
 *                         refused.
 *   Message_TaskWindow_Ego     child announces itself: learn its task handle
 *   Message_TaskWindow_Output  child's VDU output -> channel
 *   channel data               -> Message_TaskWindow_Input, paced
 *   Message_TaskWindow_Morio   child exited -> close channel
 *   Message_TaskCloseDown      child killed some other way -> close channel
 *   channel closed             -> Message_TaskWindow_Morite kills the child
 *   output queue too long      -> Message_TaskWindow_Suspend, later _Resume
 *
 * The child's command line echoes what it reads, so nothing is echoed
 * locally; a DEL it echoes after a backspace is turned into the
 * backspace-space-backspace a terminal needs.
 */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <kernel.h>
#include <swis.h>

#include "backend_taskwindow.h"
#include "buf.h"
#include "config.h"
#include "riscos.h"
#include "log.h"

#ifndef Wimp_StartTask
#define Wimp_StartTask 0x400DE
#endif

#define MSG_DATA_MAX 232       /* 256-byte message minus the 24-byte header */
/* The TaskWindow module keeps a 256-byte key buffer and silently drops what
 * does not fit, so input is paced: one chunk at a time, cut at a line end,
 * and the next only once the child has produced output (it echoes what it
 * reads) or after a short wait. */
#define INPUT_PACE_CHUNK 128
#define INPUT_PACE_WAIT_CS 20
#define PENDING_MAX (1024 * 1024)   /* input queued for a child; beyond this it is dropped */
#define OUT_HIGH_WATER (256 * 1024)   /* suspend the child above this much unsent output */
#define OUT_LOW_WATER  (32 * 1024)    /* resume it below this */
#define EOF_QUIET_CS   100            /* non-pty shell: finish 1 s after the last output */
#define SCRAP_DIR "<Wimp$ScrapDir>.SSHd"

struct tw_state {
    struct chan_ctx *chan;     /* NULL once the channel went away before Ego */
    int txt;                   /* our id, echoed back in Message_TaskWindow_Ego */
    int child;                 /* child's Wimp task handle, 0 until Ego */
    int dead;                  /* Morio/TaskCloseDown received or kill sent */
    int suspended;             /* Message_TaskWindow_Suspend sent */
    int dropped;               /* input dropped because the child was not reading */
    int is_exec;               /* started with a command (Obey wrapper, -quit) */
    int eof_pending;           /* non-pty shell: client sent EOF */
    int last_activity;         /* centiseconds, OS_ReadMonotonicTime */
    int last_sent;             /* when the last input chunk went out */
    int awaiting_output;       /* a chunk is out and nothing came back yet */
    int esc;                   /* pty: inside a terminal escape sequence (ESC_*) */
    int cr_pending;            /* the last packet ended in CR: skip a leading LF */
    char line[16];             /* shell: start of the line being typed, for 'exit' */
    size_t line_len;           /* its full length, even past sizeof line */
    char *scrap;               /* exec: the Obey file, deleted at the end */
    char rcvar[24];            /* exec: "SSHd$Rc<id>" */
    struct buf pending;        /* input waiting for the child (paced, or not yet ready) */
    struct tw_state *next;
};

static int our_task;
static int next_txt = 0x53480001;
static struct tw_state *states;

void tw_set_task_handle(int handle)
{
    our_task = handle;
}

static struct tw_state *find_by_txt(int txt)
{
    for (struct tw_state *s = states; s; s = s->next)
        if (s->txt == txt)
            return s;
    return NULL;
}

static struct tw_state *find_by_child(int child)
{
    for (struct tw_state *s = states; s; s = s->next)
        if (s->child == child && child != 0)
            return s;
    return NULL;
}

static void unlink_state(struct tw_state *s)
{
    for (struct tw_state **pp = &states; *pp; pp = &(*pp)->next) {
        if (*pp == s) {
            *pp = s->next;
            break;
        }
    }
    if (s->scrap) {
        ro_delete(s->scrap);
        free(s->scrap);
    }
    if (s->rcvar[0])
        ro_unset_var(s->rcvar);
    buf_free(&s->pending);
    free(s);
}

static void send_to_child(struct tw_state *s, int action, const char *data, size_t len)
{
    int block[64];
    block[0] = (int)((24 + len + 3) & ~3u);
    block[3] = 0;
    block[4] = action;
    block[5] = (int)len;
    if (len)
        memcpy(&block[6], data, len);
    _swix(Wimp_SendMessage, _INR(0, 2), 17, block, s->child);
}

static void kill_child(struct tw_state *s)
{
    if (s->child && !s->dead) {
        send_to_child(s, Message_TaskWindow_Morite, NULL, 0);
        s->dead = 1;
        log_debug("task window %08x killed", (unsigned)s->txt);
    }
}

/* Send the next input chunk if the child is ready for one. */
static void flush_pending(struct tw_state *s)
{
    const char *d = buf_data(&s->pending);
    size_t n;
    if (!s->child || s->dead || s->pending.len == 0)
        return;
    if (s->awaiting_output && ro_cs_since(s->last_sent, ro_now_cs()) < INPUT_PACE_WAIT_CS)
        return;
    n = s->pending.len < INPUT_PACE_CHUNK ? s->pending.len : INPUT_PACE_CHUNK;
    for (size_t i = 0; i < n; i++) {     /* a line at a time for a line-reading child */
        if (d[i] == '\r') {
            n = i + 1;
            break;
        }
    }
    send_to_child(s, Message_TaskWindow_Input, d, n);
    buf_consume(&s->pending, n);
    s->last_sent = ro_now_cs();
    s->awaiting_output = 1;
}

static void queue_input(struct tw_state *s, const char *data, size_t len)
{
    if (s->pending.len + len > PENDING_MAX) {
        len = PENDING_MAX - s->pending.len;   /* drop the excess: the child is not reading */
        if (!s->dropped++)
            log_msg("task window %08x: input dropped, child not reading", (unsigned)s->txt);
    }
    buf_append(&s->pending, data, len);
    flush_pending(s);
}

/* ---- backend ops --------------------------------------------------------- */

/* Write the Obey wrapper for an exec command.  Returns the file's RISC OS
 * path (malloc'd) or NULL. */
static char *write_wrapper(struct tw_state *s, const char *command)
{
    char *dir = ro_gstrans(SCRAP_DIR), *path, *body;
    size_t n;
    const char *err;
    if (!dir)
        return NULL;
    ro_mkdir(dir);
    n = strlen(dir) + 16;
    path = malloc(n);
    if (!path) {
        free(dir);
        return NULL;
    }
    snprintf(path, n, "%s.%08X", dir, (unsigned)s->txt);
    free(dir);
    n = strlen(command) + 2 * sizeof s->rcvar + 32;
    body = malloc(n);
    if (!body) {
        free(path);
        return NULL;
    }
    snprintf(body, n, "Set %s 1\n%s\nSet %s 0\n", s->rcvar, command, s->rcvar);
    err = ro_save_file(path, 0xfeb, body, strlen(body));
    free(body);
    if (err) {
        log_msg("cannot write %s: %s", path, err);
        free(path);
        return NULL;
    }
    return path;
}

/* Release a state that was never linked, and refuse the request. */
static int refuse(struct chan_ctx *c, struct tw_state *s, const char *msg)
{
    if (s)
        unlink_state(s);
    chan_refuse(c, msg);
    return BACKEND_REFUSED;
}

static int tw_start(struct chan_ctx *c, const char *command)
{
    struct tw_state *s;
    char cmd[1200];
    int n;
    _kernel_oserror *e;

    if (!our_task)
        return BACKEND_UNAVAILABLE;   /* text mode: the OS_CLI backends serve */
    if (command) {
        /* The command becomes one line of an Obey file. */
        for (const char *p = command; *p; p++)
            if (((unsigned char)*p < 0x20 && *p != '\t') || *p == 0x7f)
                return refuse(c, NULL, "Command refused: control characters");
        if (strlen(command) > 1000)
            return refuse(c, NULL, "Command refused: over 1000 characters");
    }
    s = calloc(1, sizeof *s);
    if (!s)
        return refuse(c, NULL, "Command refused: out of memory");
    s->chan = c;
    s->txt = next_txt++;
    if (command) {
        s->is_exec = 1;
        snprintf(s->rcvar, sizeof s->rcvar, "SSHd$Rc%08X", (unsigned)s->txt);
        s->scrap = write_wrapper(s, command);
        if (!s->scrap)
            return refuse(c, s, "Command refused: cannot write the scrap file");
        n = snprintf(cmd, sizeof cmd, "TaskWindow \"Obey %s\" -task &%08X -txt &%08X -name \"SSH session\" -quit",
                     s->scrap, (unsigned)our_task, (unsigned)s->txt);
    } else {
        n = snprintf(cmd, sizeof cmd, "TaskWindow -task &%08X -txt &%08X -name \"SSH session\"",
                     (unsigned)our_task, (unsigned)s->txt);
    }
    if (n < 0 || (size_t)n >= sizeof cmd)
        return refuse(c, s, "Command refused: the scrap directory path is too long");
    e = _swix(Wimp_StartTask, _IN(0), cmd);
    if (e) {
        char msg[300];
        log_msg("cannot start TaskWindow: %s", e->errmess);
        snprintf(msg, sizeof msg, "Cannot start a TaskWindow: %s", e->errmess);
        return refuse(c, s, msg);
    }
    if (!command && c->pty && !cfg.motd)
        chan_output_str(c, "RISC OS command line (TaskWindow). Ctrl-C is Escape; "
                           "Ctrl-D or 'exit' ends the session.\r\n");
    /* Linked only now: the child's Ego cannot arrive before our next poll. */
    s->next = states;
    states = s;
    c->backend_data = s;
    log_debug("task window %08x started", (unsigned)s->txt);
    return 0;
}

enum { ESC_NONE, ESC_CSI, ESC_SS3 };

/* The shell's line as the client types it: what ReadLine will see. */
static void line_add(struct tw_state *s, unsigned char ch)
{
    if (ch == 0x7f) {
        if (s->line_len)
            s->line_len--;
    } else {
        if (s->line_len < sizeof s->line)
            s->line[s->line_len] = (char)ch;
        s->line_len++;
    }
}

/* 'exit', 'logout' or 'quit' (with or without a leading '*'), which end
 * a shell session as they would on Unix rather than reach OS_CLI. */
static int line_is_exit(const struct tw_state *s)
{
    char w[sizeof s->line + 1];
    const char *p = w;
    size_t n = s->line_len;
    if (n > sizeof s->line)
        return 0;
    memcpy(w, s->line, n);
    while (n && w[n - 1] == ' ')
        n--;
    w[n] = '\0';
    while (*p == ' ' || *p == '*')
        p++;
    return !strcmp(p, "exit") || !strcmp(p, "logout") || !strcmp(p, "quit");
}

static int tw_data(struct chan_ctx *c, const char *buf, size_t len)
{
    struct tw_state *s = c->backend_data;
    char out[MSG_DATA_MAX];
    size_t n = 0;

    for (size_t i = 0; i < len; i++) {
        unsigned char ch = (unsigned char)buf[i];
        if (n == sizeof out) {           /* translate in message-sized chunks */
            queue_input(s, out, n);
            n = 0;
        }
        /* A terminal sends cursor and editing keys as escape sequences
         * (ESC [ ... final, or ESC O x).  ReadLine has no use for them, and
         * passing them on would turn the ESC into an Escape condition and
         * type the rest onto the command line, so they are dropped; a lone
         * ESC key is still Escape. */
        if (s->esc == ESC_CSI) {
            if (ch >= 0x40 && ch <= 0x7e)
                s->esc = ESC_NONE;       /* final byte */
            continue;
        }
        if (s->esc == ESC_SS3) {
            s->esc = ESC_NONE;
            continue;
        }
        if (s->cr_pending) {             /* CRLF split across two packets */
            s->cr_pending = 0;
            if (ch == '\n')
                continue;
        }
        if (c->pty && ch == 0x1b && i + 1 < len && (buf[i + 1] == '[' || buf[i + 1] == 'O')) {
            s->esc = buf[i + 1] == '[' ? ESC_CSI : ESC_SS3;
            i++;
            continue;
        }
        if (ch == '\n' || ch == '\r') {
            if (!s->is_exec && line_is_exit(s)) {
                chan_output_str(c, "\r\n");
                chan_finish(c, 0);
                break;
            }
            s->line_len = 0;
            out[n++] = '\r';                 /* RISC OS line end for ReadLine */
            if (ch == '\r') {
                if (i + 1 == len)
                    s->cr_pending = 1;
                else if (buf[i + 1] == '\n')
                    i++;
            }
        } else if (!c->pty) {
            out[n++] = (char)ch;             /* a pipe: bytes are data, not keys */
            line_add(s, ch);
        } else if (ch == 0x7f || ch == 0x08) {
            out[n++] = 0x7f;                 /* delete; ReadLine understands 127 */
            line_add(s, 0x7f);
        } else if (ch == 0x03) {
            out[n++] = 0x1b;                 /* Ctrl-C -> Escape */
            s->line_len = 0;
            if (c->out.len < OUT_HIGH_WATER) /* the echo is not worth unbounded memory */
                chan_output_str(c, "^C\r\n");
        } else if (ch == 0x04) {
            if (s->line_len == 0) {          /* Ctrl-D on an empty line ends it */
                chan_output_str(c, "\r\n");
                chan_finish(c, 0);
                break;
            }
        } else {
            out[n++] = (char)ch;
            line_add(s, ch);
        }
    }
    if (n)
        queue_input(s, out, n);
    return (int)len;
}

static void tw_eof(struct chan_ctx *c)
{
    struct tw_state *s = c->backend_data;
    /* Piped input is finished.  An exec channel ends when its command
     * does; a pty shell with Ctrl-D or the child's exit; a plain piped
     * shell once the child has gone quiet. */
    if (!c->pty && !s->is_exec) {
        s->eof_pending = 1;
        s->last_activity = ro_now_cs();
    }
}

static void tw_poll(struct chan_ctx *c)
{
    struct tw_state *s = c->backend_data;
    flush_pending(s);
    /* Flow control: a child printing faster than the client reads would
     * otherwise grow the channel's output queue without bound. */
    if (s->child && !s->dead) {
        if (!s->suspended && c->out.len > OUT_HIGH_WATER) {
            send_to_child(s, Message_TaskWindow_Suspend, NULL, 0);
            s->suspended = 1;
        } else if (s->suspended && c->out.len < OUT_LOW_WATER) {
            send_to_child(s, Message_TaskWindow_Resume, NULL, 0);
            s->suspended = 0;
        }
    }
    if (s->eof_pending && s->pending.len == 0 &&
        ro_cs_since(s->last_activity, ro_now_cs()) > EOF_QUIET_CS)
        chan_finish(c, 0);
}

static void tw_stop(struct chan_ctx *c)
{
    struct tw_state *s = c->backend_data;
    if (!s)
        return;
    c->backend_data = NULL;
    if (s->child) {
        kill_child(s);
        unlink_state(s);
    } else {
        /* The child has not said Ego yet; keep the record, with no channel,
         * so that it can be killed when it does instead of living on
         * unattended. */
        s->chan = NULL;
    }
}

const struct backend_ops backend_taskwindow = {
    "taskwindow", tw_start, tw_data, tw_eof, tw_poll, tw_stop
};

/* ---- Wimp messages ------------------------------------------------------- */

/* The exit status of a child: for exec, SSHd$Rc<id> is 1 until the
 * command finished without error, so an error, an Escape or a kill all
 * give 1; a shell that exits (*Quit) gives 0. */
static int exit_status(struct tw_state *s)
{
    char val[16];
    if (!s->is_exec)
        return 0;
    if (ro_read_var(s->rcvar, val, sizeof val) < 0)
        return 1;
    return atoi(val) == 0 ? 0 : 1;
}

/* Only states with a child reach here, and those always have a channel:
 * a state whose channel went first is killed and dropped when its Ego
 * arrives. */
static void child_gone(struct tw_state *s, const char *how)
{
    s->dead = 1;
    log_debug("task window %08x %s", (unsigned)s->txt, how);
    if (!s->chan->finished)              /* Ctrl-D or exit already set the status */
        chan_finish(s->chan, exit_status(s));
}

int tw_message(int reason, int *block)
{
    int sender = block[1], action = block[4];
    struct tw_state *s;

    switch (action) {
    case Message_TaskWindow_Ego:
        s = find_by_txt(block[5]);
        if (s) {
            s->child = sender;
            if (!s->chan) {                  /* its channel went before it started */
                kill_child(s);
                unlink_state(s);
            } else {
                log_debug("task window ready for %s", s->chan->owner_name);
                flush_pending(s);
            }
        }
        return 1;
    case Message_TaskWindow_Output:
        s = find_by_child(sender);
        if (s && !s->chan->finished) {
            const char *d = (const char *)&block[6];
            size_t n = (size_t)block[5], start = 0;
            /* the length comes from another task: keep it inside the message */
            size_t max = block[0] > 24 ? (size_t)(block[0] - 24) : 0;
            if (n > max)
                n = max;
            if (s->chan->pty) {
                /* ReadLine echoes DEL for a deleted character; terminals
                 * need backspace, space, backspace to show that. */
                for (size_t i = 0; i < n; i++) {
                    if (d[i] == 0x7f) {
                        chan_output_vdu(s->chan, d + start, i - start);
                        chan_output_str(s->chan, "\b \b");
                        start = i + 1;
                    }
                }
            }
            chan_output_vdu(s->chan, d + start, n - start);
            s->last_activity = ro_now_cs();
        }
        if (s) {
            s->awaiting_output = 0;          /* the child consumed input: send more */
            flush_pending(s);
        }
        if (reason == 18) {                  /* acknowledge, or the child kills itself */
            block[3] = block[2];
            _swix(Wimp_SendMessage, _INR(0, 2), 19, block, sender);
        }
        return 1;
    case Message_TaskWindow_Morio:
        s = find_by_child(sender);
        if (s)
            child_gone(s, "exited");
        return 1;
    case Message_TaskCloseDown:              /* killed from the Task Manager, say */
        s = find_by_child(sender);
        if (s)
            child_gone(s, "closed down");
        return 1;
    default:
        return 0;
    }
}

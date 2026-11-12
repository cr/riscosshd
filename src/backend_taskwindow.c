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
 *                         becomes the SSH exit status.  Quotes and length
 *                         are no longer a problem.
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
#include "riscos.h"
#include "log.h"

#ifndef Wimp_StartTask
#define Wimp_StartTask 0x400DE
#endif

#define MAX_INPUT_CHUNK 232    /* 256-byte message minus the 24-byte header */
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
    int orphaned;              /* channel gone before Ego: kill on arrival */
    int suspended;             /* Message_TaskWindow_Suspend sent */
    int dropped;               /* input dropped because the child was not reading */
    int is_exec;               /* started with a command (Obey wrapper, -quit) */
    int eof_pending;           /* non-pty shell: client sent EOF */
    int last_activity;         /* centiseconds, OS_ReadMonotonicTime */
    int last_sent;             /* when the last input chunk went out */
    int awaiting_output;       /* a chunk is out and nothing came back yet */
    char *scrap;               /* exec: the Obey file, deleted at the end */
    char rcvar[24];            /* exec: "SSHd$Rc<id>" */
    char *pending;             /* input waiting for the child (paced, or not yet ready) */
    size_t npending, pcap;
    struct tw_state *next;
};

static int our_task;
static int next_txt = 0x53480001;
static struct tw_state *states;

void tw_set_task_handle(int handle)
{
    our_task = handle;
}

int tw_available(void)
{
    return our_task != 0;
}

static int now_cs(void)
{
    int t = 0;
    _swix(OS_ReadMonotonicTime, _OUT(0), &t);
    return t;
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
    free(s->pending);
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
    size_t n;
    if (!s->child || s->dead || s->npending == 0)
        return;
    if (s->awaiting_output && now_cs() - s->last_sent < INPUT_PACE_WAIT_CS)
        return;
    n = s->npending < INPUT_PACE_CHUNK ? s->npending : INPUT_PACE_CHUNK;
    for (size_t i = 0; i < n; i++) {     /* a line at a time for a line-reading child */
        if (s->pending[i] == '\r') {
            n = i + 1;
            break;
        }
    }
    send_to_child(s, Message_TaskWindow_Input, s->pending, n);
    memmove(s->pending, s->pending + n, s->npending - n);
    s->npending -= n;
    s->last_sent = now_cs();
    s->awaiting_output = 1;
}

static void queue_input(struct tw_state *s, const char *data, size_t len)
{
    if (s->npending + len > PENDING_MAX) {
        len = PENDING_MAX - s->npending;   /* drop the excess: the child is not reading */
        if (!s->dropped++)
            log_msg("task window %08x: input dropped, child not reading", (unsigned)s->txt);
    }
    if (s->npending + len > s->pcap) {
        size_t ncap = s->pcap ? s->pcap * 2 : 4096;
        char *n;
        while (ncap < s->npending + len)
            ncap *= 2;
        n = realloc(s->pending, ncap);
        if (!n)
            return;
        s->pending = n;
        s->pcap = ncap;
    }
    memcpy(s->pending + s->npending, data, len);
    s->npending += len;
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

static char last_refusal[160] = "Command refused";

const char *tw_last_refusal(void)
{
    return last_refusal;
}

static int tw_start(struct chan_ctx *c, const char *command)
{
    struct tw_state *s;
    char cmd[320];
    _kernel_oserror *e;

    if (!our_task)
        return -1;           /* text mode: let the line shell / capture take over */
    if (command) {
        /* The command becomes one line of an Obey file. */
        for (const char *p = command; *p; p++)
            if (((unsigned char)*p < 0x20 && *p != '\t') || *p == 0x7f) {
                snprintf(last_refusal, sizeof last_refusal, "Command refused: control characters");
                return -1;
            }
        if (strlen(command) > 1000) {
            snprintf(last_refusal, sizeof last_refusal, "Command refused: over 1000 characters");
            return -1;
        }
    }
    s = calloc(1, sizeof *s);
    if (!s) {
        snprintf(last_refusal, sizeof last_refusal, "Command refused: out of memory");
        return -1;
    }
    s->chan = c;
    s->txt = next_txt++;
    s->last_activity = now_cs();
    s->next = states;
    states = s;
    c->backend_data = s;

    if (command) {
        s->is_exec = 1;
        snprintf(s->rcvar, sizeof s->rcvar, "SSHd$Rc%08X", (unsigned)s->txt);
        s->scrap = write_wrapper(s, command);
        if (!s->scrap) {
            snprintf(last_refusal, sizeof last_refusal, "Command refused: cannot write the scrap file");
            unlink_state(s);
            c->backend_data = NULL;
            return -1;
        }
        snprintf(cmd, sizeof cmd, "TaskWindow \"Obey %s\" -task &%08X -txt &%08X -name \"SSH session\" -quit",
                 s->scrap, (unsigned)our_task, (unsigned)s->txt);
    } else {
        snprintf(cmd, sizeof cmd, "TaskWindow -task &%08X -txt &%08X -name \"SSH session\"",
                 (unsigned)our_task, (unsigned)s->txt);
    }
    e = _swix(Wimp_StartTask, _IN(0), cmd);
    if (e) {
        log_msg("cannot start TaskWindow: %s", e->errmess);
        snprintf(last_refusal, sizeof last_refusal, "Cannot start a TaskWindow: %s", e->errmess);
        unlink_state(s);
        c->backend_data = NULL;
        return -1;                       /* caller falls back or refuses */
    }
    log_debug("task window %08x started", (unsigned)s->txt);
    return 0;
}

static int tw_data(struct chan_ctx *c, const char *buf, size_t len)
{
    struct tw_state *s = c->backend_data;
    char out[MAX_INPUT_CHUNK];
    size_t n = 0;

    for (size_t i = 0; i < len; i++) {
        unsigned char ch = (unsigned char)buf[i];
        if (n == sizeof out) {           /* translate in message-sized chunks */
            queue_input(s, out, n);
            n = 0;
        }
        if (ch == '\n' || ch == '\r') {
            out[n++] = '\r';                 /* RISC OS line end for ReadLine */
            if (ch == '\r' && i + 1 < len && buf[i + 1] == '\n')
                i++;
        } else if (!c->pty) {
            out[n++] = (char)ch;             /* a pipe: bytes are data, not keys */
        } else if (ch == 0x7f || ch == 0x08) {
            out[n++] = 0x7f;                 /* delete; ReadLine understands 127 */
        } else if (ch == 0x03) {
            out[n++] = 0x1b;                 /* Ctrl-C -> Escape */
            if (c->out_len < OUT_HIGH_WATER) /* the echo is not worth unbounded memory */
                chan_output_str(c, "^C\r\n");
        } else if (ch == 0x04) {
            chan_output_str(c, "\r\n");
            chan_finish(c, 0);               /* Ctrl-D ends the session */
            break;
        } else {
            out[n++] = (char)ch;
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
        s->last_activity = now_cs();
    }
}

static void tw_poll(struct chan_ctx *c)
{
    struct tw_state *s = c->backend_data;
    flush_pending(s);
    /* Flow control: a child printing faster than the client reads would
     * otherwise grow the channel's output queue without bound. */
    if (s->child && !s->dead) {
        if (!s->suspended && c->out_len > OUT_HIGH_WATER) {
            send_to_child(s, Message_TaskWindow_Suspend, NULL, 0);
            s->suspended = 1;
        } else if (s->suspended && c->out_len < OUT_LOW_WATER) {
            send_to_child(s, Message_TaskWindow_Resume, NULL, 0);
            s->suspended = 0;
        }
    }
    if (s->eof_pending && s->npending == 0 && !c->finished &&
        now_cs() - s->last_activity > EOF_QUIET_CS)
        chan_finish(c, 0);
}

static void tw_stop(struct chan_ctx *c)
{
    struct tw_state *s = c->backend_data;
    if (!s)
        return;
    c->backend_data = NULL;
    if (s->child || s->dead) {
        kill_child(s);
        unlink_state(s);
    } else {
        /* The child has not said Ego yet; keep the record so that it can
         * be killed when it does instead of living on unattended. */
        s->chan = NULL;
        s->orphaned = 1;
    }
}

const struct backend_ops backend_taskwindow = {
    "taskwindow", tw_start, tw_data, tw_eof, tw_poll, tw_stop
};

/* ---- Wimp messages ------------------------------------------------------- */

/* The exit status of an exec child: SSHd$Rc<id> is 1 until the command
 * finished without error, so an error, an Escape or a kill all give 1. */
static int exec_status(struct tw_state *s)
{
    char val[16];
    if (!s->is_exec || ro_read_var(s->rcvar, val, sizeof val) < 0)
        return 1;
    return atoi(val) == 0 ? 0 : 1;
}

static void child_gone(struct tw_state *s, const char *how)
{
    s->dead = 1;
    log_debug("task window %08x %s", (unsigned)s->txt, how);
    if (s->chan)
        chan_finish(s->chan, exec_status(s));
    else
        unlink_state(s);          /* orphan: nothing else refers to it */
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
            if (s->orphaned) {
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
        if (s && s->chan && !s->chan->finished) {
            const char *d = (const char *)&block[6];
            size_t n = (size_t)block[5], start = 0;
            /* the length comes from another task: keep it inside the message */
            if (block[0] < 24 || n > (size_t)(block[0] - 24) || n > MAX_INPUT_CHUNK)
                n = block[0] > 24 ? (size_t)(block[0] - 24) : 0;
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
            s->last_activity = now_cs();
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
    case Message_TaskWindow_NewTask:
        return 1;                            /* nested task windows: ignored */
    default:
        return 0;
    }
}

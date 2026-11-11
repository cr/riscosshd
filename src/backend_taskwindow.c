/* TaskWindow backend.
 *
 * RISC OS has no pty, but the TaskWindow module gives a command-line
 * child task whose VDU output is delivered to a controlling Wimp task as
 * Message_TaskWindow_Output and whose keyboard input is fed with
 * Message_TaskWindow_Input.  That is what Edit's task windows use; here
 * the controlling task is sshd and the "window" is an SSH channel.
 *
 *   shell request  -> Wimp_StartTask("TaskWindow -task &us -txt &id ...")
 *   Message_TaskWindow_Ego     child announces itself: learn its task handle
 *   Message_TaskWindow_Output  child's VDU output -> channel
 *   channel data               -> Message_TaskWindow_Input (<= 232 bytes each)
 *   Message_TaskWindow_Morio   child exited -> close channel
 *   channel closed             -> Message_TaskWindow_Morite kills the child
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
#include "log.h"

#ifndef Wimp_StartTask
#define Wimp_StartTask 0x400DE
#endif

#define MAX_INPUT_CHUNK 232    /* 256-byte message minus the 24-byte header */
#define PENDING_MAX 4096

struct tw_state {
    struct chan_ctx *chan;
    int txt;                   /* our id, echoed back in Message_TaskWindow_Ego */
    int child;                 /* child's Wimp task handle, 0 until Ego */
    int dead;                  /* Morio received or kill sent */
    int dropped;               /* input dropped because the child was not reading */
    char pending[PENDING_MAX]; /* input typed before the child was ready */
    size_t npending;
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

static void flush_pending(struct tw_state *s)
{
    size_t off = 0;
    if (!s->child || s->dead)
        return;
    while (off < s->npending) {
        size_t n = s->npending - off;
        if (n > MAX_INPUT_CHUNK)
            n = MAX_INPUT_CHUNK;
        send_to_child(s, Message_TaskWindow_Input, s->pending + off, n);
        off += n;
    }
    s->npending = 0;
}

static void queue_input(struct tw_state *s, const char *data, size_t len)
{
    if (s->npending + len > PENDING_MAX) {
        len = PENDING_MAX - s->npending;   /* drop the excess: the child is not reading */
        if (!s->dropped++)
            log_msg("task window %08x: input dropped, child not reading", (unsigned)s->txt);
    }
    memcpy(s->pending + s->npending, data, len);
    s->npending += len;
    flush_pending(s);
}

/* ---- backend ops --------------------------------------------------------- */

static int tw_start(struct chan_ctx *c, const char *command)
{
    struct tw_state *s;
    char cmd[320];
    _kernel_oserror *e;

    if (!our_task)
        return -1;           /* text mode: let the line shell / capture take over */
    /* The command rides inside a quoted TaskWindow argument, so quotes in
     * it cannot be passed through; the caller falls back to OS_CLI capture. */
    if (command && (strchr(command, '"') || strlen(command) > 200))
        return -1;
    s = calloc(1, sizeof *s);
    if (!s)
        return -1;
    s->chan = c;
    s->txt = next_txt++;
    s->next = states;
    states = s;
    c->backend_data = s;

    if (command)
        snprintf(cmd, sizeof cmd, "TaskWindow \"%s\" -task &%08X -txt &%08X -name \"SSH session\" -quit",
                 command, (unsigned)our_task, (unsigned)s->txt);
    else
        snprintf(cmd, sizeof cmd, "TaskWindow -task &%08X -txt &%08X -name \"SSH session\"",
                 (unsigned)our_task, (unsigned)s->txt);
    e = _swix(Wimp_StartTask, _IN(0), cmd);
    if (e) {
        log_msg("cannot start TaskWindow: %s", e->errmess);
        states = s->next;
        free(s);
        c->backend_data = NULL;
        return -1;                       /* caller falls back to the line shell */
    }
    log_debug("task window %08x started", (unsigned)s->txt);
    return 0;
}

static int tw_data(struct chan_ctx *c, const char *buf, size_t len)
{
    struct tw_state *s = c->backend_data;
    char out[PENDING_MAX];
    size_t n = 0;

    for (size_t i = 0; i < len && n < sizeof out - 2; i++) {
        unsigned char ch = (unsigned char)buf[i];
        if (ch == '\n' || ch == '\r') {
            out[n++] = '\r';                 /* RISC OS line end for ReadLine */
            if (ch == '\r' && i + 1 < len && buf[i + 1] == '\n')
                i++;
        } else if (ch == 0x7f || ch == 0x08) {
            out[n++] = 0x7f;                 /* delete; ReadLine understands 127 */
        } else if (ch == 0x03) {
            out[n++] = 0x1b;                 /* Ctrl-C -> Escape */
            if (c->pty)
                chan_output_str(c, "^C\r\n");
        } else if (ch == 0x04) {
            if (c->pty)
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
    (void)c;   /* piped input finished; the child keeps running until the channel closes */
}

static void tw_poll(struct chan_ctx *c)
{
    flush_pending(c->backend_data);
}

static void tw_stop(struct chan_ctx *c)
{
    struct tw_state *s = c->backend_data, **pp;
    if (!s)
        return;
    if (s->child && !s->dead) {
        send_to_child(s, Message_TaskWindow_Morite, NULL, 0);
        log_debug("task window %08x killed", (unsigned)s->txt);
    }
    for (pp = &states; *pp; pp = &(*pp)->next) {
        if (*pp == s) {
            *pp = s->next;
            break;
        }
    }
    free(s);
    c->backend_data = NULL;
}

const struct backend_ops backend_taskwindow = {
    "taskwindow", tw_start, tw_data, tw_eof, tw_poll, tw_stop
};

/* ---- Wimp messages ------------------------------------------------------- */

int tw_message(int reason, int *block)
{
    int sender = block[1], action = block[4];
    struct tw_state *s;

    switch (action) {
    case Message_TaskWindow_Ego:
        s = find_by_txt(block[5]);
        if (s) {
            s->child = sender;
            log_msg("task window ready for %s", s->chan->owner_name);
            flush_pending(s);
        }
        return 1;
    case Message_TaskWindow_Output:
        s = find_by_child(sender);
        if (s && !s->chan->finished) {
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
        }
        if (reason == 18) {                      /* acknowledge, or the child kills itself */
            block[3] = block[2];
            _swix(Wimp_SendMessage, _INR(0, 2), 19, block, sender);
        }
        return 1;
    case Message_TaskWindow_Morio:
        s = find_by_child(sender);
        if (s) {
            s->dead = 1;
            log_debug("task window %08x exited", (unsigned)s->txt);
            chan_finish(s->chan, 0);
        }
        return 1;
    case Message_TaskWindow_NewTask:
        return 1;                                /* nested task windows: ignored */
    default:
        return 0;
    }
}

/* SPDX-License-Identifier: MIT */
/* SFTP backend: libssh's built-in SFTP server (sftpserver.c) running on
 * the channel.  It works on POSIX file calls, which UnixLib maps onto
 * RISC OS: "/dir/file.ext" is "dir.file/ext" on the current disc, and the
 * directory the client starts in is the root setting (or -r).
 * Modern scp (OpenSSH 9+) uses SFTP, so it works too; the legacy scp -O
 * protocol is not supported.
 *
 * The session stays non-blocking.  libssh's server writes each reply from
 * inside the request handler; if that write met a full channel window in
 * blocking mode it would wait for a window update that libssh cannot read
 * while the same socket's callback is running (poll handles are locked to
 * write events during recursion), freezing the server and the desktop.
 * So incoming bytes are queued here and requests are handled from poll(),
 * one at a time, only while the window can take the largest reply.
 */
#include <stdlib.h>
#include <string.h>
#include <libssh/libssh.h>
#include <libssh/sftp.h>
#include <libssh/sftpserver.h>

#include "backend.h"
#include "buf.h"
#include "log.h"
#include "prof.h"

/* Added to libssh by deps/patches/libssh-riscos.patch. */
void sftp_server_close_handles(sftp_session sftp);

/* Largest reply the patched server sends: a 32 KB read plus headers. */
#define REPLY_MAX (32768 + 64)
#define QUEUE_MAX (1024 * 1024)
/* Requests handled per poll: a queue of small requests from one client
 * must not hold up the other sessions and the desktop. */
#define REQUESTS_PER_POLL 16

struct sftp_state {
    sftp_session sftp;
    struct buf q;             /* bytes received, not yet handled */
};

static int sftp_start(struct chan_ctx *c, const char *command)
{
    struct sftp_state *st;
    (void)command;
    st = calloc(1, sizeof *st);
    if (!st)
        return -1;
    if (sftp_channel_default_subsystem_request(c->session, c->channel, "sftp", &st->sftp) != SSH_OK) {
        log_msg("%s: cannot start the SFTP server", c->owner_name);
        free(st);
        return -1;
    }
    c->backend_data = st;
    return 0;
}

/* Queue everything; requests are processed from sftp_poll(). */
static void sftp_data(struct chan_ctx *c, const char *buf, size_t len)
{
    struct sftp_state *st = c->backend_data;
    if (st->q.len + len > QUEUE_MAX) {
        log_msg("%s: sftp request queue over %d bytes, dropping the session", c->owner_name, QUEUE_MAX);
        chan_finish(c, 1);
    } else if (buf_append(&st->q, buf, len) < 0) {
        log_msg("%s: out of memory for the sftp queue, dropping the session", c->owner_name);
        chan_finish(c, 1);
    }
}

/* Length of the complete SFTP packet at the head of the queue, or 0. */
static size_t packet_len(const struct buf *q)
{
    const unsigned char *d = (const unsigned char *)buf_data(q);
    size_t plen;
    if (q->len < 4)
        return 0;
    plen = ((size_t)d[0] << 24) | ((size_t)d[1] << 16) | ((size_t)d[2] << 8) | d[3];
    if (plen > q->len - 4)        /* written so that it cannot wrap */
        return 0;
    return plen + 4;
}

static void sftp_poll(struct chan_ctx *c)
{
    struct sftp_state *st = c->backend_data;
    size_t n;
    int done = 0;

    while (!c->finished && done++ < REQUESTS_PER_POLL && (n = packet_len(&st->q)) > 0) {
        int rc;
        if (ssh_channel_window_size(c->channel) < REPLY_MAX)
            return;                                   /* let the client catch up */
        {
            PROF_START(t);
            rc = sftp_channel_default_data_callback(c->session, c->channel, buf_data(&st->q),
                                                    (uint32_t)n, 0, &st->sftp);
            PROF_STOP(PROF_SFTP, t);
        }
        if (rc < 0) {
            /* A malformed packet, or the server stopped: end the session.
             * libssh closes the channel itself in some of these cases;
             * chan_pump closes it if it is still open. */
            chan_finish(c, 1);
            return;
        }
        buf_consume(&st->q, n);
    }
}

static void sftp_eof(struct chan_ctx *c)
{
    chan_finish(c, 0);
}

static void sftp_stop(struct chan_ctx *c)
{
    struct sftp_state *st = c->backend_data;
    sftp_server_close_handles(st->sftp);   /* files a dropped client left open */
    sftp_server_free(st->sftp);
    buf_free(&st->q);
    free(st);
}

const struct backend_ops backend_sftp = {
    sftp_start, sftp_data, sftp_eof, sftp_poll, sftp_stop
};

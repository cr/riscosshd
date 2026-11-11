/* SFTP backend: libssh's built-in SFTP server (sftpserver.c) running on
 * the channel.  It works on POSIX file calls, which UnixLib maps onto
 * RISC OS: "/dir/file.ext" is "dir.file/ext" on the current disc, and the
 * directory the client starts in is the SFTP root given with -r.
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
#include "log.h"

/* Added to libssh by deps/patches/libssh-riscos.patch. */
void sftp_server_close_handles(sftp_session sftp);

/* Largest reply the patched server sends: a 32 KB read plus headers. */
#define REPLY_MAX (32768 + 64)
#define QUEUE_MAX (1024 * 1024)

struct sftp_state {
    sftp_session sftp;
    char *q;
    size_t qlen, qcap;
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
static int sftp_data(struct chan_ctx *c, const char *buf, size_t len)
{
    struct sftp_state *st = c->backend_data;
    if (st->qlen + len > st->qcap) {
        size_t ncap = st->qcap ? st->qcap : 65536;
        char *n;
        while (ncap < st->qlen + len)
            ncap *= 2;
        if (ncap > QUEUE_MAX) {
            log_msg("%s: sftp request queue over %d bytes, dropping the session", c->owner_name, QUEUE_MAX);
            chan_finish(c, 1);
            return (int)len;
        }
        n = realloc(st->q, ncap);
        if (!n)
            return 0;
        st->q = n;
        st->qcap = ncap;
    }
    memcpy(st->q + st->qlen, buf, len);
    st->qlen += len;
    return (int)len;
}

/* Length of the complete SFTP packet at the head of the queue, or 0. */
static size_t packet_len(const struct sftp_state *st)
{
    const unsigned char *d = (const unsigned char *)st->q;
    size_t plen;
    if (st->qlen < 4)
        return 0;
    plen = ((size_t)d[0] << 24) | ((size_t)d[1] << 16) | ((size_t)d[2] << 8) | d[3];
    if (plen + 4 > st->qlen)
        return 0;
    return plen + 4;
}

static void sftp_poll(struct chan_ctx *c)
{
    struct sftp_state *st = c->backend_data;
    size_t n;

    while (!c->finished && (n = packet_len(st)) > 0) {
        int rc;
        if (ssh_channel_window_size(c->channel) < REPLY_MAX)
            return;                                   /* let the client catch up */
        rc = sftp_channel_default_data_callback(c->session, c->channel, st->q, (uint32_t)n, 0, &st->sftp);
        if (rc < 0) {
            /* protocol error: libssh sent EOF and closed the channel */
            c->finished = 1;
            c->closed = 1;
            return;
        }
        memmove(st->q, st->q + n, st->qlen - n);
        st->qlen -= n;
    }
}

static void sftp_eof(struct chan_ctx *c)
{
    chan_finish(c, 0);
}

static void sftp_stop(struct chan_ctx *c)
{
    struct sftp_state *st = c->backend_data;
    if (st) {
        if (st->sftp) {
            sftp_server_close_handles(st->sftp);   /* files a dropped client left open */
            sftp_server_free(st->sftp);
        }
        free(st->q);
        free(st);
    }
    c->backend_data = NULL;
}

const struct backend_ops backend_sftp = {
    "sftp", sftp_start, sftp_data, sftp_eof, sftp_poll, sftp_stop
};

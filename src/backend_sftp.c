/* SFTP backend: libssh's built-in SFTP server (sftpserver.c) running on
 * the channel.  It works on POSIX file calls, which UnixLib maps onto
 * RISC OS: "/dir/file.ext" is "dir.file/ext" on the current disc, and the
 * directory the client starts in is the SFTP root given with -r.
 * Modern scp (OpenSSH 9+) uses SFTP, so it works too; the legacy scp -O
 * protocol is not supported.
 */
#include <stdlib.h>
#include <libssh/libssh.h>
#include <libssh/sftp.h>
#include <libssh/sftpserver.h>

#include "backend.h"
#include "log.h"

static int sftp_start(struct chan_ctx *c, const char *command)
{
    sftp_session *sp;
    (void)command;
    sp = calloc(1, sizeof *sp);
    if (!sp)
        return -1;
    if (sftp_channel_default_subsystem_request(c->session, c->channel, "sftp", sp) != SSH_OK) {
        log_msg("%s: cannot start the SFTP server", c->owner_name);
        free(sp);
        return -1;
    }
    c->backend_data = sp;
    /* The SFTP server writes replies itself; a blocking session lets a
     * large reply wait for channel window space instead of failing. */
    ssh_set_blocking(c->session, 1);
    return 0;
}

static int sftp_data(struct chan_ctx *c, const char *buf, size_t len)
{
    sftp_session *sp = c->backend_data;
    size_t off = 0;
    int rc = 0;

    /* libssh calls this once per incoming SSH packet and the default
     * handler processes one SFTP packet per call, so several SFTP packets
     * in one burst (the tail of a write plus a CLOSE, say) would leave the
     * later ones stranded until more data arrived.  Drain what is complete. */
    while (off < len) {
        rc = sftp_channel_default_data_callback(c->session, c->channel, (void *)(buf + off),
                                                (uint32_t)(len - off), 0, sp);
        if (rc <= 0)
            break;
        off += (size_t)rc;
    }
    if (rc < 0 && !ssh_channel_is_open(c->channel)) {
        /* a real protocol error: libssh sent EOF and closed the channel */
        c->finished = 1;
        c->closed = 1;
    }
    /* rc < 0 with the channel open just means "incomplete packet, call
     * again with more"; whatever was consumed is reported. */
    return (int)off;
}

static void sftp_eof(struct chan_ctx *c)
{
    chan_finish(c, 0);
}

static void sftp_poll(struct chan_ctx *c) { (void)c; }

static void sftp_stop(struct chan_ctx *c)
{
    sftp_session *sp = c->backend_data;
    if (sp) {
        if (*sp)
            sftp_server_free(*sp);
        free(sp);
    }
    c->backend_data = NULL;
    ssh_set_blocking(c->session, 0);
}

const struct backend_ops backend_sftp = {
    "sftp", sftp_start, sftp_data, sftp_eof, sftp_poll, sftp_stop
};

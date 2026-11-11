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
    int rc = sftp_channel_default_data_callback(c->session, c->channel, (void *)buf, (uint32_t)len, 0, sp);
    if (rc < 0) {
        /* Either the buffer does not yet hold a complete SFTP packet (a
         * 32 KB write spans two SSH packets) and libssh will call again
         * with more, or a real protocol error made libssh close the
         * channel.  Consume nothing; the pump notices a closed channel. */
        if (!ssh_channel_is_open(c->channel)) {
            c->finished = 1;
            c->closed = 1;
        }
        return 0;
    }
    return rc;
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

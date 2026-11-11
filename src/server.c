/* libssh server core: a single cooperative poll loop handling every
 * connection.  No fork, no threads: each connection is a session_ctx in a
 * list, driven by one ssh_event.
 */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <errno.h>
#include <poll.h>
#include <libssh/libssh.h>
#include <libssh/server.h>
#include <libssh/callbacks.h>

#include "server.h"
#include "backend.h"
#include "auth.h"
#include "config.h"
#include "log.h"

#define MAX_SESSIONS 8
#define MAX_AUTH_TRIES 6

enum sess_state { SESS_KEX, SESS_RUNNING, SESS_DEAD };

struct session_ctx {
    ssh_session session;
    enum sess_state state;
    int auth_tries;
    int authenticated;
    char user[64];
    struct chan_ctx *chan;      /* one channel per session for now */
    struct ssh_server_callbacks_struct server_cb;
    struct ssh_channel_callbacks_struct channel_cb;
    struct session_ctx *next;
};

static ssh_bind bind_handle;
static ssh_event event;
static struct session_ctx *sessions;
static int nsessions;

/* ---- channel output queue --------------------------------------------- */

int chan_output(struct chan_ctx *c, const char *data, size_t len)
{
    if (c->out_len + len > c->out_cap) {
        size_t ncap = c->out_cap ? c->out_cap * 2 : 4096;
        char *n;
        while (ncap < c->out_len + len)
            ncap *= 2;
        n = realloc(c->out, ncap);
        if (!n)
            return -1;
        c->out = n;
        c->out_cap = ncap;
    }
    memcpy(c->out + c->out_len, data, len);
    c->out_len += len;
    return 0;
}

int chan_output_str(struct chan_ctx *c, const char *s)
{
    return chan_output(c, s, strlen(s));
}

int chan_output_vdu(struct chan_ctx *c, const char *data, size_t len)
{
    size_t i, start = 0;
    for (i = 0; i < len; i++) {
        if (data[i] == '\r' || (data[i] == '\n' && c->pty)) {
            if (i > start && chan_output(c, data + start, i - start) < 0)
                return -1;
            if (data[i] == '\n' && chan_output(c, "\r\n", 2) < 0)
                return -1;
            start = i + 1;
        }
    }
    if (len > start)
        return chan_output(c, data + start, len - start);
    return 0;
}

void chan_finish(struct chan_ctx *c, int status)
{
    c->finished = 1;
    c->exit_status = status;
}

/* Send as much queued output as the channel window allows. */
static void chan_pump(struct chan_ctx *c)
{
    while (c->out_len > 0 && ssh_channel_is_open(c->channel)) {
        uint32_t window = ssh_channel_window_size(c->channel);
        size_t n = c->out_len;
        int rc;
        if (window == 0)
            return;
        if (n > window)
            n = window;
        if (n > 32768)
            n = 32768;
        rc = ssh_channel_write(c->channel, c->out, (uint32_t)n);
        if (rc == SSH_AGAIN || rc == 0)
            return;
        if (rc < 0) {
            log_debug("channel write failed: %s", ssh_get_error(c->session));
            c->out_len = 0;
            return;
        }
        memmove(c->out, c->out + rc, c->out_len - (size_t)rc);
        c->out_len -= (size_t)rc;
    }
    if (c->finished && c->out_len == 0 && !c->closed && ssh_channel_is_open(c->channel)) {
        c->closed = 1;
        ssh_channel_request_send_exit_status(c->channel, c->exit_status);
        ssh_channel_send_eof(c->channel);
        ssh_channel_close(c->channel);
    }
}

static void chan_free(struct chan_ctx *c)
{
    if (!c)
        return;
    if (c->backend && c->backend->stop)
        c->backend->stop(c);
    if (c->channel)
        ssh_channel_free(c->channel);
    free(c->out);
    free(c);
}

/* ---- channel callbacks -------------------------------------------------- */

static int cb_pty_request(ssh_session session, ssh_channel channel, const char *term,
                          int cols, int rows, int py, int px, void *userdata)
{
    struct chan_ctx *c = userdata;
    (void)session; (void)channel; (void)py; (void)px;
    c->pty = 1;
    c->cols = cols;
    c->rows = rows;
    snprintf(c->term, sizeof c->term, "%s", term ? term : "");
    return 0;
}

static int cb_pty_resize(ssh_session session, ssh_channel channel,
                         int cols, int rows, int py, int px, void *userdata)
{
    struct chan_ctx *c = userdata;
    (void)session; (void)channel; (void)py; (void)px;
    c->cols = cols;
    c->rows = rows;
    return 0;
}

static int start_backend(struct chan_ctx *c, const struct backend_ops *b, const char *command)
{
    if (c->backend) {
        log_msg("%s request on a channel that already has a %s session",
                b->name, c->backend->name);
        return -1;
    }
    c->backend = b;
    if (b->start(c, command) < 0) {
        c->backend = NULL;
        return -1;
    }
    return 0;
}

static int cb_exec_request(ssh_session session, ssh_channel channel,
                           const char *command, void *userdata)
{
    struct chan_ctx *c = userdata;
    (void)session; (void)channel;
    log_msg("%s: exec '%s'", c->owner->user, command);
    return start_backend(c, &backend_exec, command) < 0 ? 1 : 0;
}

static int cb_shell_request(ssh_session session, ssh_channel channel, void *userdata)
{
    struct chan_ctx *c = userdata;
    (void)session; (void)channel;
    log_msg("%s: shell%s", c->owner->user, c->pty ? " (pty)" : "");
    return start_backend(c, &backend_shell, NULL) < 0 ? 1 : 0;
}

static int cb_data(ssh_session session, ssh_channel channel, void *data,
                   uint32_t len, int is_stderr, void *userdata)
{
    struct chan_ctx *c = userdata;
    (void)session; (void)channel; (void)is_stderr;
    if (!c->backend || c->finished)
        return (int)len;
    return c->backend->data(c, data, len);
}

static void cb_eof(ssh_session session, ssh_channel channel, void *userdata)
{
    struct chan_ctx *c = userdata;
    (void)session; (void)channel;
    if (c->backend && !c->finished)
        c->backend->eof(c);
}

static void cb_close(ssh_session session, ssh_channel channel, void *userdata)
{
    struct chan_ctx *c = userdata;
    (void)session; (void)channel;
    c->closed = 1;
    c->finished = 1;
}

/* ---- server callbacks --------------------------------------------------- */

static int cb_auth_pubkey(ssh_session session, const char *user, struct ssh_key_struct *pubkey,
                          char signature_state, void *userdata)
{
    struct session_ctx *s = userdata;
    (void)session;

    if (signature_state == SSH_PUBLICKEY_STATE_NONE) {
        /* Probe: tell the client whether this key would be acceptable. */
        return auth_key_allowed(pubkey) ? SSH_AUTH_SUCCESS : SSH_AUTH_DENIED;
    }
    if (signature_state == SSH_PUBLICKEY_STATE_VALID && auth_key_allowed(pubkey)) {
        s->authenticated = 1;
        snprintf(s->user, sizeof s->user, "%s", user);
        log_msg("%s: public key accepted", user);
        return SSH_AUTH_SUCCESS;
    }
    if (++s->auth_tries >= MAX_AUTH_TRIES) {
        log_msg("%s: too many auth failures", user);
        ssh_disconnect(s->session);
    }
    return SSH_AUTH_DENIED;
}

static ssh_channel cb_channel_open(ssh_session session, void *userdata)
{
    struct session_ctx *s = userdata;
    struct chan_ctx *c;

    if (!s->authenticated || s->chan)
        return NULL;
    c = calloc(1, sizeof *c);
    if (!c)
        return NULL;
    c->session = session;
    c->owner = s;
    c->channel = ssh_channel_new(session);
    if (!c->channel) {
        free(c);
        return NULL;
    }
    s->chan = c;

    memset(&s->channel_cb, 0, sizeof s->channel_cb);
    s->channel_cb.userdata = c;
    s->channel_cb.channel_pty_request_function = cb_pty_request;
    s->channel_cb.channel_pty_window_change_function = cb_pty_resize;
    s->channel_cb.channel_exec_request_function = cb_exec_request;
    s->channel_cb.channel_shell_request_function = cb_shell_request;
    s->channel_cb.channel_data_function = cb_data;
    s->channel_cb.channel_eof_function = cb_eof;
    s->channel_cb.channel_close_function = cb_close;
    ssh_callbacks_init(&s->channel_cb);
    ssh_set_channel_callbacks(c->channel, &s->channel_cb);
    return c->channel;
}

/* ---- session lifecycle -------------------------------------------------- */

static void session_destroy(struct session_ctx *s)
{
    struct session_ctx **pp;
    for (pp = &sessions; *pp; pp = &(*pp)->next) {
        if (*pp == s) {
            *pp = s->next;
            break;
        }
    }
    if (s->state == SESS_RUNNING)
        ssh_event_remove_session(event, s->session);
    chan_free(s->chan);
    ssh_disconnect(s->session);
    ssh_free(s->session);
    free(s);
    nsessions--;
}

static void accept_connection(void)
{
    struct session_ctx *s;
    ssh_session session = ssh_new();

    if (!session)
        return;
    if (ssh_bind_accept(bind_handle, session) != SSH_OK) {
        log_debug("accept: %s", ssh_get_error(bind_handle));
        ssh_free(session);
        return;
    }
    if (nsessions >= MAX_SESSIONS) {
        log_msg("connection refused: too many sessions");
        ssh_disconnect(session);
        ssh_free(session);
        return;
    }
    s = calloc(1, sizeof *s);
    if (!s) {
        ssh_free(session);
        return;
    }
    s->session = session;
    s->state = SESS_KEX;
    s->server_cb.userdata = s;
    s->server_cb.auth_pubkey_function = cb_auth_pubkey;
    s->server_cb.channel_open_request_session_function = cb_channel_open;
    ssh_callbacks_init(&s->server_cb);
    ssh_set_server_callbacks(session, &s->server_cb);
    ssh_set_auth_methods(session, SSH_AUTH_METHOD_PUBLICKEY);
    ssh_set_blocking(session, 0);
    s->next = sessions;
    sessions = s;
    nsessions++;
    log_msg("connection from client (%d active)", nsessions);

    /* Kick off the banner exchange; completes over later polls. */
    ssh_handle_key_exchange(session);
}

static void listen_cb_noop(void) {}

static int cb_listen_fd(socket_t fd, int revents, void *userdata)
{
    (void)fd; (void)userdata;
    if (revents & POLLIN)
        accept_connection();
    return 0;
}

int server_init(ssh_key hostkey)
{
    int fd;

    (void)listen_cb_noop;
    if (ssh_init() < 0) {
        log_msg("ssh_init failed");
        return -1;
    }
    bind_handle = ssh_bind_new();
    event = ssh_event_new();
    if (!bind_handle || !event)
        return -1;
    ssh_bind_options_set(bind_handle, SSH_BIND_OPTIONS_BINDADDR, cfg.bind_addr);
    ssh_bind_options_set(bind_handle, SSH_BIND_OPTIONS_BINDPORT, &cfg.port);
    ssh_bind_options_set(bind_handle, SSH_BIND_OPTIONS_IMPORT_KEY, hostkey);
    if (log_verbose > 1) {
        int lvl = SSH_LOG_PROTOCOL;
        ssh_bind_options_set(bind_handle, SSH_BIND_OPTIONS_LOG_VERBOSITY, &lvl);
    }
    if (ssh_bind_listen(bind_handle) < 0) {
        log_msg("listen on %s:%d failed: %s", cfg.bind_addr, cfg.port, ssh_get_error(bind_handle));
        return -1;
    }
    ssh_bind_set_blocking(bind_handle, 0);
    fd = ssh_bind_get_fd(bind_handle);
    if (ssh_event_add_fd(event, fd, POLLIN, cb_listen_fd, NULL) != SSH_OK) {
        log_msg("cannot watch listening socket");
        return -1;
    }
    log_msg("listening on %s:%d", cfg.bind_addr, cfg.port);
    return 0;
}

int server_poll(int timeout_ms)
{
    struct session_ctx *s, *next;
    int rc;

    /* Drive key exchange for new sessions outside the event: once it
     * completes they join the event and callbacks take over. */
    for (s = sessions; s; s = next) {
        next = s->next;
        if (s->state != SESS_KEX)
            continue;
        rc = ssh_handle_key_exchange(s->session);
        if (rc == SSH_OK) {
            s->state = SESS_RUNNING;
            ssh_event_add_session(event, s->session);
            log_debug("key exchange done");
        } else if (rc != SSH_AGAIN) {
            log_msg("key exchange failed: %s", ssh_get_error(s->session));
            session_destroy(s);
        }
    }

    rc = ssh_event_dopoll(event, timeout_ms);
    if (rc == SSH_ERROR)
        log_debug("event poll error");

    for (s = sessions; s; s = next) {
        next = s->next;
        if (s->state != SESS_RUNNING)
            continue;
        if (!ssh_is_connected(s->session)) {
            log_msg("%s: disconnected", s->user[0] ? s->user : "client");
            session_destroy(s);
            continue;
        }
        if (s->chan) {
            struct chan_ctx *c = s->chan;
            if (c->backend && !c->finished)
                c->backend->poll(c);
            chan_pump(c);
            if (c->closed && c->out_len == 0 && !ssh_channel_is_open(c->channel)) {
                chan_free(c);
                s->chan = NULL;
                /* Single-channel policy: the session ends with its channel. */
                log_msg("%s: session closed", s->user);
                session_destroy(s);
            }
        }
    }
    return nsessions;
}

void server_shutdown(void)
{
    while (sessions)
        session_destroy(sessions);
    if (event)
        ssh_event_free(event);
    if (bind_handle)
        ssh_bind_free(bind_handle);
    ssh_finalize();
}

/* SPDX-License-Identifier: MIT */
/* libssh server core: a single cooperative poll loop handling every
 * connection.  No fork, no threads: each connection is a session_ctx in a
 * list, driven by one ssh_event.
 */
#include <stdbool.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <poll.h>
#include <time.h>
#include <sys/socket.h>
#include <netinet/in.h>
#include <arpa/inet.h>
#include <kernel.h>
#include <swis.h>
#include <libssh/libssh.h>
#include <libssh/server.h>
#include <libssh/callbacks.h>

#include "server.h"
#include "backend.h"
#include "backend_taskwindow.h"
#include "auth.h"
#include "config.h"
#include "keyfp.h"
#include "log.h"
#include "prof.h"
#include "riscos.h"

#define MAX_AUTH_TRIES 6
#define PENDING_PER_ADDRESS 2   /* unauthenticated connections one address may hold */
/* Each new connection costs a key exchange (bignum work in the cooperative
 * loop) before it can even try to log in, so one address may open at most
 * RATE_MAX connections in any RATE_SECONDS. */
#define RATE_MAX 10
#define RATE_SECONDS 10
#define RATE_SLOTS 64
/* libssh reads at most 4 KB per socket per poll, so one poll per desktop
 * null event would cap uploads near 200 KB/s.  Each server_poll therefore
 * polls again while there is work, within this budget. */
#define POLL_ROUNDS_MAX 32
#define POLL_BUDGET_CS  3

enum sess_state { SESS_KEX, SESS_RUNNING };

struct session_ctx {
    ssh_session session;
    enum sess_state state;
    int auth_tries;
    int authenticated;
    int banner_sent;
    int drop;                   /* close from the main loop, not from a callback */
    char refusal[160];          /* why the last login attempt was refused */
    unsigned long addr;         /* peer IPv4 address, 0 if unknown */
    char user[64];
    char peer[48];              /* "10.10.10.2": the subject of log lines */
    char who[112];              /* "10.10.10.2 riscos" once logged in */
    struct chan_ctx *chan;      /* one channel per session for now */
    struct chan_ctx *retired;   /* replaced channels, freed from the main loop */
    time_t deadline;            /* drop the session if not authenticated, or
                                   with no channel, by this time; 0 = none */
    struct ssh_server_callbacks_struct server_cb;
    struct session_ctx *next;
};

static ssh_bind bind_handle;
static ssh_event event;
static struct session_ctx *sessions;
static int nsessions;

/* cfg.auth_timeout: seconds from accept to a successful login;
 * cfg.idle_timeout: seconds a logged-in session may sit without a channel;
 * 0 means no limit. */
static time_t deadline_in(int seconds)
{
    return seconds > 0 ? time(NULL) + seconds : 0;
}

/* ---- channel output ----------------------------------------------------- */

int chan_output(struct chan_ctx *c, const char *data, size_t len)
{
    return buf_append(&c->out, data, len);
}

int chan_output_str(struct chan_ctx *c, const char *s)
{
    return chan_output(c, s, strlen(s));
}

int chan_output_err(struct chan_ctx *c, const char *s)
{
    return buf_append(&c->err, s, strlen(s));
}

/* Text with RISC OS line ends, which are LF CR: the CR after a LF is
 * dropped, and in pty mode each LF becomes CRLF while any other CR (a
 * line redrawing itself) passes through; in pipe mode every CR goes. */
static int output_lines(struct chan_ctx *c, const char *data, size_t len)
{
    size_t i, start = 0;
    for (i = 0; i < len; i++) {
        char ch = data[i];
        int after_lf = c->vdu_after_lf;
        c->vdu_after_lf = ch == '\n';
        if (ch == '\r' && c->pty && !after_lf)
            continue;                    /* a lone CR: pass it on */
        if (ch == '\r' || (ch == '\n' && c->pty)) {
            if (i > start && chan_output(c, data + start, i - start) < 0)
                return -1;
            if (ch == '\n' && chan_output(c, "\r\n", 2) < 0)
                return -1;
            start = i + 1;
        }
    }
    if (len > start)
        return chan_output(c, data + start, len - start);
    return 0;
}

/* Parameter bytes that follow each VDU code 0-31. */
static const unsigned char vdu_params[32] = {
    0, 1, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0,
    0, 1, 2, 5, 0, 0, 1, 9, 8, 5, 0, 0, 4, 4, 0, 2
};

/* A complete VDU control sequence, for a pty: the ones a terminal can
 * show become ANSI sequences, the rest are dropped. */
static void vdu_control(struct chan_ctx *c, const unsigned char *v)
{
    char esc[24];
    switch (v[0]) {
    case 7:  chan_output_str(c, "\a"); break;                    /* bell */
    case 8:  chan_output_str(c, "\b"); break;                    /* cursor left */
    case 9:  chan_output_str(c, "\033[C"); break;                /* cursor right */
    case 11: chan_output_str(c, "\033[A"); break;                /* cursor up */
    case 12: case 22:                                            /* CLS, MODE */
             chan_output_str(c, "\033[H\033[2J"); break;
    case 17:                                                     /* COLOUR c */
        if (v[1] < 128)
            snprintf(esc, sizeof esc, "\033[%dm", (v[1] & 15) < 8 ? 30 + (v[1] & 7) : 90 + (v[1] & 7));
        else
            snprintf(esc, sizeof esc, "\033[%dm", 40 + (v[1] & 7));
        chan_output_str(c, esc);
        break;
    case 20: chan_output_str(c, "\033[0m"); break;               /* default colours */
    case 30: chan_output_str(c, "\033[H"); break;                /* home */
    case 31:                                                     /* TAB(x, y) */
        snprintf(esc, sizeof esc, "\033[%d;%dH", v[2] + 1, v[1] + 1);
        chan_output_str(c, esc);
        break;
    default: break;
    }
}

/* Text, LF and CR go through output_lines; control codes and their
 * parameter bytes, which would show as junk, are translated for a pty and
 * dropped for a pipe; ReadLine's echo of a deleted character (DEL)
 * becomes backspace-space-backspace.  A sequence may be split across
 * calls. */
int chan_output_vdu(struct chan_ctx *c, const char *data, size_t len)
{
    size_t start = 0;
    for (size_t i = 0; i < len; i++) {
        unsigned char ch = (unsigned char)data[i];
        if (c->vdu_need) {               /* a parameter byte */
            c->vdu[c->vdu_have++] = ch;
            if (--c->vdu_need == 0 && c->pty)
                vdu_control(c, c->vdu);
            start = i + 1;
            continue;
        }
        if (ch >= 32 && ch != 127)
            continue;                    /* text: sent in one piece below */
        if (ch == 10 || ch == 13 || (ch == 9 && !c->pty))
            continue;                    /* line ends (and a tab in a pipe) */
        if (output_lines(c, data + start, i - start) < 0)
            return -1;
        start = i + 1;
        if (ch == 127) {
            if (c->pty)
                chan_output_str(c, "\b \b");
        } else {
            c->vdu[0] = ch;
            c->vdu_have = 1;
            c->vdu_need = vdu_params[ch];
            if (c->vdu_need == 0 && c->pty)
                vdu_control(c, c->vdu);
        }
    }
    return output_lines(c, data + start, len - start);
}

void chan_finish(struct chan_ctx *c, int status)
{
    c->finished = 1;
    c->exit_status = status;
}

void chan_refuse(struct chan_ctx *c, const char *msg)
{
    chan_output_err(c, msg);
    chan_output_err(c, "\r\n");
    chan_finish(c, 1);
}

/* Send as much of one queue as the channel window allows.  Returns 1 if
 * it is drained. */
static int pump_queue(struct chan_ctx *c, struct buf *q, int is_stderr)
{
    /* libssh may run a nested poll inside a write (to finish a rekey),
     * whose callbacks can append to this queue and move its bytes; so the
     * chunk is copied out first. */
    static char chunk[32768];
    while (q->len > 0 && ssh_channel_is_open(c->channel)) {
        uint32_t window = ssh_channel_window_size(c->channel);
        size_t n = q->len;
        int rc;
        if (window == 0)
            return 0;
        if (n > window)
            n = window;
        if (n > sizeof chunk)
            n = sizeof chunk;
        memcpy(chunk, buf_data(q), n);
        rc = is_stderr ? ssh_channel_write_stderr(c->channel, chunk, (uint32_t)n)
                       : ssh_channel_write(c->channel, chunk, (uint32_t)n);
        if (rc == SSH_AGAIN || rc == 0)
            return 0;
        if (rc < 0) {
            log_debug("channel write failed: %s", ssh_get_error(c->session));
            buf_consume(q, q->len);
            return 1;
        }
        buf_consume(q, (size_t)rc);
    }
    return q->len == 0;
}

/* Send queued output; once a finished session's output is out, send the
 * exit status, EOF and close.  After the close the channel is no longer
 * open, which is what lets server_poll free it. */
static void chan_pump(struct chan_ctx *c)
{
    PROF_START(t);
    int drained = pump_queue(c, &c->out, 0);
    drained &= pump_queue(c, &c->err, 1);
    PROF_STOP(PROF_PUMP, t);
    if (c->finished && drained && ssh_channel_is_open(c->channel)) {
        ssh_channel_request_send_exit_status(c->channel, c->exit_status);
        ssh_channel_send_eof(c->channel);
        ssh_channel_close(c->channel);
    }
}

/* Free a channel.  The caller must have unlinked it first: closing it in
 * libssh flushes, and a flush can run a nested poll whose callbacks must
 * not find it again.  Those callbacks see a channel with no backend.  The
 * chan_ctx goes last, as libssh points at its callback struct until
 * ssh_channel_free. */
static void chan_free(struct chan_ctx *c)
{
    if (!c)
        return;
    if (c->backend && c->backend->stop)
        c->backend->stop(c);
    c->backend = NULL;
    c->finished = 1;
    if (c->channel)
        ssh_channel_free(c->channel);
    buf_free(&c->out);
    buf_free(&c->err);
    free(c);
}

/* ---- channel callbacks -------------------------------------------------- */

static int cb_pty_request(ssh_session session, ssh_channel channel, const char *term,
                          int cols, int rows, int py, int px, void *userdata)
{
    struct chan_ctx *c = userdata;
    (void)session; (void)channel; (void)term; (void)cols; (void)rows; (void)py; (void)px;
    c->pty = 1;   /* the terminal size has no RISC OS counterpart yet */
    return 0;
}

/* A second request on a channel that already runs a session, or whose
 * session has finished (a refusal, say), is refused without touching it. */
static int channel_busy(struct chan_ctx *c, const char *what)
{
    if (!c->backend && !c->finished)
        return 0;
    log_msg("%s: %s request on a channel that is %s", c->owner_name, what,
            c->backend ? "already in use" : "finished");
    return 1;
}

/* Start backend b; returns its start() result (0, BACKEND_UNAVAILABLE or
 * BACKEND_REFUSED). */
static int start_backend(struct chan_ctx *c, const struct backend_ops *b, const char *command)
{
    int rc;
    c->backend = b;
    rc = b->start(c, command);
    if (rc != 0)
        c->backend = NULL;
    return rc;
}

/* Exec and shell run in a TaskWindow child on the desktop, so output streams
 * as it is produced and a hung command cannot take the server down.  In
 * text mode the TaskWindow backend is unavailable and the in-process
 * OS_CLI backends serve instead; on the desktop they are never used. */
static int start_command_backend(struct chan_ctx *c, const struct backend_ops *fallback,
                                 const char *command)
{
    int rc = start_backend(c, &backend_taskwindow, command);
    if (rc == BACKEND_UNAVAILABLE)
        rc = start_backend(c, fallback, command);
    return rc == BACKEND_UNAVAILABLE ? 1 : 0;   /* a refusal is answered on stderr */
}

static int cb_exec_request(ssh_session session, ssh_channel channel,
                           const char *command, void *userdata)
{
    struct chan_ctx *c = userdata;
    (void)session; (void)channel;
    log_msg("%s: exec '%s'", c->owner_name, command);
    if (channel_busy(c, "exec"))
        return 1;
    if (!cfg.allow_exec) {
        chan_refuse(c, "Command execution is disabled on this server");
        return 0;
    }
    return start_command_backend(c, &backend_exec, command);
}

static int cb_shell_request(ssh_session session, ssh_channel channel, void *userdata)
{
    struct chan_ctx *c = userdata;
    (void)session; (void)channel;
    log_msg("%s: shell%s", c->owner_name, c->pty ? " (pty)" : "");
    if (channel_busy(c, "shell"))
        return 1;
    if (!cfg.allow_shell) {
        chan_refuse(c, "Interactive sessions are disabled on this server");
        return 0;
    }
    if (cfg.motd)
        chan_output_str(c, cfg.motd);
    return start_command_backend(c, &backend_shell, NULL);
}

static int cb_subsystem_request(ssh_session session, ssh_channel channel,
                                const char *subsystem, void *userdata)
{
    struct chan_ctx *c = userdata;
    (void)session; (void)channel;
    if (channel_busy(c, "subsystem"))
        return 1;
    if (strcmp(subsystem, "sftp") != 0) {
        log_msg("%s: subsystem '%s' refused", c->owner_name, subsystem);
        return 1;
    }
    if (!cfg.allow_sftp) {
        log_msg("%s: sftp refused (the sftp setting is off)", c->owner_name);
        chan_refuse(c, "SFTP is turned off on this server (the sftp setting)");
        return 0;
    }
    log_msg("%s: sftp", c->owner_name);
    return start_backend(c, &backend_sftp, NULL) == 0 ? 0 : 1;
}

static int cb_data(ssh_session session, ssh_channel channel, void *data,
                   uint32_t len, int is_stderr, void *userdata)
{
    struct chan_ctx *c = userdata;
    PROF_START(t);
    (void)session; (void)channel; (void)is_stderr;
    PROF_BYTES(len);
    if (c->backend && !c->finished && c->backend->data) {
        c->backend->data(c, data, len);
        PROF_STOP(PROF_CBDATA, t);
    }
    return (int)len;            /* always consumed, so libssh keeps nothing back */
}

static void cb_eof(ssh_session session, ssh_channel channel, void *userdata)
{
    struct chan_ctx *c = userdata;
    (void)session; (void)channel;
    if (c->backend && !c->finished && c->backend->eof)
        c->backend->eof(c);
}

static void cb_close(ssh_session session, ssh_channel channel, void *userdata)
{
    struct chan_ctx *c = userdata;
    (void)session; (void)channel;
    c->finished = 1;            /* the channel is closed: freed on the next pass */
}

static const struct ssh_channel_callbacks_struct channel_cb_template = {
    .channel_pty_request_function = cb_pty_request,
    .channel_exec_request_function = cb_exec_request,
    .channel_shell_request_function = cb_shell_request,
    .channel_subsystem_request_function = cb_subsystem_request,
    .channel_data_function = cb_data,
    .channel_eof_function = cb_eof,
    .channel_close_function = cb_close,
};

/* ---- server callbacks --------------------------------------------------- */

/* SSH_MSG_USERAUTH_BANNER, sent once from the first authentication
 * request: RFC 4252 places it inside the authentication protocol, and
 * OpenSSH rejects it before its SERVICE_ACCEPT. */
static void send_banner(struct session_ctx *s)
{
    ssh_string b;
    if (!cfg.banner || s->banner_sent)
        return;
    s->banner_sent = 1;
    b = ssh_string_from_char(cfg.banner);
    if (b) {
        ssh_send_issue_banner(s->session, b);
        ssh_string_free(b);
    }
}

static int cb_auth_pubkey(ssh_session session, const char *user, struct ssh_key_struct *pubkey,
                          char signature_state, void *userdata)
{
    struct session_ctx *s = userdata;
    (void)session;

    send_banner(s);
    /* OpenSSH first asks whether a key would do and signs only after a
     * yes, so refusals mostly happen at the question; the reason is kept
     * and logged once when the connection ends without a login. */
    if (cfg.user[0] && strcmp(user, cfg.user) != 0) {
        char safe[64];
        snprintf(safe, sizeof safe, "%s", user);
        log_sanitise(safe);
        snprintf(s->refusal, sizeof s->refusal, "user name '%s' (log in as %s@...)", safe, cfg.user);
        /* counted like any other failure, so a wrong name and a wrong key
         * behave alike */
    } else if (signature_state == SSH_PUBLICKEY_STATE_NONE) {
        /* Probe: tell the client whether this key would be acceptable.
         * Counted like a failed attempt when refused, so probing is bounded. */
        char fp[128];
        if (auth_key_allowed(pubkey))
            return SSH_AUTH_SUCCESS;
        key_fingerprint(pubkey, fp, sizeof fp);
        snprintf(s->refusal, sizeof s->refusal, "key %s is not authorised", fp);
    } else if (signature_state != SSH_PUBLICKEY_STATE_VALID) {
        snprintf(s->refusal, sizeof s->refusal, "invalid signature");
    } else if (auth_key_allowed(pubkey)) {
        char fp[128];
        s->authenticated = 1;
        s->deadline = deadline_in(cfg.idle_timeout);
        snprintf(s->user, sizeof s->user, "%s", user);
        snprintf(s->who, sizeof s->who, "%s %s", s->peer, s->user);
        key_fingerprint(pubkey, fp, sizeof fp);
        log_msg("%s: logged in with key %s", s->who, fp);
        return SSH_AUTH_SUCCESS;
    }
    if (++s->auth_tries >= MAX_AUTH_TRIES) {
        log_msg("%s: too many failed logins", s->peer);
        ssh_session_set_disconnect_message(s->session, "Too many failed login attempts");
        /* not ssh_disconnect here: it would free libssh's crypto state
         * while its packet handler, our caller, is still running */
        s->drop = 1;
    }
    return SSH_AUTH_DENIED;
}

static ssh_channel cb_channel_open(ssh_session session, void *userdata)
{
    struct session_ctx *s = userdata;
    struct chan_ctx *c;

    if (!s->authenticated)
        return NULL;
    if (s->chan) {
        /* One channel at a time; a closed one may be replaced at once, as
         * multiplexing clients close and open in the same burst.  This
         * callback can run inside a flush of the old channel, so it is
         * not freed here but parked for the main loop. */
        if (ssh_channel_is_open(s->chan->channel))
            return NULL;
        s->chan->retired_next = s->retired;
        s->retired = s->chan;
        s->chan = NULL;
    }
    c = calloc(1, sizeof *c);
    if (!c)
        return NULL;
    c->session = session;
    c->owner_name = s->who;
    c->channel = ssh_channel_new(session);
    if (!c->channel) {
        free(c);
        return NULL;
    }
    s->chan = c;
    s->deadline = 0;

    /* Each channel gets its own copy: a retired channel can still receive
     * the client's late data and CHANNEL_CLOSE, which must reach it and
     * not its replacement. */
    c->cb = channel_cb_template;
    c->cb.userdata = c;
    ssh_callbacks_init(&c->cb);
    ssh_set_channel_callbacks(c->channel, &c->cb);
    return c->channel;
}

/* ---- session lifecycle -------------------------------------------------- */

static void free_retired(struct session_ctx *s)
{
    while (s->retired) {
        struct chan_ctx *c = s->retired;
        s->retired = c->retired_next;
        chan_free(c);
    }
}

static void session_destroy(struct session_ctx *s)
{
    struct session_ctx **pp;
    struct chan_ctx *c = s->chan;
    if (!s->authenticated && s->refusal[0])
        log_msg("%s: login refused: %s", s->peer, s->refusal);
    for (pp = &sessions; *pp; pp = &(*pp)->next) {
        if (*pp == s) {
            *pp = s->next;
            break;
        }
    }
    if (s->state == SESS_RUNNING)
        ssh_event_remove_session(event, s->session);
    s->chan = NULL;
    chan_free(c);
    free_retired(s);
    ssh_disconnect(s->session);
    ssh_free(s->session);
    free(s);
    nsessions--;
}

/* Recent accepts, oldest overwritten first. */
static struct { unsigned long addr; time_t when; } recent[RATE_SLOTS];
static int recent_next;

static int too_frequent(unsigned long addr)
{
    time_t now = time(NULL);
    int n = 0;
    for (int i = 0; i < RATE_SLOTS; i++)
        if (recent[i].when && recent[i].addr == addr && now - recent[i].when < RATE_SECONDS)
            n++;
    if (n >= RATE_MAX)
        return 1;
    recent[recent_next].addr = addr;
    recent[recent_next].when = now;
    recent_next = (recent_next + 1) % RATE_SLOTS;
    return 0;
}

static void accept_connection(void)
{
    struct session_ctx *s, *o;
    ssh_session session = ssh_new();
    struct sockaddr_in sa;
    socklen_t sl = sizeof sa;
    unsigned long addr = 0;
    char peer[48];
    unsigned port = 0;
    int pending = 0;

    if (!session)
        return;
    if (ssh_bind_accept(bind_handle, session) != SSH_OK) {
        log_debug("accept: %s", ssh_get_error(bind_handle));
        ssh_free(session);
        return;
    }
    if (getpeername(ssh_get_fd(session), (struct sockaddr *)&sa, &sl) == 0) {
        addr = (unsigned long)sa.sin_addr.s_addr;
        snprintf(peer, sizeof peer, "%s", inet_ntoa(sa.sin_addr));
        port = ntohs(sa.sin_port);
    } else {
        snprintf(peer, sizeof peer, "unknown address");
    }
    /* Refusals close the socket without a DISCONNECT packet: before the
     * version exchange a binary packet only confuses the client. */
    if (too_frequent(addr)) {
        log_debug("%s: refused, over %d connections in %d s", peer, RATE_MAX, RATE_SECONDS);
        ssh_free(session);
        return;
    }
    if (nsessions >= cfg.max_sessions) {
        log_msg("%s: refused, %d connections already", peer, nsessions);
        ssh_free(session);
        return;
    }
    for (o = sessions; o; o = o->next)
        if (!o->authenticated && o->addr == addr)
            pending++;
    if (pending >= PENDING_PER_ADDRESS) {
        log_msg("%s: refused, %d connections from there not yet logged in", peer, pending);
        ssh_free(session);
        return;
    }
    s = calloc(1, sizeof *s);
    if (!s) {
        ssh_free(session);
        return;
    }
    s->addr = addr;
    snprintf(s->peer, sizeof s->peer, "%s", peer);
    snprintf(s->who, sizeof s->who, "%s", peer);
    s->session = session;
    s->state = SESS_KEX;
    s->deadline = deadline_in(cfg.auth_timeout);
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
    log_msg("%s: connected from port %u (%d active)", s->peer, port, nsessions);

    /* Start the version exchange; it and the key exchange complete over
     * later polls. */
    ssh_handle_key_exchange(session);
}

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

    bind_handle = ssh_bind_new();
    event = ssh_event_new();
    if (!bind_handle || !event)
        return -1;
    {
        /* No libssh_server_config: libssh would read it from /etc/ssh on
         * the current disc, which is the SFTP root, so any client could
         * change the server's algorithms or host key for the next start. */
        bool no = false;
        ssh_bind_options_set(bind_handle, SSH_BIND_OPTIONS_PROCESS_CONFIG, &no);
    }
    ssh_bind_options_set(bind_handle, SSH_BIND_OPTIONS_BINDADDR, cfg.bind_addr);
    ssh_bind_options_set(bind_handle, SSH_BIND_OPTIONS_BINDPORT, &cfg.port);
    ssh_bind_options_set(bind_handle, SSH_BIND_OPTIONS_IMPORT_KEY, hostkey);
    /* Key exchange: the elliptic methods and one 2048-bit group.  The
     * default list also offers 8192-bit groups, each a C bignum
     * exponentiation on the ARM inside the cooperative loop that any
     * unauthenticated client could request in a loop. */
    ssh_bind_options_set(bind_handle, SSH_BIND_OPTIONS_KEY_EXCHANGE,
                         "curve25519-sha256,curve25519-sha256@libssh.org,ecdh-sha2-nistp256,"
                         "diffie-hellman-group14-sha256");
    /* Login signatures: no SHA-1 (ssh-rsa); RSA keys sign with SHA-2. */
    ssh_bind_options_set(bind_handle, SSH_BIND_OPTIONS_PUBKEY_ACCEPTED_KEY_TYPES,
                         "ssh-ed25519,ecdsa-sha2-nistp256,ecdsa-sha2-nistp384,ecdsa-sha2-nistp521,"
                         "rsa-sha2-512,rsa-sha2-256");
    if (ssh_bind_listen(bind_handle) < 0) {
        log_msg("listen on %s:%d failed: %s", cfg.bind_addr, cfg.port, ssh_get_error(bind_handle));
        goto fail;
    }
    ssh_bind_set_blocking(bind_handle, 0);
    fd = ssh_bind_get_fd(bind_handle);
    if (ssh_event_add_fd(event, fd, POLLIN, cb_listen_fd, NULL) != SSH_OK) {
        log_msg("cannot watch listening socket");
        goto fail;
    }
    log_msg("listening on %s:%d", cfg.bind_addr, cfg.port);
    return 0;
fail:
    /* no event left behind: server_poll then does nothing in the desktop
     * loop that shows why the start failed, instead of failing each poll */
    ssh_event_free(event);
    event = NULL;
    ssh_bind_free(bind_handle);
    bind_handle = NULL;
    return -1;
}

static int past_deadline(struct session_ctx *s, time_t now)
{
    if (s->deadline && now >= s->deadline) {
        char why[64];
        if (s->authenticated)
            snprintf(why, sizeof why, "No session opened for %d seconds", cfg.idle_timeout);
        else
            snprintf(why, sizeof why, "No login within %d seconds", cfg.auth_timeout);
        log_msg("%s: timed out %s", s->who, s->authenticated ? "with no session open" : "before logging in");
        /* clients print this instead of libssh's default "Bye Bye" */
        ssh_session_set_disconnect_message(s->session, why);
        session_destroy(s);
        return 1;
    }
    return 0;
}

/* Drive key exchange for new sessions outside the event: once it completes
 * they join the event and callbacks take over.  Returns 1 while any
 * session is still in key exchange. */
static int drive_key_exchanges(time_t now)
{
    struct session_ctx *s, *next;
    int pending = 0;
    for (s = sessions; s; s = next) {
        int rc;
        next = s->next;
        if (s->state != SESS_KEX || past_deadline(s, now))
            continue;
        rc = ssh_handle_key_exchange(s->session);
        if (rc == SSH_OK) {
            s->state = SESS_RUNNING;
            ssh_event_add_session(event, s->session);
            log_debug("%s: key exchange done", s->peer);
        } else if (rc != SSH_AGAIN) {
            log_msg("%s: key exchange failed: %s", s->peer, ssh_get_error(s->session));
            session_destroy(s);
        } else {
            pending = 1;
        }
    }
    return pending;
}

/* Run backends, send output, free closed channels, drop dead sessions. */
static void service_sessions(time_t now)
{
    struct session_ctx *s, *next;
    for (s = sessions; s; s = next) {
        struct chan_ctx *c;
        next = s->next;
        if (s->state != SESS_RUNNING || past_deadline(s, now))
            continue;
        if (s->drop || !ssh_is_connected(s->session)) {
            if (!s->drop)
                log_msg("%s: disconnected%s", s->who, s->authenticated ? "" : " before logging in");
            session_destroy(s);
            continue;
        }
        free_retired(s);                /* nothing on the stack refers to them */
        c = s->chan;
        if (!c)
            continue;
        /* Both calls below can run a nested poll (libssh flushes on write
         * and close), in which the client may replace the channel: then c
         * is retired and must not be touched again here. */
        if (c->backend && !c->finished && c->backend->poll)
            c->backend->poll(c);
        if (s->chan != c)
            continue;
        chan_pump(c);
        if (s->chan != c)
            continue;
        if (!ssh_channel_is_open(c->channel)) {
            s->chan = NULL;
            chan_free(c);
            /* The connection stays for another channel (OpenSSH
             * multiplexing) until the idle deadline or the client's own
             * disconnect. */
            log_debug("%s: channel closed", s->who);
            s->deadline = deadline_in(cfg.idle_timeout);
        }
    }
}

int server_poll(int timeout_ms)
{
    int rounds = 0, until = 0;
    time_t now = time(NULL);
    PROF_START(t_all);

    if (!event)
        return 0;               /* startup failed; the desktop task lives on to show why */

    if (drive_key_exchanges(now))
        timeout_ms = 0;         /* those sessions are driven outside the event */
    for (;;) {
        PROF_START(t);
        int rc = ssh_event_dopoll(event, timeout_ms);
        PROF_STOP(PROF_DOPOLL, t);
        PROF_ROUND(rc >= 0);
        if (rc == SSH_ERROR)
            log_debug("event poll error");
        service_sessions(time(NULL));
        if (rounds == 0)        /* the budget starts after a first poll that may wait */
            until = ro_now_cs() + POLL_BUDGET_CS;
        /* 0 or more: sockets were serviced (libssh may return a leftover
         * count rather than SSH_OK); SSH_AGAIN is the timeout, SSH_ERROR < 0 */
        if (rc < 0 || ++rounds >= POLL_ROUNDS_MAX || ro_cs_since(until, ro_now_cs()) >= 0)
            break;
        timeout_ms = 0;
    }
    PROF_STOP(PROF_POLL, t_all);
    PROF_REPORT();
    return nsessions;
}

int server_logged_in(void)
{
    int n = 0;
    for (struct session_ctx *s = sessions; s; s = s->next)
        n += s->authenticated;
    return n;
}

void server_shutdown(void)
{
    while (sessions) {
        ssh_session_set_disconnect_message(sessions->session, "SSHd is shutting down");
        session_destroy(sessions);
    }
    if (event)
        ssh_event_free(event);
    if (bind_handle)
        ssh_bind_free(bind_handle);
}

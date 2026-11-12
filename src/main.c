/* sshd for RISC OS: entry point.
 *
 * By default this is a Wimp task with an icon-bar icon (src/wimp.c); the
 * server loop body, server_poll(), runs on null events.  With -t it is a
 * plain command-line program for a TaskWindow, handy for debugging.
 *
 * Settings come from the Options file in the configuration directory (see
 * options_template below); command line options override it.
 */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdarg.h>
#include <signal.h>
#include <unistd.h>
#include <unixlib/local.h>
#include <kernel.h>
#include <swis.h>
#include <libssh/libssh.h>
#include <libssh/callbacks.h>

#include "config.h"
#include "server.h"
#include "auth.h"
#include "hostkey.h"
#include "riscos.h"
#include "riscos_sftp.h"
#include "riscos_entropy.h"
#include "wimp.h"
#include "log.h"

struct sshd_config cfg = {
    .port = 22, .bind_addr = "0.0.0.0",
    .auth_timeout = 60, .idle_timeout = 120, .max_sessions = DEFAULT_MAX_SESSIONS,
    .allow_shell = 1, .allow_exec = 1, .allow_sftp = 1,
    .user = "riscos",
    .default_filetype = DEFAULT_FILETYPE, .log_size_kb = 256,
};

int crypto_ready;

/* Settings that live outside cfg. */
static const char *root_arg = "<Boot$Dir>.^";
static int logfile = 1, banner = 1, motd = 1;

/* UnixLib path translation, read from this application-defined global:
 * no c/h/s suffix directories, and the HostFS filetype convention
 * (",xxx" suffixes on names, MimeMap for extensions), see riscos_sftp.h. */
int __riscosify_control = __RISCOSIFY_NO_SUFFIX | __RISCOSIFY_FILETYPE_EXT;

static volatile int running = 1;      /* text mode loop */
static int text_mode;

void connect_hint(char *buf, size_t len)
{
    char addr[48], port[16] = "";
    if (ro_local_address(addr, sizeof addr) < 0) {
        buf[0] = '\0';
        return;
    }
    if (cfg.port != 22)
        snprintf(port, sizeof port, " -p %d", cfg.port);
    snprintf(buf, len, "ssh%s %s@%s", port, cfg.user[0] ? cfg.user : "user", addr);
}

static void on_term(int sig)
{
    (void)sig;
    running = 0;
}

static void on_quit(void)
{
    running = 0;
}

static char *join(const char *dir, const char *leaf)
{
    size_t n = strlen(dir) + strlen(leaf) + 2;
    char *p = malloc(n);
    if (p)
        snprintf(p, n, "%s.%s", dir, leaf);
    return p;
}

static void usage(void)
{
    fprintf(stderr,
            "Usage: sshd [-p port] [-b address] [-r sftp-dir] [-d config-dir] [-t] [-v]\n"
            "  -p port        TCP port to listen on (default 22)\n"
            "  -b address     address to listen on (default 0.0.0.0, all interfaces)\n"
            "  -r dir         directory SFTP clients start in (default <Boot$Dir>.^, the boot disc)\n"
            "  -d dir         configuration directory (default <Choices$Write>.SSHd)\n"
            "  -t             text mode: no Wimp, log to this window (run in a TaskWindow)\n"
            "  -v             verbose; -vv also logs the SSH protocol\n"
            "The configuration directory holds Options (settings, read before the\n"
            "command line), hostkey (generated on first run), authkeys (one public\n"
            "key per line, OpenSSH format without options), the log file, and the\n"
            "optional Banner and Motd texts.\n");
}

/* Fatal startup error.  In text mode: message and exit.  On the desktop:
 * log it, open the log window and keep the icon on the bar so the reason
 * can be read and the task quit, instead of a modal box that would stall
 * a boot sequence running !SSHd from Tasks. */
static int fatal(const char *fmt, ...)
{
    char msg[300];
    va_list ap;
    va_start(ap, fmt);
    vsnprintf(msg, sizeof msg, fmt, ap);
    va_end(ap);
    log_msg("startup failed: %s", msg);
    if (!text_mode && wimp_init() == 0) {
        log_msg("fix the cause, then Quit from the icon bar menu and run !SSHd again");
        wimp_open_log_window();
        wimp_run();
        wimp_shutdown();
    }
    return 1;
}

/* Written to <config dir>.Options on first run, so the settings and their
 * defaults are there to edit.  Every setting is commented out: the
 * defaults apply until one is uncommented. */
static const char options_template[] =
    "# SSHd settings.  A line is \"name value\"; anything from # to the end of\n"
    "# the line is a comment.  Remove the # in front of a setting to change\n"
    "# it.  Changes take effect when SSHd is next started: Quit it from its\n"
    "# icon bar menu and double-click !SSHd.\n"
    "\n"
    "# TCP port to listen on\n"
    "#port 22\n"
    "\n"
    "# Address to listen on; 0.0.0.0 is every interface\n"
    "#bind 0.0.0.0\n"
    "\n"
    "# Directory SFTP clients start in\n"
    "#root <Boot$Dir>.^\n"
    "\n"
    "# User name to log in with (ssh riscos@host); write 'user' with no value\n"
    "# to accept any name\n"
    "#user riscos\n"
    "\n"
    "# off refuses interactive sessions (ssh host)\n"
    "#shell on\n"
    "# off refuses single commands (ssh host '*Cat')\n"
    "#exec on\n"
    "# off refuses SFTP and scp\n"
    "#sftp on\n"
    "\n"
    "# Seconds a connection has to log in, and seconds a logged-in connection\n"
    "# may sit with no session open; 0 is unlimited\n"
    "#auth-timeout 60\n"
    "#idle-timeout 120\n"
    "\n"
    "# Connections held at once\n"
    "#max-sessions 8\n"
    "\n"
    "# Send a text file called Banner in this directory to clients before\n"
    "# they log in, and show one called Motd when a shell starts\n"
    "#banner on\n"
    "#motd on\n"
    "\n"
    "# Type for uploads with no ,xxx suffix and no MimeMap match:\n"
    "# ffd Data, fff Text\n"
    "#filetype ffd\n"
    "\n"
    "# 1 for more log lines, 2 for SSH protocol detail\n"
    "#verbose 0\n"
    "\n"
    "# off stops writing the file log in this directory; past log-size KB it\n"
    "# becomes log-old and a new log starts\n"
    "#log on\n"
    "#log-size 256\n";

static char *options_file;

const char *options_path(void)
{
    return options_file ? options_file : "";
}

void options_ensure_file(void)
{
    if (options_file && ro_file_size(options_file) < 0)
        ro_save_file(options_file, 0xfff, options_template, strlen(options_template));
}

enum setting_kind { S_INT, S_HEX, S_BOOL, S_STR };

static const struct setting {
    const char *name;
    enum setting_kind kind;
    void *value;
} settings[] = {
    { "port",         S_INT,  &cfg.port },
    { "bind",         S_STR,  &cfg.bind_addr },
    { "root",         S_STR,  &root_arg },
    { "user",         S_STR,  &cfg.user },
    { "shell",        S_BOOL, &cfg.allow_shell },
    { "exec",         S_BOOL, &cfg.allow_exec },
    { "sftp",         S_BOOL, &cfg.allow_sftp },
    { "auth-timeout", S_INT,  &cfg.auth_timeout },
    { "idle-timeout", S_INT,  &cfg.idle_timeout },
    { "max-sessions", S_INT,  &cfg.max_sessions },
    { "banner",       S_BOOL, &banner },
    { "motd",         S_BOOL, &motd },
    { "filetype",     S_HEX,  &cfg.default_filetype },
    { "verbose",      S_INT,  &log_verbose },
    { "log",          S_BOOL, &logfile },
    { "log-size",     S_INT,  &cfg.log_size_kb },
};

static void apply_setting(const struct setting *st, const char *val)
{
    char *end;
    long n;
    switch (st->kind) {
    case S_STR: {
        char *v = strdup(val);
        if (v)
            *(const char **)st->value = v;
        return;
    }
    case S_BOOL:
        if (!strcmp(val, "on") || !strcmp(val, "yes") || !strcmp(val, "1"))
            *(int *)st->value = 1;
        else if (!strcmp(val, "off") || !strcmp(val, "no") || !strcmp(val, "0"))
            *(int *)st->value = 0;
        else
            log_msg("Options: %s must be on or off, not '%s'", st->name, val);
        return;
    case S_INT:
    case S_HEX:
        n = strtol(val, &end, st->kind == S_HEX ? 16 : 10);
        if (*val && !*end)
            *(int *)st->value = (int)n;
        else
            log_msg("Options: %s needs a %s number, not '%s'", st->name,
                    st->kind == S_HEX ? "hexadecimal" : "whole", val);
        return;
    }
}

/* Settings from <config dir>.Options; written as a template if missing.
 * The file survives package upgrades, which replace !Run. */
static void read_options(const char *dir)
{
    char *data, *p, *nl;
    options_file = join(dir, "Options");
    if (!options_file)
        return;
    data = ro_load_file(options_file, NULL);
    if (!data) {
        options_ensure_file();
        return;
    }
    for (p = data; p && *p; p = nl) {
        char key[32], val[256];
        char *hash, *end;
        size_t i;
        int n;
        nl = strchr(p, '\n');
        if (nl)
            *nl++ = '\0';
        /* "# ..." anywhere starts a comment; the value is the rest of the
         * line, trimmed, and may hold spaces (a directory name) */
        hash = strchr(p, '#');
        if (hash)
            *hash = '\0';
        for (end = p + strlen(p); end > p && (end[-1] == ' ' || end[-1] == '\t' || end[-1] == '\r'); end--)
            end[-1] = '\0';
        n = sscanf(p, "%31s %255[^\n]", key, val);
        if (n < 1)
            continue;
        if (n == 1)
            val[0] = '\0';               /* "user" alone: any user */
        for (i = 0; i < sizeof settings / sizeof settings[0]; i++)
            if (strcmp(key, settings[i].name) == 0)
                break;
        if (i < sizeof settings / sizeof settings[0])
            apply_setting(&settings[i], val);
        else
            log_msg("Options: unknown setting '%s'", key);
    }
    free(data);
}

/* Range checks once the file and the command line are both in. */
static int check_settings(void)
{
    if (cfg.port <= 0 || cfg.port > 65535)
        return fatal("invalid port number %d", cfg.port);
    if (cfg.max_sessions < 1 || cfg.max_sessions > 64) {
        log_msg("Options: max-sessions %d out of range, using %d", cfg.max_sessions, DEFAULT_MAX_SESSIONS);
        cfg.max_sessions = DEFAULT_MAX_SESSIONS;
    }
    if (cfg.auth_timeout < 0) cfg.auth_timeout = 0;
    if (cfg.idle_timeout < 0) cfg.idle_timeout = 0;
    if (cfg.default_filetype < 0 || cfg.default_filetype > 0xfff) {
        log_msg("Options: filetype must be three hex digits, using %03x", DEFAULT_FILETYPE);
        cfg.default_filetype = DEFAULT_FILETYPE;
    }
    if (cfg.log_size_kb < 16) cfg.log_size_kb = 16;
    return 0;
}

/* A text file from the configuration directory with line ends made CRLF
 * for the terminal, or NULL if absent or empty. */
static char *load_text(const char *dir, const char *leaf)
{
    char *path = join(dir, leaf), *data, *out, *p, *q;
    size_t len = 0, lines = 0;
    if (!path)
        return NULL;
    data = ro_load_file(path, &len);
    free(path);
    if (!data || len == 0) {
        free(data);
        return NULL;
    }
    if (len > 4096)
        len = 4096;
    for (p = data; p < data + len; p++)
        if (*p == '\n')
            lines++;
    out = malloc(len + lines + 3);
    if (!out) {
        free(data);
        return NULL;
    }
    for (p = data, q = out; p < data + len; p++) {
        if (*p == '\r')
            continue;
        if (*p == '\n')
            *q++ = '\r';
        *q++ = *p;
    }
    if (q > out && q[-1] != '\n') {
        *q++ = '\r';
        *q++ = '\n';
    }
    *q = '\0';
    free(data);
    return out;
}

/* libssh's own log, through ours (its default goes to stderr, which on the
 * desktop is the screen).  Dropped while verbose is below 2, so unticking
 * Verbose in the menu silences it. */
static void libssh_log(int priority, const char *function, const char *buffer, void *userdata)
{
    (void)priority; (void)function; (void)userdata;
    if (log_verbose >= 2)
        log_msg("libssh: %s", buffer);
}

int main(int argc, char **argv)
{
    ssh_key hostkey;
    const char *dir_arg = "<Choices$Write>.SSHd";
    const char *err;
    char *sftp_root;
    int n, opt;
    int opt_port = 0, port_given = 0, opt_verbose = 0;
    const char *opt_root = NULL, *opt_bind = NULL;

    while ((opt = getopt(argc, argv, "p:d:r:b:tv")) != -1) {
        switch (opt) {
        case 'p': {
            char *end;
            port_given = 1;
            opt_port = (int)strtol(optarg, &end, 10);
            if (!*optarg || *end)
                opt_port = 0;                  /* rejected by check_settings */
            break;
        }
        case 'd': dir_arg = optarg; break;
        case 'r': opt_root = optarg; break;
        case 'b': opt_bind = optarg; break;
        case 't': text_mode = 1; break;
        case 'v': opt_verbose++; break;
        default: usage(); return 1;
        }
    }

    log_set_stderr(text_mode);
    log_msg("SSHd %s starting", SSHD_VERSION);
    /* Every key and nonce comes from CryptRandom.  libssh is patched to
     * fail without it too; this check comes first with a clear message. */
    if (riscos_entropy_check() < 0)
        return fatal("the CryptRandom module is missing or not working: install the CryptRandom "
                     "package with PackMan, then run !SSHd again");
    ssh_threads_set_callbacks(ssh_threads_get_noop());
    if (ssh_init() < 0)
        return fatal("libssh initialisation failed (random number source)");
    crypto_ready = 1;
    ro_init_signals(text_mode ? on_quit : NULL);   /* Escape quits in text mode only */

    cfg.config_dir = ro_gstrans(dir_arg);
    if (!cfg.config_dir || !*cfg.config_dir)
        return fatal("cannot resolve the configuration directory '%s'", dir_arg);
    err = ro_mkdir(cfg.config_dir);
    if (err)
        return fatal("cannot create the configuration directory: %s", err);
    cfg.hostkey_path = join(cfg.config_dir, "hostkey");
    cfg.authorized_keys_path = join(cfg.config_dir, "authkeys");
    if (text_mode)
        cfg.capture_path = join(cfg.config_dir, "capture");

    read_options(cfg.config_dir);
    if (port_given) cfg.port = opt_port;   /* check_settings refuses a bad one */
    if (opt_root) root_arg = opt_root;
    if (opt_bind) cfg.bind_addr = opt_bind;
    if (opt_verbose) log_verbose = opt_verbose;
    if (check_settings())
        return 1;
    if (banner)
        cfg.banner = load_text(cfg.config_dir, "Banner");
    if (motd)
        cfg.motd = load_text(cfg.config_dir, "Motd");
    if (log_verbose >= 2) {
        ssh_set_log_level(SSH_LOG_PROTOCOL);
        ssh_set_log_callback(libssh_log);
    }
    if (logfile) {
        char *logpath = join(cfg.config_dir, "log");
        const char *why = "out of memory";
        if (!logpath || log_set_file(logpath, (size_t)cfg.log_size_kb * 1024, &why) < 0)
            log_msg("cannot write log file %s: %s", logpath ? logpath : "", why);
    }
    log_msg("configuration in %s", cfg.config_dir);
    if (!cfg.allow_shell || !cfg.allow_exec || !cfg.allow_sftp)
        log_msg("services: shell %s, exec %s, sftp %s", cfg.allow_shell ? "on" : "off",
                cfg.allow_exec ? "on" : "off", cfg.allow_sftp ? "on" : "off");
    if (cfg.user[0])
        log_msg("only user name %s may log in", cfg.user);
    riscos_sftp_default_type = cfg.default_filetype;

    /* SFTP: libssh's server resolves paths relative to the process's
     * current directory, so make that the configured root. */
    sftp_root = ro_gstrans(root_arg);
    if (sftp_root && *sftp_root && chdir(sftp_root) == 0)
        log_msg("sftp root %s", sftp_root);
    else
        log_msg("sftp root '%s' not usable: clients start in the server's directory", root_arg);
    free(sftp_root);

    n = auth_load_keys(cfg.authorized_keys_path);
    if (n < 0) {
        log_msg("no %s yet: add your public key in the Keys window", cfg.authorized_keys_path);
        ro_save_file(cfg.authorized_keys_path, 0xfff, "", 0);
        n = 0;
    } else {
        log_msg("%d authorised key%s", n, n == 1 ? "" : "s");
    }

    hostkey = hostkey_load_or_create(cfg.hostkey_path);
    if (!hostkey)
        return fatal("cannot load or generate the host key %s", cfg.hostkey_path);

    if (server_init(hostkey) < 0) {
        char running_ver[32];
        /* A second copy started by hand while one runs: say so in a box
         * (someone is at the keyboard) and leave no second icon behind. */
        if (!text_mode && ro_read_var("SSHd$Running", running_ver, sizeof running_ver) == 0) {
            char msg[128];
            snprintf(msg, sizeof msg, "SSHd %s is already running; its icon is on the icon bar.", running_ver);
            wimp_report_error(msg);
            return 1;
        }
        return fatal("cannot listen on port %d: another program is using it, or the network "
                     "is not running; choose another port with Choices..., then Quit and run "
                     "!SSHd again", cfg.port);
    }
    _swix(OS_SetVarVal, _INR(0, 4), "SSHd$Running", SSHD_VERSION, (int)strlen(SSHD_VERSION), 0, 0);
    {
        char hint[96];
        connect_hint(hint, sizeof hint);
        if (hint[0])
            log_msg("connect with: %s", hint);
        else
            log_msg("no network address yet; the Keys window shows one once there is");
    }

    if (text_mode) {
        signal(SIGTERM, on_term);
        signal(SIGHUP, on_term);
        log_msg("press Escape in this window to stop the server");
        while (running) {
            server_poll(100);
            ro_yield();
        }
    } else {
        if (wimp_init() < 0) {
            ro_unset_var("SSHd$Running");
            return 1;
        }
        if (n == 0)
            wimp_open_keys_window();
        wimp_run();
    }

    log_msg("shutting down");
    ro_unset_var("SSHd$Running");
    server_shutdown();
    auth_free();
    ssh_finalize();
    wimp_shutdown();
    return 0;
}

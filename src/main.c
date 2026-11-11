/* sshd for RISC OS: entry point.
 *
 * By default this is a Wimp task with an icon-bar icon (src/wimp.c); the
 * server loop body, server_poll(), runs on null events.  With -t it is a
 * plain command-line program for a TaskWindow, handy for debugging.
 *
 * Usage: sshd [-p port] [-d <config dir>] [-r <sftp dir>] [-b addr] [-t] [-v]
 * The directory's Options file (port, root, bind, verbose, log) is read
 * first; command line options override it.
 */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
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
#include "wimp.h"
#include "log.h"

#ifndef SSHD_VERSION
#define SSHD_VERSION "dev"
#endif

struct sshd_config cfg = { 22, "0.0.0.0", NULL, NULL, NULL, NULL, NULL };

/* UnixLib path translation, read from this application-defined global:
 * no c/h/s suffix directories, and the HostFS filetype convention
 * (",xxx" suffixes on names, MimeMap for extensions), see riscos_sftp.h. */
int __riscosify_control = __RISCOSIFY_NO_SUFFIX | __RISCOSIFY_FILETYPE_EXT;

static volatile int running = 1;
static int text_mode;

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
            "Usage: sshd [-p port] [-d config-dir] [-r sftp-dir] [-b address] [-t] [-v]\n"
            "  -p port        TCP port to listen on (default 22)\n"
            "  -d dir         configuration directory (default <Choices$Write>.SSHd)\n"
            "  -b address     address to listen on (default 0.0.0.0, all interfaces)\n"
            "  -r dir         directory SFTP clients start in (default <Boot$Dir>.^, the boot disc)\n"
            "  -t             text mode: no Wimp, log to this window (run in a TaskWindow)\n"
            "  -v             verbose; -vv also logs the SSH protocol\n"
            "The directory holds 'hostkey' (generated on first run) and\n"
            "'authkeys' (OpenSSH authorized_keys format; one key per line).\n");
}

/* Fatal startup error.  In text mode: message and exit.  On the desktop:
 * log it, open the log window and keep the icon on the bar so the reason
 * can be read and the task quit, instead of a modal box that would stall
 * a boot sequence running !SSHd from Tasks. */
static int fatal(const char *fmt, const char *arg)
{
    char msg[300];
    snprintf(msg, sizeof msg, fmt, arg);
    log_msg("startup failed: %s", msg);
    if (!text_mode && wimp_init() == 0) {
        log_msg("fix the cause, then Quit from the icon bar menu and run !SSHd again");
        wimp_open_log_window();
        wimp_run();
        wimp_shutdown();
    }
    return 1;
}

/* Settings from <config dir>.Options, one "name value" per line:
 *   port N, root <dir>, bind <address>, verbose N, log on|off.
 * Command line options override.  Survives package upgrades, which
 * replace !Run. */
static void read_options(const char *dir, const char **root_arg, int *logfile)
{
    char *path = join(dir, "Options"), *data, *p, *nl;
    if (!path)
        return;
    data = ro_load_file(path, NULL);
    free(path);
    if (!data)
        return;
    for (p = data; p && *p; p = nl) {
        char key[32], val[256];
        nl = strchr(p, '\n');
        if (nl)
            *nl++ = '\0';
        if (*p == '#' || sscanf(p, "%31s %255[^\r]", key, val) != 2)
            continue;
        if (strcmp(key, "port") == 0)
            cfg.port = atoi(val);
        else if (strcmp(key, "root") == 0)
            *root_arg = strdup(val);
        else if (strcmp(key, "bind") == 0)
            cfg.bind_addr = strdup(val);
        else if (strcmp(key, "verbose") == 0)
            log_verbose = atoi(val);
        else if (strcmp(key, "log") == 0)
            *logfile = strcmp(val, "off") != 0;
        else
            log_msg("Options: unknown setting '%s'", key);
    }
    free(data);
}

static void libssh_log(int priority, const char *function, const char *buffer, void *userdata)
{
    (void)priority; (void)function; (void)userdata;
    log_msg("libssh: %s", buffer);
}

int main(int argc, char **argv)
{
    ssh_key hostkey;
    const char *dir_arg = "<Choices$Write>.SSHd";
    const char *root_arg = "<Boot$Dir>.^";
    const char *err;
    int n, opt, logfile = 1;
    int opt_port = 0, opt_verbose = 0;
    const char *opt_root = NULL, *opt_bind = NULL;

    while ((opt = getopt(argc, argv, "p:d:r:b:tvh")) != -1) {
        switch (opt) {
        case 'p': opt_port = atoi(optarg); break;
        case 'd': dir_arg = optarg; break;
        case 'r': opt_root = optarg; break;
        case 'b': opt_bind = optarg; break;
        case 't': text_mode = 1; break;
        case 'v': opt_verbose++; break;
        default: usage(); return 1;
        }
    }

    ssh_threads_set_callbacks(ssh_threads_get_noop());
    if (ssh_init() < 0)
        return fatal("libssh initialisation failed%s", "");
    if (log_verbose > 1) {
        ssh_set_log_level(SSH_LOG_PROTOCOL);
        ssh_set_log_callback(libssh_log);
    }
    ro_init_signals(text_mode ? on_quit : NULL);   /* Escape quits in text mode only */

    cfg.config_dir = ro_gstrans(dir_arg);
    if (!cfg.config_dir || !*cfg.config_dir)
        return fatal("Cannot resolve configuration directory '%s'", dir_arg);
    err = ro_mkdir(cfg.config_dir);
    if (err)
        return fatal("Cannot create configuration directory: %s", err);
    cfg.hostkey_path = join(cfg.config_dir, "hostkey");
    cfg.authorized_keys_path = join(cfg.config_dir, "authkeys");
    cfg.capture_path = join(cfg.config_dir, "capture");

    read_options(cfg.config_dir, &root_arg, &logfile);
    if (opt_port) cfg.port = opt_port;
    if (opt_root) root_arg = opt_root;
    if (opt_bind) cfg.bind_addr = opt_bind;
    if (opt_verbose) log_verbose = opt_verbose;
    if (cfg.port <= 0 || cfg.port > 65535)
        return fatal("Invalid port %s", "number");
    if (logfile) {
        /* keep the log file bounded: start afresh when it has grown large */
        char *logpath = join(cfg.config_dir, "log");
        size_t len = 0;
        char *old = ro_load_file(logpath, &len);
        free(old);
        if (len > 256 * 1024)
            ro_delete(logpath);
        if (ro_append_file(logpath, "", 0) == 0)
            log_set_file(logpath);
        else
            log_msg("cannot write log file %s: %s", logpath, ro_append_error);
    }
    log_msg("configuration in %s", cfg.config_dir);

    /* SFTP: libssh's server resolves paths relative to the process's
     * current directory, so make that the configured root. */
    cfg.sftp_root = ro_gstrans(root_arg);
    if (cfg.sftp_root && *cfg.sftp_root && chdir(cfg.sftp_root) == 0)
        log_msg("sftp root %s", cfg.sftp_root);
    else
        log_msg("sftp root '%s' not usable: clients start in the server's directory", root_arg);

    n = auth_load_keys(cfg.authorized_keys_path);
    if (n < 0) {
        log_msg("no %s yet: add your public key in the Keys window", cfg.authorized_keys_path);
        ro_save_file(cfg.authorized_keys_path, 0xfff, "", 0);
        n = 0;
    } else {
        log_msg("%d authorized key%s", n, n == 1 ? "" : "s");
    }

    hostkey = hostkey_load_or_create(cfg.hostkey_path);
    if (!hostkey)
        return fatal("Cannot load or generate the host key (is CryptRandom loaded?)%s", "");

    if (server_init(hostkey) < 0) {
        char port[16], running_ver[32];
        snprintf(port, sizeof port, "%d", cfg.port);
        /* A second copy started by hand while one runs: say so in a box
         * (someone is at the keyboard) and leave no second icon behind. */
        if (!text_mode && ro_read_var("SSHd$Running", running_ver, sizeof running_ver) == 0) {
            char msg[128];
            snprintf(msg, sizeof msg, "SSHd %s is already running (port %s in use)", running_ver, port);
            wimp_report_error(msg);
            return 1;
        }
        return fatal("Cannot listen on port %s: is another SSH server running?", port);
    }
    _swix(OS_SetVarVal, _INR(0, 4), "SSHd$Running", SSHD_VERSION, (int)strlen(SSHD_VERSION), 0, 0);
    if (!text_mode && wimp_init() < 0)
        return 1;

    signal(SIGTERM, on_term);
    signal(SIGHUP, on_term);

    if (text_mode) {
        log_msg("press Escape in this window to stop the server");
        while (running) {
            server_poll(100);
            ro_yield();
        }
    } else {
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

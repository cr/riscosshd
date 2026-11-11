/* sshd for RISC OS: entry point.
 *
 * By default this is a Wimp task with an icon-bar icon (src/wimp.c); the
 * server loop body, server_poll(), runs on null events.  With -t it is a
 * plain command-line program for a TaskWindow, handy for debugging.
 *
 * Usage: sshd [-p port] [-d <config dir>] [-r <sftp dir>] [-t] [-v]
 */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <signal.h>
#include <unistd.h>
#include <unixlib/local.h>
#include <libssh/libssh.h>
#include <libssh/callbacks.h>

#include "config.h"
#include "server.h"
#include "auth.h"
#include "hostkey.h"
#include "riscos.h"
#include "wimp.h"
#include "log.h"

struct sshd_config cfg = { 22, "0.0.0.0", NULL, NULL, NULL, NULL, NULL };

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
            "Usage: sshd [-p port] [-d config-dir] [-r sftp-dir] [-t] [-v]\n"
            "  -p port        TCP port to listen on (default 22)\n"
            "  -d dir         configuration directory (default <Choices$Write>.SSHd)\n"
            "  -r dir         directory SFTP clients start in (default <Boot$Dir>.^, the boot disc)\n"
            "  -t             text mode: no Wimp, log to this window (run in a TaskWindow)\n"
            "  -v             verbose; -vv also logs the SSH protocol\n"
            "The directory holds 'hostkey' (generated on first run) and\n"
            "'authkeys' (OpenSSH authorized_keys format; one key per line).\n");
}

/* Fatal startup error: error box on the desktop, message in text mode. */
static int fatal(const char *fmt, const char *arg)
{
    char msg[300];
    snprintf(msg, sizeof msg, fmt, arg);
    if (text_mode)
        log_msg("%s", msg);
    else
        wimp_report_error(msg);
    return 1;
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
    int n, opt;

    while ((opt = getopt(argc, argv, "p:d:r:tvh")) != -1) {
        switch (opt) {
        case 'p': cfg.port = atoi(optarg); break;
        case 'd': dir_arg = optarg; break;
        case 'r': root_arg = optarg; break;
        case 't': text_mode = 1; break;
        case 'v': log_verbose++; break;
        default: usage(); return 1;
        }
    }
    if (cfg.port <= 0 || cfg.port > 65535) {
        usage();
        return 1;
    }

    /* UnixLib path translation: plain dot/slash swapping only.  By default
     * it also looks extensions up in MimeMap and rewrites them, which
     * would turn "notes.txt" into "notes/Text"; filetypes are handled by
     * src/riscos_sftp.c instead. */
    __riscosify_control |= __RISCOSIFY_NO_SUFFIX | __RISCOSIFY_FILETYPE_NOT_SET;

    if (!text_mode && wimp_init() < 0)
        return 1;

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
        char port[16];
        snprintf(port, sizeof port, "%d", cfg.port);
        return fatal("Cannot listen on port %s: is another SSH server running?", port);
    }

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
    server_shutdown();
    auth_free();
    ssh_finalize();
    wimp_shutdown();
    return 0;
}

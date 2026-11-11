/* sshd for RISC OS: entry point and cooperative main loop.
 *
 * This first milestone is a plain command-line program (run it in a
 * TaskWindow so the desktop keeps going).  The loop body, server_poll(),
 * is written so that a Wimp front end can call it from a null-event
 * handler later.
 *
 * Usage: sshd [-p port] [-d <config dir>] [-v]
 */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <signal.h>
#include <unistd.h>

#include "config.h"
#include "server.h"
#include "auth.h"
#include "hostkey.h"
#include "riscos.h"
#include "log.h"

struct sshd_config cfg = { 22, "0.0.0.0", NULL, NULL, NULL, NULL };

static volatile int running = 1;

static void on_term(int sig)
{
    (void)sig;
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
            "Usage: sshd [-p port] [-d config-dir] [-v]\n"
            "  -p port        TCP port to listen on (default 22)\n"
            "  -d dir         configuration directory (default <Choices$Write>.SSHd)\n"
            "  -v             verbose; -vv also logs the SSH protocol\n"
            "The directory holds 'hostkey' (generated on first run) and\n"
            "'authkeys' (OpenSSH authorized_keys format; one key per line).\n");
}

int main(int argc, char **argv)
{
    ssh_key hostkey;
    const char *dir_arg = "<Choices$Write>.SSHd";
    int n, opt;

    while ((opt = getopt(argc, argv, "p:d:vh")) != -1) {
        switch (opt) {
        case 'p': cfg.port = atoi(optarg); break;
        case 'd': dir_arg = optarg; break;
        case 'v': log_verbose++; break;
        default: usage(); return 1;
        }
    }
    if (cfg.port <= 0 || cfg.port > 65535) {
        usage();
        return 1;
    }

    cfg.config_dir = ro_gstrans(dir_arg);
    if (!cfg.config_dir || !*cfg.config_dir) {
        log_msg("cannot resolve configuration directory '%s'", dir_arg);
        return 1;
    }
    {
        const char *err = ro_mkdir(cfg.config_dir);
        if (err) {
            log_msg("cannot create %s: %s", cfg.config_dir, err);
            return 1;
        }
    }
    cfg.hostkey_path = join(cfg.config_dir, "hostkey");
    cfg.authorized_keys_path = join(cfg.config_dir, "authkeys");
    cfg.capture_path = join(cfg.config_dir, "capture");
    log_msg("configuration in %s", cfg.config_dir);

    n = auth_load_keys(cfg.authorized_keys_path);
    if (n < 0) {
        log_msg("no %s: nobody can log in. Put your public key there (one per line).",
                cfg.authorized_keys_path);
        ro_save_file(cfg.authorized_keys_path, 0xfff, "", 0);
    } else {
        log_msg("%d authorized key%s", n, n == 1 ? "" : "s");
    }

    hostkey = hostkey_load_or_create(cfg.hostkey_path);
    if (!hostkey)
        return 1;

    if (server_init(hostkey) < 0)
        return 1;

    ro_init_signals();
    signal(SIGTERM, on_term);
    signal(SIGHUP, on_term);

    while (running)
        server_poll(100);

    log_msg("shutting down");
    server_shutdown();
    auth_free();
    return 0;
}

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <signal.h>
#include <kernel.h>
#include <swis.h>

#include "riscos.h"
#include "log.h"

#ifndef OS_ChangeRedirection
#define OS_ChangeRedirection 0x5e
#endif

char *ro_gstrans(const char *src)
{
    char buf[1024];
    int len = 0;
    _kernel_oserror *e = _swix(OS_GSTrans, _INR(0, 2) | _OUT(2), src, buf, sizeof buf - 1, &len);
    if (e) {
        log_msg("GSTrans '%s': %s", src, e->errmess);
        return NULL;
    }
    buf[len] = '\0';
    return strdup(buf);
}

char *ro_load_file(const char *path, size_t *len_out)
{
    int type = 0, len = 0, fh = 0, unread = 0;
    char *buf;
    if (_swix(OS_File, _INR(0, 1) | _OUT(0) | _OUT(4), 17, path, &type, &len) || type != 1)
        return NULL;
    buf = malloc((size_t)len + 1);
    if (!buf)
        return NULL;
    /* Read through a handle for at most the size just seen, so a file
     * that grows between the two calls cannot overrun the buffer. */
    if (_swix(OS_Find, _INR(0, 1) | _OUT(0), 0x4f, path, &fh) || fh == 0) {
        free(buf);
        return NULL;
    }
    if (_swix(OS_GBPB, _INR(0, 3) | _OUT(3), 4, fh, buf, len, &unread)) {
        _swix(OS_Find, _INR(0, 1), 0, fh);
        free(buf);
        return NULL;
    }
    _swix(OS_Find, _INR(0, 1), 0, fh);
    if (unread > 0 && unread <= len)
        len -= unread;
    buf[len] = '\0';
    if (len_out)
        *len_out = (size_t)len;
    return buf;
}

const char *ro_save_file(const char *path, int filetype, const void *data, size_t len)
{
    _kernel_oserror *e = _swix(OS_File, _INR(0, 2) | _INR(4, 5), 10, path, filetype,
                               data, (const char *)data + len);
    return e ? e->errmess : NULL;
}

const char *ro_mkdir(const char *path)
{
    _kernel_oserror *e = _swix(OS_File, _INR(0, 1) | _IN(4), 8, path, 0);
    return e ? e->errmess : NULL;
}

int ro_append_file(const char *path, const void *data, size_t len)
{
    int fh = 0, extent = 0, unwritten = 0;
    /* 0xcf: open for update, create if missing, no path search */
    if (_swix(OS_Find, _INR(0, 1) | _OUT(0), 0xcf, path, &fh) || fh == 0)
        return -1;
    if (_swix(OS_Args, _INR(0, 1) | _OUT(2), 2, fh, &extent) == NULL)
        _swix(OS_Args, _INR(0, 2), 1, fh, extent);          /* pointer to the end */
    if (_swix(OS_GBPB, _INR(0, 3) | _OUT(3), 2, fh, data, len, &unwritten) || unwritten) {
        _swix(OS_Find, _INR(0, 1), 0, fh);
        return -1;
    }
    _swix(OS_Find, _INR(0, 1), 0, fh);
    return 0;
}

void ro_delete(const char *path)
{
    _swix(OS_File, _INR(0, 1), 6, path);
}

int ro_read_var(const char *name, char *buf, size_t len)
{
    int n = 0;
    /* R4 = 3: expanded as a string; an unset variable is an error */
    if (_swix(OS_ReadVarVal, _INR(0, 4) | _OUT(2), name, buf, len - 1, 0, 3, &n))
        return -1;
    if (n < 0 || (size_t)n >= len)
        n = 0;
    buf[n] = '\0';
    return 0;
}

void ro_unset_var(const char *name)
{
    _swix(OS_SetVarVal, _INR(0, 4), name, 0, -1, 0, 0);
}

static volatile int escape_seen;
static volatile int in_command;
static void (*quit_handler)(void);

static void on_sigint(int sig)
{
    (void)sig;
    signal(SIGINT, on_sigint);
    if (in_command)
        escape_seen = 1;
    else if (quit_handler)
        quit_handler();
}

void ro_yield(void)
{
    static int pollword = 1;
    _swix(OS_UpCall, _INR(0, 1), 6, &pollword);
}

/* A RISC OS error that UnixLib cannot map (SIGOSERROR), or a hardware
 * exception inside a SWI (SIGEMT), would otherwise die with just a signal
 * name.  Print the OS error text so module faults can be diagnosed. */
static void on_oserror(int sig)
{
    _kernel_oserror *e = _kernel_last_oserror();
    /* through log_msg so it also reaches the log file, then stderr */
    log_msg("fatal RISC OS error (signal %d): &%x %s", sig,
            e ? (unsigned)e->errnum : 0u, e ? e->errmess : "");
    fprintf(stderr, "sshd: fatal RISC OS error (signal %d)", sig);
    if (e)
        fprintf(stderr, ": &%x %s", (unsigned)e->errnum, e->errmess);
    fputc('\n', stderr);
    fflush(stderr);
    signal(sig, SIG_DFL);
    raise(sig);
}

void ro_init_signals(void (*quit_fn)(void))
{
    quit_handler = quit_fn;
    signal(SIGINT, on_sigint);
    signal(SIGEMT, on_oserror);
    signal(SIGOSERROR, on_oserror);
}

int ro_run_command(const char *command, const char *capture_path,
                   char **out, size_t *out_len, char **err)
{
    int fh = 0, old_in = 0, old_out = 0;
    _kernel_oserror *e, *cmd_err = NULL;
    char *data;
    size_t len = 0;

    *out = NULL;
    *out_len = 0;
    *err = NULL;

    /* Open the capture file for output (0x8f: create/truncate, no path search). */
    e = _swix(OS_Find, _INR(0, 1) | _OUT(0), 0x8f, capture_path, &fh);
    if (e) {
        *err = strdup(e->errmess);
        return 1;
    }

    /* Redirect VDU output to the file and run the command. */
    e = _swix(OS_ChangeRedirection, _INR(0, 1) | _OUTR(0, 1), 0, fh, &old_in, &old_out);
    if (e) {
        _swix(OS_Find, _INR(0, 1), 0, fh);
        *err = strdup(e->errmess);
        return 1;
    }
    fflush(stdout);
    escape_seen = 0;
    in_command = 1;
    cmd_err = _swix(OS_CLI, _IN(0), command);
    in_command = 0;
    fflush(stdout);
    _swix(OS_ChangeRedirection, _INR(0, 1), old_in, old_out);
    _swix(OS_Find, _INR(0, 1), 0, fh);

    data = ro_load_file(capture_path, &len);
    if (data) {
        *out = data;
        *out_len = len;
    }
    _swix(OS_File, _INR(0, 1), 6, capture_path); /* delete, ignore errors */

    if (cmd_err) {
        *err = strdup(cmd_err->errmess);
        return 1;
    }
    if (escape_seen) {
        *err = strdup("Escape");
        return 130;
    }
    return 0;
}

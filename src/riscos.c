#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <signal.h>
#include <unistd.h>
#include <sys/types.h>
#include <sys/socket.h>
#include <sys/ioctl.h>
#include <net/if.h>
#include <netinet/in.h>
#include <arpa/inet.h>
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
    if (len < 0 || len >= (int)sizeof buf)   /* never trust the length beyond the buffer */
        len = sizeof buf - 1;
    buf[len] = '\0';
    return strdup(buf);
}

char *ro_load_file(const char *path, size_t *len_out)
{
    long size = ro_file_size(path);
    int len = (int)size, fh = 0, unread = 0;
    char *buf;
    if (size < 0)
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

const char *ro_append_error = "";

long ro_file_size(const char *path)
{
    int type = 0, len = 0;
    if (_swix(OS_File, _INR(0, 1) | _OUT(0) | _OUT(4), 17, path, &type, &len) || type != 1)
        return -1;
    return len;
}

int ro_append_file(const char *path, const void *data, size_t len, size_t limit)
{
    int fh = 0, extent = 0, unwritten = 0, created = 0;
    _kernel_oserror *e;
    /* 0xc3: open for update with no path search (bits 0-1 = 3, so the name
     * is used as given); the handle is 0 when the file does not exist, as
     * only open-for-output (0x83) creates one. */
    e = _swix(OS_Find, _INR(0, 1) | _OUT(0), 0xc3, path, &fh);
    if (!e && fh == 0) {
        e = _swix(OS_Find, _INR(0, 1) | _OUT(0), 0x83, path, &fh);
        created = 1;
    }
    if (e || fh == 0) {
        ro_append_error = e ? e->errmess : "handle 0";
        return -1;
    }
    _swix(OS_Args, _INR(0, 1) | _OUT(2), 2, fh, &extent);
    if (limit && (size_t)extent > limit) {
        /* Past the limit the file becomes <path>-old (replacing an older
         * one) and a new file starts, so a flood of lines can neither fill
         * the disc nor wipe out the record just before it. */
        char old[300];
        _swix(OS_Find, _INR(0, 1), 0, fh);
        fh = 0;
        if ((size_t)snprintf(old, sizeof old, "%s-old", path) < sizeof old) {
            _swix(OS_File, _INR(0, 1), 6, old);
            _swix(OS_FSControl, _INR(0, 2), 25, path, old);   /* rename */
        }
        e = _swix(OS_Find, _INR(0, 1) | _OUT(0), 0x83, path, &fh);
        if (e || fh == 0) {
            ro_append_error = e ? e->errmess : "handle 0";
            return -1;
        }
        created = 1;
        extent = 0;
    }
    _swix(OS_Args, _INR(0, 2), 1, fh, extent);              /* pointer to the end */
    if (_swix(OS_GBPB, _INR(0, 3) | _OUT(3), 2, fh, data, len, &unwritten) || unwritten) {
        _swix(OS_Find, _INR(0, 1), 0, fh);
        ro_append_error = "write failed";
        return -1;
    }
    _swix(OS_Find, _INR(0, 1), 0, fh);
    if (created)
        _swix(OS_File, _INR(0, 2), 18, path, 0xfff);         /* Text */
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

int ro_local_address(char *buf, size_t len)
{
    /* SIOCGIFCONF fills entries of a 16-byte name and a sockaddr whose own
     * sa_len gives its size (the Internet module is BSD 4.4); UnixLib's
     * struct ifreq is padded differently, so the list is walked by hand. */
    char list[1024];
    struct ifconf ifc;
    size_t off = 0;
    int s = socket(AF_INET, SOCK_DGRAM, 0), rc = -1;
    if (s < 0)
        return -1;
    ifc.ifc_len = sizeof list;
    ifc.ifc_buf = list;
    if (ioctl(s, SIOCGIFCONF, &ifc) == 0) {
        while (off + IFNAMSIZ + sizeof(struct sockaddr) <= (size_t)ifc.ifc_len) {
            struct sockaddr *sa = (struct sockaddr *)(list + off + IFNAMSIZ);
            size_t salen = sa->sa_len > sizeof *sa ? sa->sa_len : sizeof *sa;
            if (sa->sa_family == AF_INET) {
                struct in_addr a = ((struct sockaddr_in *)sa)->sin_addr;
                if ((ntohl(a.s_addr) >> 24) != 127) {
                    snprintf(buf, len, "%s", inet_ntoa(a));
                    rc = 0;
                    break;
                }
            }
            off += IFNAMSIZ + salen;
        }
    }
    close(s);
    return rc;
}

int ro_now_cs(void)
{
    int t = 0;
    _swix(OS_ReadMonotonicTime, _OUT(0), &t);
    return t;
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
    signal(sig, SIG_DFL);               /* first: logging may fault again */
    ro_unset_var("SSHd$Running");
    /* through log_msg so it also reaches the log file, then stderr */
    log_msg("fatal RISC OS error (signal %d): &%x %s", sig,
            e ? (unsigned)e->errnum : 0u, e ? e->errmess : "");
    fprintf(stderr, "sshd: fatal RISC OS error (signal %d)", sig);
    if (e)
        fprintf(stderr, ": &%x %s", (unsigned)e->errnum, e->errmess);
    fputc('\n', stderr);
    fflush(stderr);
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

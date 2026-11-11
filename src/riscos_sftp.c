#include <stdio.h>
#include <string.h>
#include <kernel.h>
#include <swis.h>
#include <unixlib/local.h>

#include "riscos_sftp.h"
#include "log.h"

static int hexval(char c)
{
    if (c >= '0' && c <= '9') return c - '0';
    if (c >= 'a' && c <= 'f') return c - 'a' + 10;
    if (c >= 'A' && c <= 'F') return c - 'A' + 10;
    return -1;
}

int riscos_sftp_strip_suffix(char *name)
{
    size_t n = strlen(name);
    int a, b, c;
    if (n < 5 || name[n - 4] != ',')
        return -1;
    a = hexval(name[n - 3]); b = hexval(name[n - 2]); c = hexval(name[n - 1]);
    if (a < 0 || b < 0 || c < 0)
        return -1;
    name[n - 4] = '\0';
    return (a << 8) | (b << 4) | c;
}

void riscos_sftp_set_type(const char *unixpath, int type)
{
    char buf[1024];
    int ft;
    _kernel_oserror *e;
    /* __riscosify returns a pointer to the end of its output, hence buf */
    if (__riscosify_std(unixpath, 0, buf, sizeof buf, &ft) == NULL)
        return;
    e = _swix(OS_File, _INR(0, 2), 18, buf, type);
    if (e)
        log_msg("sftp: cannot set type &%03x on %s: %s", type, buf, e->errmess);
}

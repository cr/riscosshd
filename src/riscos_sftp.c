#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <ctype.h>
#include <kernel.h>
#include <swis.h>
#include <unixlib/local.h>

#include "riscos_sftp.h"
#include "log.h"

#ifndef MimeMap_Translate
#define MimeMap_Translate 0x50B00
#endif
#define MMM_MIME_TYPE   0
#define MMM_RISCOS_TYPE 1
#define MMM_EXTENSION   3

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

int riscos_sftp_type_from_extension(const char *name)
{
    const char *leaf = strrchr(name, '/'), *dot;
    int type = -1;
    leaf = leaf ? leaf + 1 : name;
    dot = strrchr(leaf, '.');
    if (!dot || dot == leaf || dot[1] == '\0')
        return -1;
    if (_swix(MimeMap_Translate, _INR(0, 2) | _OUT(3), MMM_EXTENSION, dot + 1, MMM_RISCOS_TYPE, &type))
        return -1;
    return type;
}

/* UnixLib path -> native RISC OS path in buf, or NULL.  (__riscosify
 * returns a pointer to the terminating NUL, hence the buf.) */
static const char *native(const char *unixpath, char *buf, size_t buflen)
{
    int ft;
    return __riscosify_std(unixpath, 0, buf, buflen, &ft) ? buf : NULL;
}

int riscos_sftp_type_of(const char *unixpath)
{
    char buf[1024];
    const char *path = native(unixpath, buf, sizeof buf);
    int objtype = 0;
    unsigned load = 0;
    if (!path)
        return -1;
    if (_swix(OS_File, _INR(0, 1) | _OUT(0) | _OUT(2), 17, path, &objtype, &load) || objtype != 1)
        return -1;
    if ((load & 0xfff00000u) != 0xfff00000u)
        return -1;                        /* not date-stamped: no type */
    return (int)((load >> 8) & 0xfff);
}

void riscos_sftp_set_type(const char *unixpath, int type)
{
    char buf[1024];
    const char *path = native(unixpath, buf, sizeof buf);
    _kernel_oserror *e;
    if (!path)
        return;
    e = _swix(OS_File, _INR(0, 2), 18, path, type);
    if (e)
        log_msg("sftp: cannot set type &%03x on %s: %s", type, path, e->errmess);
    else
        log_debug("sftp: %s typed &%03x", path, type);
}

const char *riscos_sftp_display_name(const char *unixpath, const char *leaf,
                                     const struct stat *st, char *out, size_t outlen)
{
    int type, implied;
    if (!S_ISREG(st->st_mode))
        return leaf;
    type = riscos_sftp_type_of(unixpath);
    if (type < 0)
        return leaf;
    implied = riscos_sftp_type_from_extension(leaf);
    if (implied == type)
        return leaf;
    snprintf(out, outlen, "%s,%03x", leaf, type);
    return out;
}

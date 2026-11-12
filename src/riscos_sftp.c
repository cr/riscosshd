/* SPDX-License-Identifier: MIT */
#include <stdlib.h>
#include <string.h>
#include <kernel.h>
#include <swis.h>
#include <unixlib/local.h>

#include "riscos_sftp.h"
#include "config.h"
#include "log.h"

/* The type a ",xxx" suffix at the end of name gives, or -1 if it has
 * none. */
static int suffix_type(const char *name)
{
    size_t n = strlen(name);
    if (n < 5 || name[n - 4] != ',' || strspn(name + n - 3, "0123456789abcdefABCDEF") != 3)
        return -1;
    return (int)strtol(name + n - 3, NULL, 16);
}

int riscos_sftp_strip_suffix(char *name)
{
    int type = suffix_type(name);
    if (type >= 0)
        name[strlen(name) - 4] = '\0';
    return type;
}

void riscos_sftp_type_new_file(const char *unixpath)
{
    const char *leaf = strrchr(unixpath, '/'), *dot;
    int type;
    leaf = leaf ? leaf + 1 : unixpath;
    if (suffix_type(leaf) >= 0)
        return;                         /* "name,xxx": UnixLib typed it */
    dot = strrchr(leaf, '.');
    /* MimeMap_Translate: input 3 = ".ext", output 0 = filetype number */
    if (dot && dot[1] && _swix(MimeMap_Translate, _INR(0, 3) | _OUT(3), 3, dot, 0, 0, &type) == NULL)
        return;                         /* MimeMap chose the type */
    riscos_sftp_set_type(unixpath, cfg.default_filetype);
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

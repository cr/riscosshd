/* SPDX-License-Identifier: MIT */
/* Host-side stand-ins for src/riscos.c, enough to unit-test auth.c. */
#include <stdio.h>
#include <stdlib.h>
#include "riscos.h"

char *ro_load_file(const char *path, size_t *len_out)
{
    FILE *f = fopen(path, "rb");
    char *buf;
    long n;
    if (!f)
        return NULL;
    fseek(f, 0, SEEK_END);
    n = ftell(f);
    rewind(f);
    buf = malloc((size_t)n + 1);
    if (!buf || fread(buf, 1, (size_t)n, f) != (size_t)n) {
        fclose(f);
        free(buf);
        return NULL;
    }
    fclose(f);
    buf[n] = '\0';
    if (len_out)
        *len_out = (size_t)n;
    return buf;
}

const char *ro_save_file(const char *path, int filetype, const void *data, size_t len)
{
    FILE *f = fopen(path, "wb");
    (void)filetype;
    if (!f)
        return "cannot open for writing";
    fwrite(data, 1, len, f);
    fclose(f);
    return NULL;
}

/* log.c appends to a log file once one is set; the test sets none, but
 * the symbols must link. */
const char *ro_append_file(const char *path, const void *data, size_t len, size_t limit)
{
    (void)path; (void)data; (void)len; (void)limit;
    return NULL;
}

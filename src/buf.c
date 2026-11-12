/* SPDX-License-Identifier: MIT */
#include <stdlib.h>
#include <string.h>
#include "buf.h"

int buf_append(struct buf *b, const void *data, size_t n)
{
    if (b->off + b->len + n > b->cap) {
        if (b->off) {                          /* reclaim the consumed front first */
            memmove(b->p, b->p + b->off, b->len);
            b->off = 0;
        }
        if (b->len + n > b->cap) {
            size_t ncap = b->cap ? b->cap : 4096;
            char *p;
            while (ncap < b->len + n)
                ncap *= 2;
            p = realloc(b->p, ncap);
            if (!p)
                return -1;
            b->p = p;
            b->cap = ncap;
        }
    }
    memcpy(b->p + b->off + b->len, data, n);
    b->len += n;
    return 0;
}

void buf_consume(struct buf *b, size_t n)
{
    if (n > b->len)
        n = b->len;
    b->off += n;
    b->len -= n;
    if (b->len == 0)
        b->off = 0;
}

void buf_free(struct buf *b)
{
    free(b->p);
    b->p = NULL;
    b->off = b->len = b->cap = 0;
}

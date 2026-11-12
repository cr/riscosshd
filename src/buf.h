/* SPDX-License-Identifier: MIT */
/* A growable byte queue: append at the end, consume from the front.
 * Consuming only moves an offset; the bytes are moved down once, when an
 * append would otherwise have to grow the buffer, so draining a large
 * queue in small pieces costs no copying. */
#ifndef SSHD_BUF_H
#define SSHD_BUF_H
#include <stddef.h>

struct buf {
    char *p;                  /* allocation */
    size_t off;               /* start of the unread bytes */
    size_t len;               /* number of unread bytes */
    size_t cap;               /* size of the allocation */
};

/* Append n bytes.  Returns 0, or -1 if memory ran out (nothing appended). */
int buf_append(struct buf *b, const void *data, size_t n);
/* Drop n bytes (at most len) from the front. */
void buf_consume(struct buf *b, size_t n);
/* Release the memory; the buffer is empty and reusable afterwards. */
void buf_free(struct buf *b);
/* The unread bytes. */
static inline char *buf_data(const struct buf *b) { return b->p + b->off; }

#endif

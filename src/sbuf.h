#ifndef LUME_SBUF_H
#define LUME_SBUF_H

/* Fork-local growable string buffer.
 *
 * The host tree (work/research/lume) gets this half of agent-httpd's
 * minijson.h — the reader half (jread_string / jfind_value) is not
 * implemented here. src/value.c only needs the buffer half, and this fork
 * deliberately does not depend on agent-httpd, so the three append calls are
 * inlined here instead of being linked from a static library.
 *
 * contract, kept identical to the original: `p` stays NULL until the first
 * append; the buffer is always NUL-terminated; `oom` is sticky, so a failed
 * growth turns every later append into a no-op (a later append cannot
 * resurrect a dead buffer with a half-written tail). */

#include <stddef.h>
#include <stdlib.h>
#include <string.h>

typedef struct {
    char *p;      /* NULL until first append; always NUL-terminated */
    size_t len;
    size_t cap;
    int oom;      /* sticky: further appends become no-ops */
} sbuf;

static inline void sb_mem(sbuf *b, const char *s, size_t n)
{
    if (b->oom) return;
    if (b->len + n + 1 > b->cap) {
        size_t cap = b->cap ? b->cap : 64;
        while (cap < b->len + n + 1) {
            if (cap > (size_t)-1 / 2) { b->oom = 1; return; }
            cap *= 2;
        }
        char *np = (char *)realloc(b->p, cap);
        if (!np) { b->oom = 1; return; }
        b->p = np;
        b->cap = cap;
    }
    memcpy(b->p + b->len, s, n);
    b->len += n;
    b->p[b->len] = '\0';
}

static inline void sb_str(sbuf *b, const char *s)
{
    if (s) sb_mem(b, s, strlen(s));
}

static inline void sb_chr(sbuf *b, char c)
{
    sb_mem(b, &c, 1);
}

#endif /* LUME_SBUF_H */

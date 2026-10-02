/* irbuf.c — see irbuf.h */

#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "irbuf.h"

void irbuf_init(IrBuf *b)
{
    b->cap  = 256;
    b->len  = 0;
    b->data = (char *)malloc(b->cap);
    if (!b->data) {
        b->cap = 0;
        return;
    }
    b->data[0] = '\0';
}

void irbuf_free(IrBuf *b)
{
    free(b->data);
    b->data = NULL;
    b->len = b->cap = 0;
}

static void irbuf_reserve(IrBuf *b, size_t extra)
{
    if (b->cap == 0) irbuf_init(b);
    if (b->cap - b->len > extra) return;

    size_t ncap = b->cap ? b->cap : 256;
    while (ncap - b->len <= extra) ncap *= 2;

    char *nd = (char *)realloc(b->data, ncap);
    if (!nd) return;                 /* out of memory: keep the old buffer */
    b->data = nd;
    b->cap  = ncap;
}

void irbuf_puts(IrBuf *b, const char *s)
{
    size_t n = strlen(s);
    irbuf_reserve(b, n + 1);
    if (!b->data) return;
    memcpy(b->data + b->len, s, n);
    b->len += n;
    b->data[b->len] = '\0';
}

void irbuf_printf(IrBuf *b, const char *fmt, ...)
{
    char tmp[1024];

    va_list ap;
    va_start(ap, fmt);
    int n = vsnprintf(tmp, sizeof tmp, fmt, ap);
    va_end(ap);

    if (n < 0) return;

    if ((size_t)n < sizeof tmp) {
        irbuf_reserve(b, (size_t)n + 1);
        if (!b->data) return;
        memcpy(b->data + b->len, tmp, (size_t)n);
        b->len += (size_t)n;
        b->data[b->len] = '\0';
        return;
    }

    /* long output: snprintf twice so we can drop the fixed tmp buffer */
    char *big = (char *)malloc((size_t)n + 1);
    if (!big) return;
    va_start(ap, fmt);
    vsnprintf(big, (size_t)n + 1, fmt, ap);
    va_end(ap);
    irbuf_puts(b, big);
    free(big);
}

/* Emit src as the contents of an IR string literal (the caller supplies the
 * surrounding double quotes). Everything is written as a \HH hex escape except
 * printable characters that are not `"` or `\` — LLVM's lexer accepts \HH for
 * arbitrary bytes, so this is safe for any source, including newlines. */
size_t irbuf_emit_ir_string(IrBuf *b, const char *src, size_t len)
{
    static const char hex[] = "0123456789abcdef";
    size_t start = b->len;
    size_t dec = 0;          /* bytes LLVM will actually see after unescaping */

    for (size_t i = 0; i < len; i++) {
        unsigned char c = (unsigned char)src[i];
        dec++;
        irbuf_reserve(b, 4);
        if (!b->data) return b->len - start;

        unsigned char need_hex =
            (c < 0x20) || (c == 0x7f) || (c == '"') || (c == '\\');

        if (!need_hex) {
            b->data[b->len++] = (char)c;
            continue;
        }

        b->data[b->len++] = '\\';
        b->data[b->len++] = hex[(c >> 4) & 0xf];
        b->data[b->len++] = hex[c & 0xf];
    }
    b->data[b->len] = '\0';
    return dec;     /* one LLVM byte per escaped or literal char */
}

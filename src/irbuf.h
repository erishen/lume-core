/* irbuf.h — growable text buffer used for emitting LLVM IR as text.
 *
 * [lume-core] We emit LLVM IR text (.ll) instead of linking libLLVM through
 * its C API. Rationale (see README.md): zero third-party link deps, and the
 * IR stays readable — which is the whole point of a research compiler. Any
 * later switch to the LLVM C API or MLIR can be done by swapping this file
 * for an IRBuilder.
 */

#ifndef LUMELLVM_IRBUF_H
#define LUMELLVM_IRBUF_H

#include <stddef.h>

typedef struct {
    char  *data;
    size_t len;
    size_t cap;
} IrBuf;

void irbuf_init(IrBuf *b);
void irbuf_free(IrBuf *b);

void irbuf_puts(IrBuf *b, const char *s);
void irbuf_printf(IrBuf *b, const char *fmt, ...);

/* Append src as the contents of an IR string literal, i.e. escape it the way
 * LLVM's lexer wants inside a "..." (backslashes, quotes and newlines).
 * Returns the number of bytes LLVM will see after unescaping, which is what
 * the `[N x i8]` array size must agree with (note `\XX` is one byte, not
 * three characters). */
size_t irbuf_emit_ir_string(IrBuf *b, const char *src, size_t len);

#endif /* LUMELLVM_IRBUF_H */

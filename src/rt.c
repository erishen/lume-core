/* lume-llvm runtime — non-variadic print helpers.
 *
 * Why this file exists: on macOS/arm64, `va_start` reads an argument save area
 * that the *caller* is required to build — that is why the C compiler emits a
 * `str x8, [sp]` sequence before `bl _printf`. A hand-written .ll that simply
 * leaves a value in x1 prints garbage (the callee re-reads whatever the stack
 * happens to hold). Declaring printf with a fixed prototype does not help,
 * because the broken part is the caller-side save area, not the signature.
 *
 * These helpers are compiled *from C*, so the compiler emits a correctly set
 * up call; the IR backend only issues ordinary non-variadic calls to them,
 * which pass arguments in plain integer registers.
 */

#include <stdio.h>
#include <string.h>

static int emit(const char *s)
{
    fputs(s, stdout);
    return (int)strlen(s);
}

long lume_print_i64(long v)
{
    char buf[40];
    int n = snprintf(buf, sizeof buf, "%ld\n", v);
    emit(buf);
    return n;
}

long lume_print_double(double v)
{
    char buf[64];
    int n = snprintf(buf, sizeof buf, "%f\n", v);
    emit(buf);
    return n;
}

long lume_print_bool(long v)
{
    return emit(v ? "true\n" : "false\n");
}

long lume_print_str(const char *s)
{
    /* Same line-ending contract as the other helpers: every print() ends the
     * line, so mixed int/string output stays readable. */
    char buf[512];
    int n = snprintf(buf, sizeof buf, "%s\n", s);
    if (n > (int)sizeof buf - 1) n = (int)sizeof buf - 1;
    return emit(buf);
}

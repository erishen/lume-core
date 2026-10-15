/* regex.h — vendored POSIX regex interface for the Windows port.
 *
 * Upstream: musl libc (src/regex/{regcomp,regexec,regerror,tre-mem}.c and
 * include/regex.h), MIT licensed, Copyright (C) 2005-2020 Rich Felker and
 * contributors.  Fetched 2026-10-15 from the musl git tree; see the files
 * in this directory for the upstream license text (MIT).
 *
 * Why this exists: lume's str_match/regex builtins call regcomp/regexec/
 * regfree.  POSIX platforms link the system regex; MinGW/Windows has no
 * POSIX regex in the C runtime — it used to come from the libgnurx package
 * (mingw-w64-ucrt-x86_64-libgnurx, LGPL).  That package is no longer
 * installable from the MSYS2 repositories (the Windows CI started failing
 * with "target not found" on 2026-10-15), and vendoring an LGPL library
 * into this MIT project would not be license-compatible anyway.  musl's
 * regex is a self-contained, MIT-licensed implementation of the same POSIX
 * API, so it is vendored here and compiled into the Windows build instead
 * of linking -lregex.
 *
 * This header is a portability rewrite of musl's include/regex.h: the
 * original includes <features.h> and <bits/alltypes.h>, which are musl
 * internals with no counterpart on MinGW/Windows.  The types and constants
 * below match musl's definitions (regoff_t is int, as on glibc) so the
 * vendored sources and their callers agree.
 *
 * Only the Windows build compiles this directory (see the Makefile's
 * IS_WINDOWS branch); POSIX builds keep using the system <regex.h>.
 */
#ifndef LUME_COMPAT_REGEX_H
#define LUME_COMPAT_REGEX_H

#include <stddef.h>

#ifdef __cplusplus
extern "C" {
#endif

typedef int regoff_t;

typedef struct re_pattern_buffer {
    size_t re_nsub;
    void *__opaque, *__padding[4];
    size_t __nsub2;
    char __padding2;
} regex_t;

typedef struct {
    regoff_t rm_so;
    regoff_t rm_eo;
} regmatch_t;

#define REG_EXTENDED    1
#define REG_ICASE       2
#define REG_NEWLINE     4
#define REG_NOSUB       8

#define REG_NOTBOL      1
#define REG_NOTEOL      2

#define REG_OK          0
#define REG_NOMATCH     1
#define REG_BADPAT      2
#define REG_ECOLLATE    3
#define REG_ECTYPE      4
#define REG_EESCAPE     5
#define REG_ESUBREG     6
#define REG_EBRACK      7
#define REG_EPAREN      8
#define REG_EBRACE      9
#define REG_BADBR       10
#define REG_ERANGE      11
#define REG_ESPACE      12
#define REG_BADRPT      13

#define REG_ENOSYS      -1

int regcomp(regex_t *__restrict, const char *__restrict, int);
int regexec(const regex_t *__restrict, const char *__restrict, size_t,
            regmatch_t *__restrict, int);
void regfree(regex_t *);

size_t regerror(int, const regex_t *__restrict, char *__restrict, size_t);

#ifdef __cplusplus
}
#endif

#endif /* LUME_COMPAT_REGEX_H */

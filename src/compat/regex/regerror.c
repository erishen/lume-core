/* regerror.c — vendored from musl libc (MIT licensed, Copyright (C) 2005-2020
 * Rich Felker and contributors; see src/compat/regex/regex.h for provenance
 * and why this is vendored).  Two portability edits vs upstream:
 *   - the musl-internal "locale_impl.h" include is dropped (no such header
 *     off musl; nothing else here needs it);
 *   - LCTRANS_CUR() (musl's locale message translation) is dropped — the
 *     English messages are used verbatim, as on any non-gettext platform.
 */
#include <string.h>
#include <regex.h>
#include <stdio.h>

/* Error message strings for error codes listed in `regex.h'.  This list
   needs to be in sync with the codes listed there, naturally. */

/* Converted to single string by Rich Felker to remove the need for
 * data relocations at runtime, 27 Feb 2006. */

static const char messages[] = {
  "No error\0"
  "No match\0"
  "Invalid regexp\0"
  "Unknown collating element\0"
  "Unknown character class name\0"
  "Trailing backslash\0"
  "Invalid back reference\0"
  "Missing ']'\0"
  "Missing ')'\0"
  "Missing '}'\0"
  "Invalid contents of {}\0"
  "Invalid character range\0"
  "Out of memory\0"
  "Repetition not preceded by valid expression\0"
  "\0Unknown error"
};

size_t regerror(int c, const regex_t *restrict preg, char *restrict buf, size_t size)
{
	const char *s;
	unsigned e=c;
	(void)preg;   /* lume: POSIX declares it; TRE's messages need only the code */
	for (s=messages; e && *s; e--, s+=strlen(s)+1);
	if (!*s) s++;
	return 1+snprintf(buf, size, "%s", s);
}

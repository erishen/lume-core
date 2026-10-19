#ifndef LUME_ARENA_H
#define LUME_ARENA_H

#include <stddef.h>

/* One bump arena per compilation unit (TODO.md P2).
 *
 * The parser used to reclaim mid-parse garbage through an orphan journal
 * plus a three-function node_free family, every new node shape and scope
 * rule owing those functions a hand-maintained case. The arena removes the
 * whole class instead: nothing allocated here is freed individually, the
 * memory dies wholesale in arena_reset() at the points where the tree dies
 * today (the same call sites that ran node_free — see docs/ARENA.md).
 *
 * Single-threaded by contract: the loader parses before fork, the LSP
 * parses one document version at a time. */
void  *arena_alloc(size_t sz);
char  *arena_strdup(const char *s);
/* Copy an n-byte slice (token text has no NUL terminator). */
char  *arena_strndup(const char *s, size_t n);
/* Grow an arena array: allocate new_bytes, copy old_bytes, abandon the old
 * block (bump semantics — the stale bytes stay reachable until reset, which
 * is fine at compile-time scale). Replaces the parser's realloc chains. */
void  *arena_grow(void *old, size_t old_bytes, size_t new_bytes);
/* Free every chunk. Call only where no arena memory is reachable: after
 * backend emission / interpretation ends, per fuzz input, per LSP document
 * version, at module-set teardown. */
void   arena_reset(void);

#endif /* LUME_ARENA_H */

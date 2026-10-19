#include "arena.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

/* Chunked bump allocator. Chunks live in a push-down list; reset frees the
 * whole list. The first allocation of a fresh chunk is served from its
 * head, so every chunk is 16-byte aligned and every offset stays 16-byte
 * aligned too — good for any scalar or vector the parser stamps in. */

typedef struct Chunk Chunk;
struct Chunk {
    Chunk *next;
    size_t used;
    size_t cap;
    max_align_t data[]; /* flexible: the bump region */
};

#define ARENA_CHUNK_MIN (64 * 1024)

static Chunk *head;

static void *arena_fill(size_t sz)
{
    size_t cap = ARENA_CHUNK_MIN;
    if (sz > cap) cap = sz;
    Chunk *c = malloc(sizeof(Chunk) + cap);
    if (!c) {
        fputs("lume: arena out of memory\n", stderr);
        exit(70);
    }
    c->next = head;
    c->used = 0;
    c->cap = cap;
    head = c;
    return c->data;
}

void *arena_alloc(size_t sz)
{
    sz = (sz + 15u) & ~(size_t)15u;
    if (!head || head->cap - head->used < sz)
        arena_fill(sz);
    char *p = (char *)head->data + head->used;
    head->used += sz;
    return p;
}

char *arena_strdup(const char *s)
{
    size_t n = strlen(s) + 1;
    char *r = arena_alloc(n);
    memcpy(r, s, n);
    return r;
}

char *arena_strndup(const char *s, size_t n)
{
    char *r = arena_alloc(n + 1);
    memcpy(r, s, n);
    r[n] = '\0';
    return r;
}

void *arena_grow(void *old, size_t old_bytes, size_t new_bytes)
{
    void *p = arena_alloc(new_bytes);
    if (old && old_bytes)
        memcpy(p, old, old_bytes);
    return p;
}

void arena_reset(void)
{
    Chunk *c = head;
    while (c) {
        Chunk *next = c->next;
        free(c);
        c = next;
    }
    head = NULL;
}

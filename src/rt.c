/* lume runtime (native backend) — non-variadic print helpers.
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

#include <math.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static int emit(const char *s)
{
    fputs(s, stdout);
    return (int)strlen(s);
}

/* Defined below but called from lume_print_str's null guard. */
long lume_print_null(void);

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
    /* %g, not %f: the interpreter prints floats the same way (`print(1.0)`
     * is "1", `print(1e6)` is "1e+06"), and this helper is what a compiled
     * script calls, so the two have to agree or the same program prints
     * differently compiled than interpreted. */
    int n = snprintf(buf, sizeof buf, "%g\n", v);
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
     * line, so mixed int/string output stays readable.
     *
     * The null guard is load-bearing, not defensive padding. A `let x: string
     * = null` reaches here as a null `i8*` (both native backends represent
     * TY_NULL as an opaque pointer), and handing that to snprintf's "%s" is
     * undefined behaviour -- glibc happens to print "(null)", but nothing
     * promises it. The backends now reject null for scalar annotations, so
     * only the string-annotated case can land here, and it must print the
     * same bare word the interpreter prints. */
    if (!s) return lume_print_null();
    char buf[512];
    int n = snprintf(buf, sizeof buf, "%s\n", s);
    if (n > (int)sizeof buf - 1) n = (int)sizeof buf - 1;
    return emit(buf);
}

/* `null` has no value to pass — it prints the bare word and takes no
 * argument, so `print(null)` matches the interpreter's "null" exactly. */
long lume_print_null(void)
{
    return emit("null\n");
}

/* ------------------------------------------------------------------ *
 * Builtin helpers.
 *
 * lume's own builtins are Value-based: `Native2(VM *, int, Value *, Value *)`.
 * A compile-time backend has no VM to hand them and no way to rebuild the
 * Value layout in IR, so the native backend exposes its own plain-scalar
 * helpers and lowers builtin calls to those instead. They are deliberately
 * dumb — no VM, no error reporting, no allocation policy — and the type
 * checker is what keeps them honest.
 * ------------------------------------------------------------------ */

static char *dup_str(const char *s)
{
    size_t n = strlen(s) + 1;
    char *p = malloc(n);
    if (p) memcpy(p, s, n);
    return p;
}

char *lume_bi_str_i64(long v)
{
    char buf[32];
    snprintf(buf, sizeof buf, "%ld", v);
    return dup_str(buf);
}

char *lume_bi_str_double(double v)
{
    char buf[48];
    snprintf(buf, sizeof buf, "%g", v);
    return dup_str(buf);
}

char *lume_bi_str_bool(long v) { return dup_str(v ? "true" : "false"); }

char *lume_bi_cat(const char *a, const char *b)
{
    size_t na = a ? strlen(a) : 0;
    size_t nb = b ? strlen(b) : 0;
    char *p = malloc(na + nb + 1);
    if (!p) return NULL;
    memcpy(p, a, na);
    memcpy(p + na, b, nb);
    p[na + nb] = '\0';
    return p;
}

long lume_bi_abs(long v)     { return v < 0 ? -v : v; }
long lume_bi_min(long a, long b) { return a < b ? a : b; }
long lume_bi_max(long a, long b) { return a > b ? a : b; }
double lume_bi_minf(double a, double b) { return a < b ? a : b; }
double lume_bi_maxf(double a, double b) { return a > b ? a : b; }

double lume_bi_sqrt(double v) { return sqrt(v); }
double lume_bi_pow(double a, double b) { return pow(a, b); }
double lume_bi_floor(double v) { return floor(v); }
double lume_bi_ceil(double v)  { return ceil(v); }
double lume_bi_round(double v) { return round(v); }

long   lume_bi_int(double v)  { return (long)v; }
double lume_bi_float(long v)  { return (double)v; }
long lume_bi_len_s(const char *s) { return s ? (long)strlen(s) : 0; }

/* ------------------------------------------------------------------ *
 * List and map objects.
 *
 * The interpreter keeps every value behind a GC-managed Value, so a list is
 * an Obj and a map is an Obj. The native backend has no GC, so these are
 * plain malloc'd records reached through an opaque pointer — codegen knows
 * the static element type and picks whichever helper fits, which is why the
 * slots carry a tag instead of a whole union: one push() lowers onto one
 * helper for ints, floats and strings alike.
 *
 * `num` holds an integer or the bit pattern of a double, `str` the body of
 * a string. A missing index reads as a zero slot rather than crashing: the
 * native path has no error channel of its own, and the type checker's
 * bounds rules are the thing that is supposed to keep indices in range.
 * ------------------------------------------------------------------ */

#define LUME_SLOT_INT    0
#define LUME_SLOT_FLOAT  1
#define LUME_SLOT_STR    2
/* A nested container: another LumeList or LumeMap, reached through the same
 * opaque i8* everything else travels as. Before this tag every slot held a
 * scalar, so `{ a: { b: 1 } }` and `[[1,2],[3]]` could not be built at all —
 * the emitters refused with "map values must be int, float or string" /
 * "list elements must be int, float or string" while the interpreter has
 * always accepted them. */
#define LUME_SLOT_OBJ    3

typedef struct { long num; const char *str; int tag; } LumeSlot;

/* Which container kind an LUME_SLOT_OBJ pointer refers to. A bare i8* does
 * not say, and the printer has to know to walk it as a list or as a map, so
 * the tag lives in the object itself and the slot just carries the address. */
#define LUME_CTR_LIST    0x4c49   /* 'LI' */
#define LUME_CTR_MAP     0x4d41   /* 'MA' */

typedef struct { long len, cap; int kind; LumeSlot *items; }    LumeList;
typedef struct { long len, cap; int kind; char **keys; LumeSlot *vals; } LumeMap;

static void *rt_alloc(size_t n)
{
    void *p = calloc(1, n ? n : 1);
    if (!p) { fputs("lume(native): out of memory\n", stderr); exit(70); }
    return p;
}

static void list_grow(LumeList *l)
{
    long cap = l->cap ? l->cap * 2 : 8;
    LumeSlot *it = realloc(l->items, (size_t)cap * sizeof *it);
    if (!it) { fputs("lume(native): out of memory\n", stderr); exit(70); }
    memset(it + (size_t)l->cap, 0, (size_t)(cap - l->cap) * sizeof *it);
    l->items = it;
    l->cap = cap;
}

LumeList *lume_list_new(void)
{
    LumeList *l = rt_alloc(sizeof(LumeList));
    l->kind = LUME_CTR_LIST;
    return l;
}

long lume_list_len(const LumeList *l) { return l ? l->len : 0; }

/* One push per element type: the caller picked the flavour from the static
 * type, so the slot can be filled without a tag round-trip. */
long lume_list_push_i(LumeList *l, long v)
{
    if (!l) return 0;
    if (l->len >= l->cap) list_grow(l);
    l->items[l->len].num = v;
    l->items[l->len].tag = LUME_SLOT_INT;
    return ++l->len;
}

long lume_list_push_f(LumeList *l, double v)
{
    if (!l) return 0;
    if (l->len >= l->cap) list_grow(l);
    long bits;
    memcpy(&bits, &v, sizeof bits);
    l->items[l->len].num = bits;
    l->items[l->len].tag = LUME_SLOT_FLOAT;
    return ++l->len;
}

long lume_list_push_s(LumeList *l, const char *v)
{
    if (!l) return 0;
    if (l->len >= l->cap) list_grow(l);
    l->items[l->len].str = v;
    l->items[l->len].tag = LUME_SLOT_STR;
    return ++l->len;
}

/* Push a nested container. `v` is the lume_list_new() / lume_map_new()
 * pointer, and its own `kind` is what tells a reader how to walk it later, so
 * one function serves both container kinds. */
long lume_list_push_obj(LumeList *l, void *v)
{
    if (!l) return 0;
    if (l->len >= l->cap) list_grow(l);
    l->items[l->len].num = (long)(intptr_t)v;
    l->items[l->len].str = NULL;
    l->items[l->len].tag = LUME_SLOT_OBJ;
    return ++l->len;
}

/* Render one slot the way the interpreter would print it. The interpreter's
 * print() falls back to JSON for anything that is not a plain scalar, so a
 * string inside a list or a map keeps its quotes and escapes — `["a","b"]`,
 * not `[a,b]`.
 *
 * Writes straight to stdout and returns the byte count rather than handing
 * back a string: a nested container (LUME_SLOT_OBJ) is printed by recursing
 * into its own elements, which cannot render into a fixed buffer — the depth
 * is unbounded and two of them can sit side by side in one list. Scalars
 * still go through a static buffer, each emitted before the next is written. */
/* Mutually recursive: a container body holds slots, and a slot may hold a
 * container. Declared before either body so both can call the other. */
static int slot_emit(LumeSlot s);

/* Write a container's body -- the elements between the brackets, no brackets
 * and no trailing newline. Both printers and slot_emit's LUME_SLOT_OBJ branch
 * go through here, so a nested container renders by exactly the same rules as
 * a top-level one and the three cannot drift. The cast to LumeList is safe for
 * both shapes: `kind` is the first int in each and `len`/`cap` follow it. */
static int container_body(const void *p)
{
    const LumeList *as_list = (const LumeList *)p;
    if (as_list->kind != LUME_CTR_LIST) {
        const LumeMap *m = (const LumeMap *)p;
        int n = 0;
        for (long i = 0; i < m->len; i++) {
            if (i) n += emit(",");
            n += emit("\"");
            n += emit(m->keys[i]);
            n += emit("\":");
            n += slot_emit(m->vals[i]);
        }
        return n;
    }
    int n = 0;
    for (long i = 0; i < as_list->len; i++) {
        if (i) n += emit(",");
        n += slot_emit(as_list->items[i]);
    }
    return n;
}

static int slot_emit(LumeSlot s)
{
    if (s.tag == LUME_SLOT_OBJ) {
        /* The slot holds the container's address; its own `kind` says which
         * of the two container shapes it is, since an i8* does not. The
         * printers below end with a newline, so the brackets are written
         * here -- this is a container *inside* a value, not a whole one. */
        const void *p = (const void *)(intptr_t)s.num;
        if (!p) return emit("null");
        const LumeList *shape = (const LumeList *)p;
        int n = emit(shape->kind == LUME_CTR_LIST ? "[" : "{");
        n += container_body(p);
        return n + emit(shape->kind == LUME_CTR_LIST ? "]" : "}");
    }

    static char buf[1024];
    if (!s.str) s.str = "";
    switch (s.tag) {
    case LUME_SLOT_STR: {
        size_t n = 0;
        buf[0] = '\0';
        buf[n++] = '"';
        for (const unsigned char *p = (const unsigned char *)s.str;
             *p && n + 8 < sizeof buf; p++) {
            switch (*p) {
            case '"':  n += (size_t)snprintf(buf + n, sizeof buf - n, "\\\""); break;
            case '\\': n += (size_t)snprintf(buf + n, sizeof buf - n, "\\\\"); break;
            case '\n': n += (size_t)snprintf(buf + n, sizeof buf - n, "\\n");  break;
            case '\t': n += (size_t)snprintf(buf + n, sizeof buf - n, "\\t");  break;
            case '\r': n += (size_t)snprintf(buf + n, sizeof buf - n, "\\r");  break;
            default:
                if (*p < 0x20)
                    n += (size_t)snprintf(buf + n, sizeof buf - n, "\\u%04x", *p);
                else
                    buf[n++] = (char)*p;
            }
        }
        /* The closing quote has to be NUL-terminated, not just written: emit() is
         * fputs + strlen, so whatever the previous caller left in this static
         * buffer would still be read after it. That went unnoticed while every
         * string in a program was written once per run — the first use of the
         * buffer was always preceded by a longer one filling it, or by nothing
         * at all and a zeroed buffer. Recursion is what made it visible: a
         * nested container is the second string rendered into the same buffer,
         * so `{ a: "AAAAAAAAAAAAAAAA" }` followed by `{ x: { y: "s" } }` printed
         * `{"y":"s"AAAAAAAAAAAAAA}` -- the 16-byte tail of the earlier string,
         * because the short one left the old bytes in place after its quote. */
        size_t end = n < sizeof buf ? n : sizeof buf - 1;
        buf[end] = '"';
        buf[end + 1 < sizeof buf ? end + 1 : end] = '\0';
        return emit(buf);
    }
    case LUME_SLOT_FLOAT: {
        double d = 0;
        memcpy(&d, &s.num, sizeof d);
        snprintf(buf, sizeof buf, "%g", d);
        return emit(buf);
    }
    default:              snprintf(buf, sizeof buf, "%ld", s.num); return emit(buf);
    }
}

/* Element access for the three statically known element types — the call
 * site picks the flavour, and a string slot read through the int helper
 * reads 0 rather than a dangling pointer. */
long lume_list_at_i(const LumeList *l, long i)
{
    if (!l || i < 0 || i >= l->len) return 0;
    return l->items[i].tag == LUME_SLOT_STR ? 0 : l->items[i].num;
}

double lume_list_at_f(const LumeList *l, long i)
{
    double d = 0;
    if (!l || i < 0 || i >= l->len) return d;
    if (l->items[i].tag == LUME_SLOT_STR) return d;
    memcpy(&d, &l->items[i].num, sizeof d);
    return d;
}

const char *lume_list_at_s(const LumeList *l, long i)
{
    if (!l || i < 0 || i >= l->len) return "";
    return l->items[i].tag == LUME_SLOT_STR ? l->items[i].str : "";
}

/* Read a nested container back out of a list -- the counterpart of
 * lume_map_get_obj, and the reason `for (let row in [[1],[2]])` has a way to
 * see the inner lists. Null for an out-of-range index or a scalar element. */
void *lume_list_at_obj(const LumeList *l, long i)
{
    if (!l || i < 0 || i >= l->len || l->items[i].tag != LUME_SLOT_OBJ) return NULL;
    return (void *)(intptr_t)l->items[i].num;
}

long lume_list_print(const LumeList *l)
{
    if (!l) return emit("[]\n");
    return emit("[") + container_body(l) + emit("]\n");
}


/* ---------------------------------------------------------------- maps --- */

static void map_grow(LumeMap *m)
{
    long cap = m->cap ? m->cap * 2 : 8;
    char **ks = realloc(m->keys, (size_t)cap * sizeof *ks);
    if (!ks) { fputs("lume(native): out of memory\n", stderr); exit(70); }
    memset(ks + (size_t)m->cap, 0, (size_t)(cap - m->cap) * sizeof *ks);
    LumeSlot *vs = realloc(m->vals, (size_t)cap * sizeof *vs);
    if (!vs) { fputs("lume(native): out of memory\n", stderr); exit(70); }
    memset(vs + (size_t)m->cap, 0, (size_t)(cap - m->cap) * sizeof *vs);
    m->keys = ks;
    m->vals = vs;
    m->cap  = cap;
}

static long map_find(const LumeMap *m, const char *k)
{
    if (!m || !k) return -1;
    for (long i = 0; i < m->len; i++)
        if (m->keys[i] && strcmp(m->keys[i], k) == 0) return i;
    return -1;
}

LumeMap *lume_map_new(void)
{
    LumeMap *m = rt_alloc(sizeof(LumeMap));
    m->kind = LUME_CTR_MAP;
    return m;
}

/* ---------- strict accessors for the index syntax (SPEC 6.2) ----------
 *
 * `l[0]` / `m["k"]` are strict: an out-of-range index or a missing key is a
 * runtime error, where the existing lume_list_at_* / lume_map_get_* helpers
 * answer 0 / a default. Those stay as they are because `el()` and `get()` are
 * specified in terms of them and quietly tightening them would change every
 * existing caller. These are for the syntax instead.
 *
 * A negative index counts from the end, so l[-1] is the last element --
 * resolved here once, for both native backends, rather than in each of them.
 *
 * The numeric flavours report failure out of band, via lume_index_failed(),
 * because a stored 0 is a legal value and cannot be told from a failed read by
 * the return alone. The string flavours take an `ok` out-param instead, since a
 * string has no sentinel of its own. */
static int g_index_failed = 0;

int lume_index_failed(void) { return g_index_failed; }

long lume_list_at_strict(const LumeList *l, long i)
{
    g_index_failed = 0;
    if (!l) { g_index_failed = 1; return 0; }
    if (i < 0) i += l->len;
    if (i < 0 || i >= l->len) { g_index_failed = 1; return 0; }
    return l->items[i].tag == LUME_SLOT_STR ? 0 : l->items[i].num;
}

double lume_list_at_strict_f(const LumeList *l, long i)
{
    g_index_failed = 0;
    if (!l) { g_index_failed = 1; return 0; }
    if (i < 0) i += l->len;
    if (i < 0 || i >= l->len) { g_index_failed = 1; return 0; }
    double d = 0;
    if (l->items[i].tag != LUME_SLOT_STR) memcpy(&d, &l->items[i].num, sizeof d);
    return d;
}

void *lume_list_at_strict_obj(const LumeList *l, long i)
{
    g_index_failed = 0;
    if (!l) { g_index_failed = 1; return NULL; }
    if (i < 0) i += l->len;
    if (i < 0 || i >= l->len) { g_index_failed = 1; return NULL; }
    if (l->items[i].tag != LUME_SLOT_OBJ) { g_index_failed = 1; return NULL; }
    return (void *)(intptr_t)l->items[i].num;
}

/* A string element has no sentinel, so `ok` reports whether one was there. */
const char *lume_list_at_strict_str(const LumeList *l, long i, int *ok)
{
    *ok = 0;
    if (!l) return NULL;
    if (i < 0) i += l->len;
    if (i < 0 || i >= l->len) return NULL;
    if (l->items[i].tag != LUME_SLOT_STR) return NULL;
    *ok = 1;
    return l->items[i].str;
}

long lume_map_at_strict_i(const LumeMap *m, const char *k)
{
    g_index_failed = 0;
    long i = map_find(m, k);
    if (i < 0 || m->vals[i].tag == LUME_SLOT_STR) { g_index_failed = 1; return 0; }
    return m->vals[i].num;
}

double lume_map_at_strict_f(const LumeMap *m, const char *k)
{
    g_index_failed = 0;
    long i = map_find(m, k);
    if (i < 0 || m->vals[i].tag == LUME_SLOT_STR) { g_index_failed = 1; return 0; }
    double d = 0;
    memcpy(&d, &m->vals[i].num, sizeof d);
    return d;
}

void *lume_map_at_strict_obj(const LumeMap *m, const char *k)
{
    g_index_failed = 0;
    long i = map_find(m, k);
    if (i < 0 || m->vals[i].tag != LUME_SLOT_OBJ) { g_index_failed = 1; return NULL; }
    return (void *)(intptr_t)m->vals[i].num;
}

const char *lume_map_at_strict_str(const LumeMap *m, const char *k, int *ok)
{
    *ok = 0;
    long i = map_find(m, k);
    if (i < 0 || m->vals[i].tag != LUME_SLOT_STR) return NULL;
    *ok = 1;
    return m->vals[i].str;
}


long lume_map_len(const LumeMap *m) { return m ? m->len : 0; }

static long map_put(LumeMap *m, const char *k, long num, const char *str, int tag)
{
    if (!m) return 0;
    long i = map_find(m, k);
    if (i < 0) {
        if (m->len >= m->cap) map_grow(m);
        i = m->len++;
        /* Key ownership: the map keeps its own copy, because a key that came
         * from a literal is a global constant but one that came from a
         * `put(m, expr, v)` call may be a temporary the IR drops. */
        m->keys[i] = dup_str(k);
    }
    m->vals[i].num = num;
    m->vals[i].str = str;
    m->vals[i].tag = tag;
    return m->len;
}

long lume_map_put_i(LumeMap *m, const char *k, long v) { return map_put(m, k, v, NULL, LUME_SLOT_INT); }
long lume_map_put_f(LumeMap *m, const char *k, double v)
{
    long bits;
    memcpy(&bits, &v, sizeof bits);
    return map_put(m, k, bits, NULL, LUME_SLOT_FLOAT);
}
long lume_map_put_s(LumeMap *m, const char *k, const char *v) { return map_put(m, k, 0, v, LUME_SLOT_STR); }

/* Store a nested container as a value -- the map half of lume_list_push_obj.
 * `map_put` is the shared writer; all it needs is the object tag and the
 * pointer in the num field. */
long lume_map_put_obj(LumeMap *m, const char *k, void *v)
{
    return map_put(m, k, (long)(intptr_t)v, NULL, LUME_SLOT_OBJ);
}


/* get() with the interpreter's default: an absent key yields `dflt`. */
long lume_map_get_i(const LumeMap *m, const char *k, long dflt)
{
    long i = map_find(m, k);
    if (i < 0) return dflt;
    return m->vals[i].tag == LUME_SLOT_STR ? 0 : m->vals[i].num;
}
/* Read a nested container back out. Returns null for an absent key or one that
 * does not hold a container -- the caller hands the result straight to another
 * container helper or to a printer, both of which take a null pointer as an
 * empty container. This is what makes `get(m, k, { x: 1 })` and a nested read
 * work: the getter has to give back a pointer, not the int/float/string the
 * other three return.
 *
 * The third parameter exists only so the call site can pass a default the way
 * it does for every other flavour (get() picks its getter from the default's
 * type). A container default is never stored -- an absent key reads as null,
 * which the container helpers already treat as empty. */
void *lume_map_get_obj(const LumeMap *m, const char *k, void *dflt)
{
    (void)dflt;
    long i = map_find(m, k);
    if (i < 0 || m->vals[i].tag != LUME_SLOT_OBJ) return NULL;
    return (void *)(intptr_t)m->vals[i].num;
}

/* Does this Result hold an error? `?` branches on it right after the call and
 * before the payload is read, which is the order the interpreter uses
 * (interp.c's propagate branch looks for "err" first and only then reads "ok").
 * A Result with neither key -- a callee typed any -- counts as no error, so a
 * loose call keeps working rather than propagating something that is not there. */
long lume_map_has(const LumeMap *m, const char *k)
{
    return map_find(m, k) >= 0;
}

double lume_map_get_f(const LumeMap *m, const char *k, double dflt)
{
    long i = map_find(m, k);
    double d = dflt;
    if (i >= 0 && m->vals[i].tag != LUME_SLOT_STR) memcpy(&d, &m->vals[i].num, sizeof d);
    return d;
}
const char *lume_map_get_s(const LumeMap *m, const char *k, const char *dflt)
{
    long i = map_find(m, k);
    if (i < 0) return dflt;
    return m->vals[i].tag == LUME_SLOT_STR ? m->vals[i].str : "";
}

const char *lume_map_key_at(const LumeMap *m, long i)
{
    if (!m || i < 0 || i >= m->len) return "";
    return m->keys[i];
}

/* keys() — the interpreter hands back a *list*, so this builds one and the
 * call site treats the result as a list value. */
LumeList *lume_map_keys(const LumeMap *m)
{
    LumeList *l = lume_list_new();
    if (!m) return l;
    for (long i = 0; i < m->len; i++) lume_list_push_s(l, m->keys[i]);
    return l;
}

long lume_map_print(const LumeMap *m)
{
    if (!m) return emit("{}\n");
    return emit("{") + container_body(m) + emit("}\n");
}

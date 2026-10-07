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
#include <stdio.h>
#include <stdlib.h>
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
     * line, so mixed int/string output stays readable. */
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

typedef struct { long num; const char *str; int tag; } LumeSlot;

typedef struct { long len; long cap; LumeSlot *items; }    LumeList;
typedef struct { long len; long cap; char **keys; LumeSlot *vals; } LumeMap;

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

LumeList *lume_list_new(void) { return rt_alloc(sizeof(LumeList)); }

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

/* Render one slot the way the interpreter would print it. The interpreter's
 * print() falls back to JSON for anything that is not a plain scalar, so a
 * string inside a list or a map keeps its quotes and escapes — `["a","b"]`,
 * not `[a,b]`. The two callers (list elements, map values) consume the
 * result immediately, so a single static buffer is enough. */
static const char *slot_text(LumeSlot s)
{
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
        buf[n < sizeof buf ? n : sizeof buf - 1] = '"';
        return buf;
    }
    case LUME_SLOT_FLOAT: {
        double d = 0;
        memcpy(&d, &s.num, sizeof d);
        snprintf(buf, sizeof buf, "%g", d);
        return buf;
    }
    default:              snprintf(buf, sizeof buf, "%ld", s.num); return buf;
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

long lume_list_print(const LumeList *l)
{
    if (!l) return emit("[]\n");
    int n = emit("[");
    for (long i = 0; i < l->len; i++) {
        if (i) n += emit(",");
        n += emit(slot_text(l->items[i]));
    }
    n += emit("]\n");
    return n;
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

LumeMap *lume_map_new(void) { return rt_alloc(sizeof(LumeMap)); }

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


/* get() with the interpreter's default: an absent key yields `dflt`. */
long lume_map_get_i(const LumeMap *m, const char *k, long dflt)
{
    long i = map_find(m, k);
    if (i < 0) return dflt;
    return m->vals[i].tag == LUME_SLOT_STR ? 0 : m->vals[i].num;
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
    int n = emit("{");
    for (long i = 0; i < m->len; i++) {
        if (i) n += emit(",");
        n += emit("\"");
        n += emit(m->keys[i]);
        n += emit("\":");
        n += emit(slot_text(m->vals[i]));
    }
    n += emit("}\n");
    return n;
}

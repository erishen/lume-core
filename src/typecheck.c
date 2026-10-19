/* Static type checker for Lume. Runs once after parsing, before execution.
 *
 * 2026-09-27: 表达式检查(ck_expr)与语句/函数体检查(ck_stmt/ck_blk/ck_fn)
 * 分别拆到 typecheck_expr.c / typecheck_stmt.c;Checker 与 scope 结构、
 * 跨片共享函数见 typecheck_internal.h。
 *
 * Design:
 *   - int / float / string / bool / null / Result are primitives (TY_*).
 *   - `T[]` is a list type; `type Name = { f: T, ... }` declares structs.
 *   - Unannotated parameters/returns/handlers are TY_ANY ("loose") and are
 *     never constrained; this is what lets route/tool callbacks accept the
 *     framework's request objects.
 *   - All types are nullable: null is assignable everywhere.
 *   - `{ ok: ... }` / `{ err: ... }` single-key maps are Result literals.
 *   - `call()?` needs the callee to be Result-typed and the enclosing
 *     function to return Result (or be untyped), and never at top level.
 *   - int -> float widens implicitly; nothing else does.
 *   - `if`/`while`/`and`/`or`/`not` require bool (strict, no JS truthiness).
 *
 * It stops at the first error (errbuf holds a readable message, like the
 * lexer/parser do).
 */

#include "typecheck_internal.h"

/* ===================== type representation ===================== */

static Type ANY_TYPE = {.kind = TY_ANY};

Type *any_type(void) { return &ANY_TYPE; }

/* Every Type this file allocates lands on one reclaim list (see
 * type_release_all). The parser builds them too, through the same
 * constructors, which is why the list is file-wide rather than per-Checker:
 * a Type produced while parsing is still a Type nobody owns in turn. */
static Type *ty_allocs;

static void ty_keep(Type *t) {
    t->tnext = ty_allocs;
    ty_allocs = t;
}

Type *type_prim(TypeKind kind) {
    Type *t = calloc(1, sizeof(Type));
    t->kind = kind;
    ty_keep(t);
    return t;
}

Type *type_list(Type *elem) {
    Type *t = calloc(1, sizeof(Type));
    t->kind = TY_LIST;
    t->elem = elem;
    ty_keep(t);
    return t;
}

Type *type_struct(const char *name) {
    Type *t = calloc(1, sizeof(Type));
    t->kind = TY_STRUCT;
    t->name = strdup(name);
    ty_keep(t);
    return t;
}

Type *type_anon_struct(void) {
    Type *t = calloc(1, sizeof(Type));
    t->kind = TY_STRUCT;
    t->name = NULL;
    ty_keep(t);
    return t;
}

Type *type_enum(const char *name) {
    Type *t = calloc(1, sizeof(Type));
    t->kind = TY_ENUM;
    t->name = strdup(name);
    ty_keep(t);
    return t;
}

Type *type_func(int arity, Type **params, Type *ret) {
    Type *t = calloc(1, sizeof(Type));
    t->kind = TY_FUNC;
    /* Own the param vector: copy it out of the caller (the parser's and the
     * emitters' vectors are arena memory, TODO.md P2 — storing the pointer
     * made type_release_all's free depend on who handed the array in, which
     * the arena deleted the way to tell). The Type graph stays malloc'd and
     * type_release_all frees every t->types unconditionally. */
    t->types = arity > 0 ? malloc((size_t)arity * sizeof(Type *)) : NULL;
    if (t->types && params)
        memcpy(t->types, params, (size_t)arity * sizeof(Type *));
    t->count = arity;
    t->ret = ret;
    ty_keep(t);
    return t;
}

Type *type_result(void) {
    Type *t = calloc(1, sizeof(Type));
    t->kind = TY_RESULT;
    ty_keep(t);
    return t;
}

/* Record what a `return { ok: X }` unwraps to.
 *
 * `?` needs the payload type statically: the emitters cannot read a map field
 * whose layout they do not know, and they refuse to compile rather than guess
 * (SPEC 8.1 #6). The payload is written onto the *enclosing function's*
 * declared Result type -- c->cur_ret -- not onto the fresh object the literal
 * itself produced. That object is what `callee_t->ret` points at for every
 * call of that function, so a payload recorded there is the one a `?` at any
 * call site reads back.
 *
 * Two `return { ok: X }` in one function that disagree are a type error rather
 * than something to paper over: the function has no single payload type, and
 * picking the first would silently mistype whichever call site read the other.
 * Reported only once per function, on the second and later disagreements, so a
 * function with a consistent payload stays quiet. */
void record_ok_payload(Checker *c, Type *lit_result, Type *payload, size_t line) {
    Type *target = (c->cur_ret && c->cur_ret->kind == TY_RESULT) ? c->cur_ret
                                                                 : lit_result;
    if (!target->elem) {
        target->elem = payload;
        return;
    }
    if (target->elem->kind == TY_ANY || payload->kind == TY_ANY) {
        target->elem = any_type();
        return;
    }
    if (target->elem->kind == payload->kind &&
        (!target->elem->name || !payload->name ||
         strcmp(target->elem->name, payload->name) == 0))
        return;   /* the same payload type, said twice */

    if (c->cur_ret == target)
        ck_fail(c, line, "this function returns { ok: ... } of two different "
                         "types (%s and %s), so '?' has no single payload type",
                ty_str(target->elem), ty_str(payload));
    else
        target->elem = any_type();   /* not in a Result function: stay permissive */
}

void type_release_all(void) {
    Type *t = ty_allocs;
    ty_allocs = NULL; /* so a second call is a no-op, not a double free */
    while (t) {
        Type *next = t->tnext;
        free(t->name);
        if (t->names) {
            for (int i = 0; i < t->count; i++) free(t->names[i]);
            free(t->names);
        }
        /* types[] holds Type* (members or params) that are part of this same
         * graph -- the param vector calloc'd by the checker's Pass C and the
         * copies type_func() makes of caller vectors alike -- so the whole
         * shell goes here, not block by block. Since type_func() copies the
         * caller's array (TODO.md P2: the parser's vectors are arena memory
         * now), every t->types is owned by this graph and freed here. */
        free(t->types);
        free(t);
        t = next;
    }
}

void type_add_member(Type *t, const char *name, Type *ty) {
    if (!t || t->kind != TY_STRUCT) return;
    if (t->count == t->cap) {
        t->cap = t->cap ? t->cap * 2 : 4;
        t->names = realloc(t->names, sizeof(char *) * (size_t)t->cap);
        t->types = realloc(t->types, sizeof(Type *) * (size_t)t->cap);
    }
    t->names[t->count] = strdup(name);
    t->types[t->count] = ty;
    t->count++;
}

static void tp_inner(Type *t) {
    switch (t->kind) {
        case TY_NULL:   printf("null"); break;
        case TY_INT:    printf("int"); break;
        case TY_FLOAT:  printf("float"); break;
        case TY_STRING: printf("string"); break;
        case TY_BOOL:   printf("bool"); break;
        case TY_ANY:    printf("any"); break;
        case TY_RESULT: printf("Result"); break;
        case TY_ENUM:
            printf("%s", t->name ? t->name : "enum");
            break;
        case TY_NS:
            printf("module");
            break;
        case TY_LIST:
            tp_inner(t->elem);
            printf("[]");
            break;
        case TY_FUNC:
            printf("func(");
            for (int i = 0; i < t->count; i++) {
                if (i) printf(", ");
                tp_inner(t->types[i]);
            }
            printf(")");
            if (t->ret) { printf(": "); tp_inner(t->ret); }
            break;
        case TY_STRUCT:
            if (t->name) {
                printf("%s", t->name);
            } else {
                printf("{ ");
                for (int i = 0; i < t->count; i++) {
                    if (i) printf(", ");
                    printf("%s: ", t->names[i]);
                    tp_inner(t->types[i]);
                }
                printf(" }");
            }
            break;
    }
}

void type_print(Type *t) {
    if (!t) { printf("?"); return; }
    tp_inner(t);
}

/* Format a type into a static buffer (for error messages). */
const char *ty_str(Type *t) {
    /* A ring of buffers, not one: one diagnostic routinely spells two types
     * ("'==' does not apply to %s and %s"), and with a single static buffer
     * the *second* call cleared what the first had returned — even when the
     * second answered straight from a literal, because the reset at the top
     * runs on every call. The result was an empty type name in the message
     * ("does not apply to  and int") for every container operand. Each call
     * takes the next slot, so a message with a handful of types — including
     * the recursive `int[]` spelling below — stays intact. */
    static char bufs[8][128];
    static unsigned next;
    char *buf = bufs[next++ % 8];
    buf[0] = '\0';
    if (!t || t->kind == TY_ANY) return "any";
    if (t->kind == TY_NULL) return "null";
    if (t->kind == TY_INT) return "int";
    if (t->kind == TY_FLOAT) return "float";
    if (t->kind == TY_STRING) return "string";
    if (t->kind == TY_BOOL) return "bool";
    if (t->kind == TY_RESULT) return "Result";
    if (t->kind == TY_ENUM) return t->name ? t->name : "enum";
    if (t->kind == TY_LIST) {
        /* `int[]` reads better than the raw kind; the element type is only
         * spelled when it is known, since `[]` alone is any[]. The element
         * string is copied out first — ty_str spells into a ring slot, and
         * snprintf with source and destination overlapping is undefined. */
        if (t->elem && t->elem->kind != TY_ANY) {
            char eb[64];
            snprintf(eb, sizeof eb, "%s", ty_str(t->elem));
            snprintf(buf, sizeof bufs[0], "%s[]", eb);
        } else {
            snprintf(buf, sizeof bufs[0], "list");
        }
        return buf;
    }
    if (t->kind == TY_STRUCT)
        return t->name ? t->name : "map";
    if (t->kind == TY_FUNC)
        return "func";
    snprintf(buf, sizeof bufs[0], "<%s>", t->name ? t->name : "?");
    return buf;
}



void ck_fail(Checker *c, size_t line, const char *fmt, ...) {
    if (c->failed) return;
    /* During the collecting walk of Pass D, a diagnostic must not stop the
     * walk: the rest of this body may hold the `return { ok: X }` that a `?`
     * elsewhere needs, and stopping at the first complaint would leave that
     * payload unknown and the operator uncompilable for a reason that has
     * nothing to do with the program. So the error is only *noted* here and
     * replayed after the second walk, which reports it for real. */
    if (c->collecting) {
        c->collect_failed = true;
        return;
    }
    c->failed = true;
    if (!c->errbuf || !c->errbuf_size) return;
    snprintf(c->errbuf, c->errbuf_size, "line %zu: ", line);
    va_list ap;
    va_start(ap, fmt);
    size_t used = strlen(c->errbuf);
    vsnprintf(c->errbuf + used, c->errbuf_size - used, fmt, ap);
    va_end(ap);
}

/* Record an exported binding's signature on the module being checked (a no-op
 * outside module context). Duplicate names are ignored — the duplicate is
 * already rejected by the scope at declaration time. */
void export_add(Checker *c, const char *name, Type *t) {
    if (!c->self || !t) return;
    for (int i = 0; i < c->self->export_types.count; i++)
        if (strcmp(c->self->export_types.names[i], name) == 0) return;
    c->self->export_types.names =
        realloc(c->self->export_types.names,
                sizeof(char *) * ((size_t)c->self->export_types.count + 1));
    c->self->export_types.types =
        realloc(c->self->export_types.types,
                sizeof(Type *) * ((size_t)c->self->export_types.count + 1));
    if (!c->self->export_types.names || !c->self->export_types.types) return;
    c->self->export_types.names[c->self->export_types.count] = strdup(name);
    c->self->export_types.types[c->self->export_types.count] = t;
    c->self->export_types.count++;
}

/* Set while a module is being checked. scope_new() chains every scope it
 * creates onto the active Checker: the parent pointer only runs from an inner
 * scope outwards, so without this list the inner scopes would be unreachable
 * once the outermost check returns and could never be freed. */
static Checker *g_ck_scopes;

CScope *scope_new(CScope *parent) {
    CScope *s = calloc(1, sizeof(CScope));
    s->parent = parent;
    if (g_ck_scopes) {
        s->all_next = g_ck_scopes->all_scopes;
        g_ck_scopes->all_scopes = s;
    }
    return s;
}

/* Release the checker's own bookkeeping: the scope list with its symbol and
 * namespace chains, and the struct table. The Type* those point at is not
 * ours to free here — that is type_release_all()'s sweep. */
static void ck_free(Checker *c) {
    CScope *s = c->all_scopes;
    while (s) {
        CScope *sn = s->all_next;   /* read before s dies */
        CSym *it = s->syms;
        while (it) { CSym *nxt = it->next; free(it->name); free(it); it = nxt; }
        CNS *ns = s->nss;
        while (ns) { CNS *nxt = ns->next; free(ns->name); free(ns); ns = nxt; }
        free(s);
        s = sn;
    }
    StructDef *d = c->structs;
    while (d) {
        StructDef *dn = d->next;
        free(d->name);
        free(d);
        d = dn;
    }
    c->all_scopes = NULL;
    c->structs = NULL;
    EnumDef *e = c->enums;
    while (e) {
        EnumDef *en = e->next;
        for (int j = 0; j < e->vcount; j++) {
            free(e->vnames[j]);
            /* vfields[j]/vfield_types[j] shells: the field-name strings live
             * in the parse arena; the Type* payloads
             * belong to type_release_all(). Only the shells die here. */
            free(e->vfields[j]);
            free(e->vfield_types[j]);
        }
        free(e->vnames);
        free(e->vfields);
        free(e->vfield_types);
        free(e->vfield_counts);
        free(e->name);
        free(e);
        e = en;
    }
    c->enums = NULL;
}

void scope_put(CScope *s, const char *name, Type *t) {
    for (CSym *it = s->syms; it; it = it->next) {
        if (strcmp(it->name, name) == 0) { it->type = t; return; }
    }
    CSym *sym = calloc(1, sizeof(CSym));
    sym->name = strdup(name);
    sym->type = t;
    sym->next = s->syms;
    s->syms = sym;
}

/* 声明入口: 同层重名直接编译错(用户声明之间)。历史上 let/变量与 func 进
 * 同一个 CScope, scope_put 覆盖式写入——`let crm_lock` + `export func
 * crm_lock()` 编译全绿、运行时按声明顺序静默覆盖, 只在 fork worker 里炸
 * (最坏一类 bug)。只查当前层不沿 parent 链, 内层遮蔽(shadowing)照常
 * 允许; 与内置函数重名也放行(用户声明遮蔽内置名是既有合法用法, 如
 * `let type = "x"`)。 */
/* Single source of truth for the builtin name list. The two consumers below —
 * the is_builtin_name() guard and the loose scope seeding in type_check_init —
 * each used to carry their own copy, so adding a builtin meant editing two
 * lists that nobody checked against each other. */
const char *const LUME_BUILTIN_NAMES[] = {
    "run", "print", "str", "int", "len", "keys", "get",
    /* SQLite (2026-10-12, builtins_sql.c): registered in every build;
     * without libsqlite3 the runtime stub reports the missing dep. */
    "sql_query", "sql_write",
    "json", "stringify", "now", "el", "render", "html",
    "float", "bool", "string", "type", "Result", /* type words usable as idents */
    "write", "read", /* built-in verb groups (see seed_verb_groups) */
    "env", "argv", "files", "read_file", "read_stdin", "write_file", "mkdir", "strftime", "put",
    "range", "map", "filter", "reduce", /* collection tools */
    /* sql_query / sql_write: host build only. */
    "lock_file", "unlock_file", /* flock advisory lock (invest ledger) */
    "tools", "skills", "mcps",
    "discovery_endpoints", "catalog", /* discovery builtins */
    "push", "try", /* collection / error handling (2026-09-27) */
    "sort", "str_match", "regex", /* collection / string search (2026-10-11) */
    "replace", /* string builtins (2026-09-27) */
    "upper", "lower", "capitalize", "trim", "contains", "split", "join",
    "substr", /* string tools (2026-10-08, P4a codegen) */
    "crypt_sha512", /* sha512 crypt hash (2026-09-29) */
    "sha256", "hmac_sha256", /* message digests, hex out (2026-10-15, payment signing) */
    "base64_encode", "base64_decode", /* RFC 4648, binary-safe (2026-10-16, PayPal OAuth2 Basic) */
    /* math builtins (2026-09-28, builtins_math.c) */
    "abs", "sqrt", "exp", "log", "ln", "pow", "floor", "ceil", "round",
    "min", "max", "pi", "e",
    /* outbound HTTP (2026-10-04, builtins_http.c) */
    "http_get",
    /* HTTP verbs that carry a body (2026-10-05) */
    "http_post", "http_put", "http_patch",
    "http_delete", /* same as http_get: no body */
};

const size_t LUME_BUILTIN_COUNT =
    sizeof(LUME_BUILTIN_NAMES) / sizeof(LUME_BUILTIN_NAMES[0]);

bool is_builtin_name(const char *name) {
    for (size_t i = 0; i < LUME_BUILTIN_COUNT; i++)
        if (strcmp(LUME_BUILTIN_NAMES[i], name) == 0) return true;
    return false;
}

void scope_decl(Checker *c, CScope *s, const char *name, Type *t, size_t line) {
    if (!is_builtin_name(name)) {
        for (CSym *it = s->syms; it; it = it->next) {
            if (strcmp(it->name, name) == 0) {
                ck_fail(c, line,
                        "duplicate declaration of '%s' in the same scope", name);
                return;
            }
        }
    }
    scope_put(s, name, t);
}

Type *scope_get(CScope *s, const char *name) {
    for (CScope *sc = s; sc; sc = sc->parent) {
        for (CSym *it = sc->syms; it; it = it->next)
            if (strcmp(it->name, name) == 0) return it->type;
    }
    return NULL;
}

void scope_put_ns(CScope *s, const char *name, Module *m) {
    CNS *ns = calloc(1, sizeof(CNS));
    ns->name = strdup(name);
    ns->mod = m;
    ns->next = s->nss;
    s->nss = ns;
}

Module *scope_get_ns(CScope *s, const char *name) {
    for (CScope *sc = s; sc; sc = sc->parent) {
        for (CNS *it = sc->nss; it; it = it->next)
            if (strcmp(it->name, name) == 0) return it->mod;
    }
    return NULL;
}

StructDef *find_struct(Checker *c, const char *name) {
    for (StructDef *d = c->structs; d; d = d->next)
        if (strcmp(d->name, name) == 0) return d;
    return NULL;
}

void add_struct(Checker *c, const char *name, Type *t) {
    StructDef *d = calloc(1, sizeof(StructDef));
    d->name = strdup(name);
    d->type = t;
    d->next = c->structs;
    c->structs = d;
}

EnumDef *find_enum(Checker *c, const char *name) {
    for (EnumDef *d = c->enums; d; d = d->next)
        if (strcmp(d->name, name) == 0) return d;
    return NULL;
}

void add_enum(Checker *c, EnumDef *d) {
    d->next = c->enums;
    c->enums = d;
}

/* Two payload types are "the same" for or-pattern binds: same kind, and
 * for the named kinds the same name. Deliberately stricter than
 * type_compat — an or-pattern arm has ONE binding set, so `A(x: int) |
 * B(x: float)` is rejected rather than silently widened (write two arms). */
static bool pat_types_same(Type *a, Type *b) {
    if (!a || !b) return true;              /* unstamped: nothing to compare */
    if (a->kind != b->kind) return false;
    if (a->name || b->name) return a->name && b->name &&
                                  strcmp(a->name, b->name) == 0;
    return true;
}

/* Validate one pattern list — a match arm's or-pattern, or an if-let's.
 *  - every pattern must name a variant of `ed` and bind exactly its
 *    payload count (skipped when ed is NULL: an untyped scrutinee, where
 *    nothing can be validated);
 *  - `_` binds nothing;
 *  - no variant twice inside the list;
 *  - the alternatives bind the same names with the same types.
 * Returns pats[0].bindc — the arm's single binding-set size. */
int ck_pat_list(Checker *c, MatchPat *pats, int patc, EnumDef *ed,
                size_t line) {
    for (int pi = 0; pi < patc; pi++) {
        MatchPat *pat = &pats[pi];
        if (strcmp(pat->vname, "_") == 0) {
            if (pat->bindc > 0)
                ck_fail(c, line, "the '_' match pattern binds no payload");
            continue;
        }
        if (ed) {
            int vi = -1;
            for (int j = 0; j < ed->vcount; j++)
                if (strcmp(ed->vnames[j], pat->vname) == 0) { vi = j; break; }
            if (vi < 0) {
                ck_fail(c, line, "enum '%s' has no variant '%s'",
                        ed->name, pat->vname);
                continue;
            }
            if (ed->vfield_counts[vi] != pat->bindc)
                ck_fail(c, line,
                        "variant '%s' carries %d payload field(s), "
                        "arm binds %d",
                        pat->vname, ed->vfield_counts[vi], pat->bindc);
        }
        for (int pj = 0; pj < pi; pj++)
            if (strcmp(pats[pj].vname, pat->vname) == 0)
                ck_fail(c, line, "duplicate match pattern '%s'", pat->vname);
    }
    if (patc > 1) {
        for (int pi = 1; pi < patc; pi++) {
            if (pats[pi].bindc != pats[0].bindc)
                ck_fail(c, line,
                        "or-pattern alternatives must bind the same names "
                        "(%d vs %d)", pats[0].bindc, pats[pi].bindc);
            for (int k = 0; k < pats[0].bindc && k < pats[pi].bindc; k++) {
                if (strcmp(pats[pi].binds[k], pats[0].binds[k]) != 0)
                    ck_fail(c, line,
                            "or-pattern alternatives must bind the same "
                            "names ('%s' vs '%s')",
                            pats[0].binds[k], pats[pi].binds[k]);
                Type *t0 = pats[0].vfield_types ? pats[0].vfield_types[k]
                                                : NULL;
                Type *t1 = pats[pi].vfield_types ? pats[pi].vfield_types[k]
                                                 : NULL;
                if (!pat_types_same(t0, t1))
                    ck_fail(c, line,
                            "or-pattern binding '%s' has conflicting types "
                            "('%s' vs '%s')",
                            pats[0].binds[k], ty_str(t0), ty_str(t1));
            }
        }
    }
    return pats[0].bindc;
}

/* Declare an arm's (already validated) binding set in the CURRENT scope —
 * the caller has already switched to a fresh child scope. Types come from
 * the first alternative's stamp; an untyped scrutinee or an unstamped
 * variant degrades to any. The first (collecting) walk uses scope_put —
 * the second walk re-declares. */
void ck_put_pat_binds(Checker *c, MatchPat *pats, int bindc, size_t line) {
    /* bindc is only an upper bound: both validators that could correct it
     * (ck_pat_list's arity clamp, the or-pattern same-names check) merely
     * record their complaint, so on error paths it can exceed pats[0]'s
     * stamped arrays — binds[] is exactly pats[0].bindc long and
     * vfield_types[] is the resolve_ctors full-variant stamp (vfieldc).
     * Skipping the excess is sound: every path that lands here has already
     * failed the run with a precise diagnostic. */
    if (bindc > pats[0].bindc) bindc = pats[0].bindc;
    if (pats[0].vfield_types && bindc > pats[0].vfieldc)
        bindc = pats[0].vfieldc;
    for (int k = 0; k < bindc; k++) {
        Type *pt = pats[0].vfield_types ? pats[0].vfield_types[k]
                                        : any_type();
        if (c->collecting)
            scope_put(c->scope, pats[0].binds[k], pt);
        else
            scope_decl(c, c->scope, pats[0].binds[k], pt, line);
    }
}

/* A constructor name resolves to its enum + variant. Linear over every
 * variant of every enum — the tables are file-sized, that is fine. */
EnumDef *find_ctor(Checker *c, const char *name, int *vidx) {
    for (EnumDef *d = c->enums; d; d = d->next)
        for (int i = 0; i < d->vcount; i++)
            if (strcmp(d->vnames[i], name) == 0) {
                if (vidx) *vidx = i;
                return d;
            }
    return NULL;
}

/* A variant name is a constructor file-wide (parser.c rewrites every bare
 * use), so a let/param/func with the same name would be silently unreachable
 * — the rewrite would win. Reject the declaration instead of letting the
 * name mean two things. */
void ck_ctor_shadow(Checker *c, const char *name, size_t line) {
    if (find_ctor(c, name, NULL))
        ck_fail(c, line,
                "'%s' shadows an enum constructor (variant names are "
                "reserved across the file)", name);
}

/* Resolve a named struct reference to its definition (or TY_ANY). */
Type *resolve(Checker *c, Type *t, size_t line) {
    if (!t) return any_type();
    if (t->kind == TY_STRUCT && t->name) {
        StructDef *d = find_struct(c, t->name);
        if (!d) {
            EnumDef *e = find_enum(c, t->name);
            if (e) return e->type;
            /* Types exported by an imported module are already fully formed
             * (the dependency is type-checked before the importer), so they
             * are usable directly even though they are not registered in
             * this file's struct table. */
            if (t->count > 0) return t;
            ck_fail(c, line, "unknown type '%s'", t->name);
            return any_type();
        }
        return d->type;
    }
    return t;
}

/* Is a value of type `src` assignable to a slot of type `dst`? */
bool type_compat(Checker *c, Type *src, Type *dst, size_t line) {
    if (!src || src->kind == TY_ANY) return true;
    if (!dst || dst->kind == TY_ANY) return true;
    if (src->kind == TY_NULL) return true;          /* all types nullable */
    if (src->kind == TY_INT && dst->kind == TY_FLOAT) return true; /* widen */
    if (src->kind != dst->kind) {
        /* A TY_STRUCT shell is what the parser produces for every ident
         * type; after resolve() the enum side is TY_ENUM. If the kinds
         * disagree but the names agree, one of the two is an unresolved
         * shell spelling the same declared type — accept it by name. */
        bool s_named = (src->kind == TY_ENUM || src->kind == TY_STRUCT) && src->name;
        bool d_named = (dst->kind == TY_ENUM || dst->kind == TY_STRUCT) && dst->name;
        if (s_named && d_named && strcmp(src->name, dst->name) == 0)
            return true;
        return false;
    }

    switch (src->kind) {
        case TY_LIST:
            return type_compat(c, src->elem, dst->elem, line);
        case TY_ENUM:
            return src->name && dst->name &&
                   strcmp(src->name, dst->name) == 0;
        case TY_STRUCT: {
            if (src->name && dst->name)
                return strcmp(src->name, dst->name) == 0;
            /* need field shape comparison */
            if (src->count != dst->count) return false;
            for (int i = 0; i < src->count; i++) {
                int j = -1;
                for (int k = 0; k < dst->count; k++)
                    if (strcmp(src->names[i], dst->names[k]) == 0) { j = k; break; }
                if (j < 0 || !type_compat(c, src->types[i], dst->types[j], line))
                    return false;
            }
            return true;
        }
        case TY_FUNC: {
            if (src->count != dst->count) return false;
            for (int i = 0; i < src->count; i++)
                if (!type_compat(c, src->types[i], dst->types[i], line))
                    return false;
            return type_compat(c, src->ret, dst->ret, line);
        }
        default:
            return true; /* equal primitive kinds */
    }
}

/* Does an expression evaluate in a boolean context? */
bool is_bool_ok(Checker *c, Type *t, size_t line) {
    if (!t || t->kind == TY_ANY) return true;
    if (t->kind == TY_BOOL) return true;
    ck_fail(c, line, "expected bool, got %s", ty_str(t));
    return false;
}

/* Wire `expected` into the returned type: a value of type `actual` must be
 * assignable to an `expected` slot. Same rule as N_LET/N_ASSIGN/N_RETURN,
 * so call args, list elements and struct fields get static guarantees too
 * instead of a generic runtime error. ANY on either side stays loose. */
void expect_compat(Checker *c, Type *actual, Type *expected,
                          size_t line, const char *what) {
    if (c->failed) return;
    if (!expected || expected->kind == TY_ANY) return;
    if (!actual || actual->kind == TY_ANY) return;
    if (!type_compat(c, actual, expected, line))
        ck_fail(c, line,
                "%s: cannot use a value of type %s where %s is expected",
                what, ty_str(actual), ty_str(expected));
}

/* Static statement in a type; combines ARITH with the widening rule so
 * `1 + 2` is int but `1 + 2.5` is float. */
Type *arith_result(Type *a, Type *b) {
    if (!a || a->kind == TY_ANY || !b || b->kind == TY_ANY) return any_type();
    if (a->kind == TY_FLOAT || b->kind == TY_FLOAT) {
        Type *f = type_prim(TY_FLOAT);
        return f;
    }
    Type *i = type_prim(TY_INT);
    return i;
}
/* ===================== program entry ===================== */

/* Module-aware entry (loader.c): `self` is the module being checked, `mods`
 * the loader registry. Plain single-file checks pass NULL/NULL/0. */
bool type_check_module(struct Module *self, struct Module **mods, int mod_count,
                       Node *prog, char *errbuf, size_t errbuf_size) {
    if (!prog || prog->type != N_PROGRAM) return true;

    /* Local functions first: stamp cnames and rewrite the call sites inside
     * each enclosing function, so every pass below sees the hoisted names
     * (SPEC §4.14). Idempotent; the native backends re-run it as a guard. */
    if (!lume_resolve_localfns(prog, errbuf, errbuf_size)) return false;

    Checker c;
    memset(&c, 0, sizeof(c));
    if (errbuf && errbuf_size) errbuf[0] = '\0';
    c.errbuf = errbuf;
    c.errbuf_size = errbuf_size;
    /* Up first: scope_new() links onto this Checker, and the very first scope
     * is created two lines below. Setting g_ck_scopes afterwards would leave
     * the root scope off c->all_scopes, so ck_free() would skip the symbols
     * declared at the top level of the script. */
    g_ck_scopes = &c; /* scope_new() chains onto this Checker */
    c.scope = scope_new(NULL);
    c.self = self;
    c.mods = mods;
    c.mod_count = mod_count;

    /* builtins are loose */
    for (size_t i = 0; i < sizeof(LUME_BUILTIN_NAMES) / sizeof(LUME_BUILTIN_NAMES[0]); i++)
        scope_put(c.scope, LUME_BUILTIN_NAMES[i], any_type());

    /* Pass A: register every struct name before resolving any body, so
     * forward references between structs work. The def Type keeps the name:
     * named-struct strict checking keys off `TY_STRUCT.name`. */
    for (int i = 0; i < prog->as.program.count; i++) {
        Node *s = prog->as.program.stmts[i];
        if (s->type == N_TYPE_DECL) {
            if (find_struct(&c, s->as.type_decl.name)) {
                ck_fail(&c, s->line, "type '%s' already defined",
                        s->as.type_decl.name);
                break;
            }
            Type *def = type_anon_struct();
            def->name = strdup(s->as.type_decl.name);
            add_struct(&c, s->as.type_decl.name, def);
        }
    }

    /* Pass A2: register every enum name the same way (same namespace as
     * structs — a file cannot declare `type X` and `enum X`). */
    for (int i = 0; i < prog->as.program.count && !c.failed; i++) {
        Node *s = prog->as.program.stmts[i];
        if (s->type != N_ENUM_DECL) continue;
        if (find_enum(&c, s->as.enum_decl.name) ||
            find_struct(&c, s->as.enum_decl.name)) {
            ck_fail(&c, s->line, "type '%s' already defined",
                    s->as.enum_decl.name);
            break;
        }
        EnumDef *d = calloc(1, sizeof(EnumDef));
        d->name = strdup(s->as.enum_decl.name);
        d->vcount = s->as.enum_decl.vcount;
        d->vnames = calloc((size_t)d->vcount + 1, sizeof(char *));
        d->vfields = calloc((size_t)d->vcount + 1, sizeof(char **));
        d->vfield_types = calloc((size_t)d->vcount + 1, sizeof(Type **));
        d->vfield_counts = calloc((size_t)d->vcount + 1, sizeof(int));
        for (int j = 0; j < d->vcount; j++) {
            d->vnames[j] = strdup(s->as.enum_decl.vnames[j]);
            d->vfield_counts[j] = s->as.enum_decl.vfield_counts[j];
        }
        d->type = type_enum(d->name);
        add_enum(&c, d);
    }

    /* Pass B: fill struct fields. */
    if (!c.failed) {
        for (int i = 0; i < prog->as.program.count && !c.failed; i++) {
            Node *s = prog->as.program.stmts[i];
            if (s->type != N_TYPE_DECL) continue;
            Type *def = find_struct(&c, s->as.type_decl.name)->type;
            for (int j = 0; j < s->as.type_decl.count; j++)
                type_add_member(def, s->as.type_decl.field_names[j],
                                resolve(&c, s->as.type_decl.field_types[j],
                                        s->line));
            if (s->is_export)
                export_add(&c, s->as.type_decl.name, def);
        }
    }

    /* Pass B2: resolve enum payload types. Only the four scalars are
     * allowed: the value is a runtime map, and a named-struct payload would
     * have to travel as an aggregate through a map slot the emitters spell
     * as i8* — that needs an explicit boxing decision before it can be
     * more than a silent mismatch waiting to happen. */
    if (!c.failed) {
        for (int i = 0; i < prog->as.program.count && !c.failed; i++) {
            Node *s = prog->as.program.stmts[i];
            if (s->type != N_ENUM_DECL) continue;
            EnumDef *d = find_enum(&c, s->as.enum_decl.name);
            for (int j = 0; j < s->as.enum_decl.vcount && !c.failed; j++) {
                int fc = s->as.enum_decl.vfield_counts[j];
                if (fc == 0) continue;
                d->vfields[j] = calloc((size_t)fc + 1, sizeof(char *));
                d->vfield_types[j] = calloc((size_t)fc + 1, sizeof(Type *));
                for (int k = 0; k < fc && !c.failed; k++) {
                    d->vfields[j][k] = s->as.enum_decl.vfields[j][k];
                    Type *ft = resolve(&c, s->as.enum_decl.vfield_types[j][k],
                                       s->line);
                    TypeKind k2 = ft ? ft->kind : TY_ANY;
                    if (k2 != TY_INT && k2 != TY_FLOAT && k2 != TY_STRING &&
                        k2 != TY_BOOL && k2 != TY_ANY)
                        ck_fail(&c, s->line,
                                "enum payload field '%s' must be int, float, "
                                "string or bool (got %s)",
                                d->vfields[j][k], ty_str(ft));
                    d->vfield_types[j][k] = ft;
                }
            }
        }
    }

    /* Pass C: register function signatures (two passes so mutual recursion
     * between functions resolves). Local functions register too, under their
     * synthetic cname: the call sites inside the enclosing function were
     * rewritten to that name, and the source name exists in no scope at all,
     * so a call from anywhere else is an undefined variable — the scoping a
     * local declaration should have (SPEC §4.14). */
    {
        int nfn = lume_collect_funcs(prog, NULL, 0);
        Node **allfns = nfn > 0 ? malloc(sizeof(Node *) * (size_t)nfn) : NULL;
        if (nfn > 0) lume_collect_funcs(prog, allfns, nfn);
        for (int i = 0; i < nfn && !c.failed; i++) {
            Node *s = allfns[i];
            const char *fname = s->as.func.cname ? s->as.func.cname
                                                 : s->as.func.name;
            ck_ctor_shadow(&c, fname, s->line);
            Type **params = calloc((size_t)s->as.func.arity + 1, sizeof(Type *));
            for (int j = 0; j < s->as.func.arity; j++) {
                params[j] = s->as.func.param_types[j]
                                ? resolve(&c, s->as.func.param_types[j], s->line)
                                : any_type();
                /* Write the resolved type back: the native emitters read the
                 * AST's param_types directly, and a TY_STRUCT shell spelled
                 * `Shape` must not be mistaken for a struct aggregate when the
                 * name is an enum (the emitters have no checker tables). Only
                 * annotated slots are written — NULL means "left to inference"
                 * and the signature pass keys off that. */
                if (s->as.func.param_types[j])
                    s->as.func.param_types[j] = params[j];
            }
            Type *ret = s->as.func.ret ? resolve(&c, s->as.func.ret, s->line)
                                       : any_type();
            if (s->as.func.ret)
                s->as.func.ret = ret;   /* same write-back, for the emitters */
            scope_decl(&c, c.scope, fname,
                       type_func(s->as.func.arity, params, ret), s->line);
            /* Only a top-level function can be exported; a local's export
             * flag is never set by the parser, and its cname must not leak
             * into the module surface anyway. */
            if (s->is_export && !s->as.func.cname)
                export_add(&c, s->as.func.name,
                           scope_get(c.scope, fname));
        }
        free(allfns);
    }

    /* Pass D: walk everything (function bodies included).
     *
     * Run twice, and the reason is `?`. A call `f()?` needs f's payload type,
     * which is only knowable from the `return { ok: X }` statements inside f --
     * and those may sit *after* the call in source order. One walk would leave
     * every payload unknown for a forward reference, and the emitters cannot
     * compile `?` against an unknown type (SPEC 8.1 #6), so the first walk is
     * there to fill the payloads in and the second is the one that reports.
     *
     * Walking twice is safe because the second walk can only find *more*: every
     * diagnostic the first one produced is produced again, and the loop below
     * returns before the second walk starts if the first failed. A payload can
     * make a previously-untyped `?` typed, which can surface a genuine type
     * error that the first walk (seeing `any` there) could not -- and that is
     * the point of doing it at all.
     *
     * Only function bodies matter for the collection, and ck_stmt already
     * walks them via ck_fn with the params bound, so no separate walker is
     * needed -- which is also what keeps the two walks seeing identical scopes. */
    for (int pass = 0; pass < 2 && !c.failed; pass++) {
        bool collect = (pass == 0);
        /* The collecting walk gets a scope of its own, parented on the one
         * holding the builtins, so that nothing it declares can be seen by the
         * walk that reports. Sharing one scope made the second walk re-declare
         * every top-level `let` into a scope the first walk had already bound,
         * which scope_decl correctly rejects as a duplicate -- a diagnostic
         * about the checker rather than about the program. Its bindings are
         * thrown away with it; only the Result payloads it records on the
         * function types survive, and those live on the Type objects, not here. */
        CScope *outer = c.scope;
        if (collect) c.scope = scope_new(outer);
        for (int i = 0; i < prog->as.program.count && !c.failed; i++)
            ck_stmt_mode(&c, prog->as.program.stmts[i], collect);
        if (collect) c.scope = outer;
    }

    g_ck_scopes = NULL;
    ck_free(&c);
    return !c.failed;
}

bool type_check_program(Node *prog, char *errbuf, size_t errbuf_size) {
    return type_check_module(NULL, NULL, 0, prog, errbuf, errbuf_size);
}
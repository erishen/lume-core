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

Type *type_prim(TypeKind kind) {
    Type *t = calloc(1, sizeof(Type));
    t->kind = kind;
    return t;
}

Type *type_list(Type *elem) {
    Type *t = calloc(1, sizeof(Type));
    t->kind = TY_LIST;
    t->elem = elem;
    return t;
}

Type *type_struct(const char *name) {
    Type *t = calloc(1, sizeof(Type));
    t->kind = TY_STRUCT;
    t->name = strdup(name);
    return t;
}

Type *type_anon_struct(void) {
    Type *t = calloc(1, sizeof(Type));
    t->kind = TY_STRUCT;
    t->name = NULL;
    return t;
}

Type *type_func(int arity, Type **params, Type *ret) {
    Type *t = calloc(1, sizeof(Type));
    t->kind = TY_FUNC;
    t->types = params;
    t->count = arity;
    t->ret = ret;
    return t;
}

Type *type_result(void) {
    Type *t = calloc(1, sizeof(Type));
    t->kind = TY_RESULT;
    return t;
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
    static char buf[128];
    buf[0] = '\0';
    if (!t || t->kind == TY_ANY) return "any";
    if (t->kind == TY_NULL) return "null";
    if (t->kind == TY_INT) return "int";
    if (t->kind == TY_FLOAT) return "float";
    if (t->kind == TY_STRING) return "string";
    if (t->kind == TY_BOOL) return "bool";
    if (t->kind == TY_RESULT) return "Result";
    snprintf(buf, sizeof(buf), "<%s>", t->name ? t->name : "?");
    return buf;
}



void ck_fail(Checker *c, size_t line, const char *fmt, ...) {
    if (c->failed) return;
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

CScope *scope_new(CScope *parent) {
    CScope *s = calloc(1, sizeof(CScope));
    s->parent = parent;
    return s;
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
bool is_builtin_name(const char *name) {
    static const char *const BUILTINS[] = {
        "run", "print", "str", "int", "len", "keys", "get",
        "json", "stringify", "now", "el", "render", "html",
        "float", "bool", "string", "type", "Result", /* type words usable as idents */
        "write", "read", /* built-in verb groups (see seed_verb_groups) */
        "env", "files", "read_file", "write_file", "mkdir", "strftime", "put",
        "range", "map", "filter", "reduce", /* collection tools */
        "sql_query", "sql_write", /* sqlite builtins (DSL-level) */
        "lock_file", "unlock_file", /* flock advisory lock (invest ledger) */
        "tools", "skills", "mcps",
        "discovery_endpoints", "catalog", /* discovery builtins */
        "push", "try", /* collection / error handling (2026-09-27) */
        "replace", /* string builtins (2026-09-27) */
        "crypt_sha512", /* sha512 crypt hash (2026-09-29) */
        /* math builtins (2026-09-28, builtins_math.c) */
        "abs", "sqrt", "exp", "log", "ln", "pow", "floor", "ceil", "round",
        "min", "max", "pi", "e",
    };
    for (size_t i = 0; i < sizeof(BUILTINS) / sizeof(BUILTINS[0]); i++)
        if (strcmp(BUILTINS[i], name) == 0) return true;
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

/* Resolve a named struct reference to its definition (or TY_ANY). */
Type *resolve(Checker *c, Type *t, size_t line) {
    if (!t) return any_type();
    if (t->kind == TY_STRUCT && t->name) {
        StructDef *d = find_struct(c, t->name);
        if (!d) {
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
    if (src->kind != dst->kind) return false;

    switch (src->kind) {
        case TY_LIST:
            return type_compat(c, src->elem, dst->elem, line);
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

    Checker c;
    memset(&c, 0, sizeof(c));
    if (errbuf && errbuf_size) errbuf[0] = '\0';
    c.errbuf = errbuf;
    c.errbuf_size = errbuf_size;
    c.scope = scope_new(NULL);
    c.self = self;
    c.mods = mods;
    c.mod_count = mod_count;

    /* builtins are loose */
    {
        static const char *BUILTINS[] = {
            "run", "print", "str", "int", "len", "keys", "get",
            "json", "stringify", "now", "el", "render", "html",
            "float", "bool", "string", "type", "Result", /* type words usable as idents */
            "write", "read", /* built-in verb groups (see seed_verb_groups) */
            "env", "files", "read_file", "write_file", "mkdir", "strftime", "put",
            "range", "map", "filter", "reduce", /* collection tools */
            "sql_query", "sql_write", /* sqlite builtins (DSL-level) */
            "lock_file", "unlock_file", /* flock advisory lock (invest ledger) */
            "push", "try", /* 列表追加 / 错误捕获 (2026-09-27) */
            "replace", /* 字符串内建 (2026-09-27) */
            "crypt_sha512", /* sha512 crypt hash (2026-09-29) */
            /* math builtins (2026-09-28) */
            "abs", "sqrt", "exp", "log", "ln", "pow", "floor", "ceil", "round",
            "min", "max", "pi", "e",
            "tools", "skills", "mcps",
            "discovery_endpoints", "catalog", /* discovery builtins */
        };
        for (size_t i = 0; i < sizeof(BUILTINS) / sizeof(BUILTINS[0]); i++)
            scope_put(c.scope, BUILTINS[i], any_type());
    }

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

    /* Pass C: register function signatures (two passes so mutual recursion
     * between functions resolves). */
    for (int i = 0; i < prog->as.program.count && !c.failed; i++) {
        Node *s = prog->as.program.stmts[i];
        if (s->type != N_FUNC_DECL) continue;
        Type **params = calloc((size_t)s->as.func.arity + 1, sizeof(Type *));
        for (int j = 0; j < s->as.func.arity; j++)
            params[j] = s->as.func.param_types[j]
                            ? resolve(&c, s->as.func.param_types[j], s->line)
                            : any_type();
        Type *ret = s->as.func.ret ? resolve(&c, s->as.func.ret, s->line)
                                   : any_type();
        scope_decl(&c, c.scope, s->as.func.name,
                   type_func(s->as.func.arity, params, ret), s->line);
        if (s->is_export)
            export_add(&c, s->as.func.name,
                       scope_get(c.scope, s->as.func.name));
    }

    /* Pass D: walk everything (function bodies included). */
    for (int i = 0; i < prog->as.program.count && !c.failed; i++)
        ck_stmt(&c, prog->as.program.stmts[i]);

    return !c.failed;
}

bool type_check_program(Node *prog, char *errbuf, size_t errbuf_size) {
    return type_check_module(NULL, NULL, 0, prog, errbuf, errbuf_size);
}
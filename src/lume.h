#ifndef LUME_H
#define LUME_H

/* Lume — a strongly-typed scripting DSL whose runtime drives
 * libagenthttpd.a (the embeddable agent-httpd server library).
 *
 * Pipeline: lexer -> parser(AST) -> static type checker -> tree-walk
 * interpreter(Runtime) -> bridge(translates routes/tools/config into the
 * agenthttpd embedding API).
 *
 * Types: int, float, string, bool, null, Result (no user generics). All
 * types are nullable. `type Name = { field: type, ... }` declares structs;
 * `T[]` is a list type. Annotations are optional (`let x = 5` infers int);
 * unannotated function parameters are loose (drives route/tool callbacks).
 * `call()?` on a Result-typed call propagates the `{ err: ... }` case up
 * the call stack (Rust-style `?`).
 *
 * Every object lives in a mark-sweep GC heap; strings are immutable.
 * The interpreter maintains an explicit value stack so every temporary is a
 * GC root (clox-style) — a Value returned by eval is held by the caller,
 * so an allocation later in the same expression can never reclaim it.
 *
 * Host language is C11, no third-party deps beyond libagenthttpd.a + libc.
 */

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <setjmp.h>

/* This tree is the standalone-language build. The host build (lume)
 * pulls in agenthttpd.h here, which is what brought tools.h / skills.h /
 * sqlite_tool.h / minijson.h along with it; this tree links nothing but libc,
 * so there is no agent-httpd include to make.
 *
 * The one thing that used to ride in on minijson.h is the sbuf string buffer
 * (json_append_value and friends take a `void *sbuf`). sbuf.h is the fork-local
 * stand-in: same struct, same always-NUL-terminated + sticky-oom contract,
 * only the append half, inlined. */

#include "sbuf.h"

/* ===================== tokens ===================== */

typedef enum {
    TOK_EOF, TOK_ERROR,

    TOK_IDENT, TOK_NUMBER, TOK_STRING, TOK_ISTR,

    /* keywords */
    TOK_SERVER, TOK_ROUTE, TOK_TOOL, TOK_FUNC, TOK_RETURN,
    TOK_IF, TOK_ELSE, TOK_WHILE, TOK_FOR, TOK_IN, TOK_BREAK, TOK_CONTINUE,
    TOK_LET,
    TOK_IMPORT, TOK_EXPORT, TOK_AS,
    TOK_TRUE, TOK_FALSE, TOK_NULL, TOK_AND, TOK_OR, TOK_NOT,
    TOK_TYPE, TOK_INT, TOK_FLOAT, TOK_KW_STRING, TOK_BOOL, TOK_RESULT,
    TOK_KW_FN,
    TOK_GET, TOK_HEAD, TOK_POST, TOK_PUT, TOK_PATCH, TOK_DELETE, TOK_OPTIONS,
    TOK_VERBS,
    TOK_ENUM, TOK_MATCH, TOK_PIPE,

    TOK_LPAREN, TOK_RPAREN, TOK_LBRACE, TOK_RBRACE,
    TOK_LBRACKET, TOK_RBRACKET,
    TOK_COMMA, TOK_SEMI, TOK_DOT, TOK_COLON, TOK_QUESTION,

    TOK_EQ, TOK_EQEQ, TOK_NEQ, TOK_LT, TOK_LE, TOK_GT, TOK_GE, TOK_ARROW,
    TOK_PLUS, TOK_MINUS, TOK_STAR, TOK_SLASH, TOK_PERCENT
} TokenType;

typedef struct {
    TokenType type;
    const char *start;/* into the source buffer (never freed) */
    int length;
    size_t line;
    double num;          /* TOK_NUMBER, valid when is_int == 0 */
    long long inum;      /* TOK_NUMBER, valid when is_int == 1 (see docs/SPEC.md) */
    int is_int;/* TOK_NUMBER: literal had no '.', 'e' or 'E' */
    int neg_min;         /* TOK_NUMBER: magnitude was 2^63, so it only becomes a
                          * value as the operand of a unary minus
                          * (`-9223372036854775808` == LLONG_MIN). inum holds
                          * the clamped LLONG_MAX until the parser negates it. */
    /* TOK_ISTR only: the scanned segments of an `s!"..."` interpolated string,
     * serialized as (tag byte 'L' = literal, already unescaped; 'E' = expression
     * source text) + int32 length + payload, repeated. The parser deserializes
     * this into a `str(...) + ...` tree and frees it; every token-free site
     * must free a non-NULL istr. Heap-allocated (malloc'd), NULL for all other
     * token types. */
    char *istr;
    int istr_len;
} Token;

const char *token_type_name(TokenType t);

/* ===================== type system ===================== */

typedef struct Type Type;
typedef struct Node Node;
struct Module; /* forward decl; full definition below (typedef Module) */

typedef enum {
    TY_NULL, TY_INT, TY_FLOAT, TY_STRING, TY_BOOL,
    TY_ANY,    /* untyped: route handlers, builtins, unchecked params */
    TY_LIST,   /* elem is the element type */
    TY_STRUCT, /* name != NULL for declared structs; anonymous otherwise */
    TY_ENUM,   /* declared `enum X { ... }`: name is the enum name. A value
                * travels as the same heap map the interpreter builds
                * (`{ "$": variant, ...payload }`), so it is an opaque i8*
                * natively — the variant list lives in the checker's enum
                * table and each emitter's EDef table, keyed by name. */
    TY_FUNC,   /* a named/top-level function value */
    TY_RESULT, /* `{ ok: ... }` or `{ err: ... }` */
    TY_NS      /* module namespace: `import "lib.lume" as ns` — name is the
                * alias; members resolve through the module's export table */
} TypeKind;

typedef struct Type {
    TypeKind kind;
    char *name;             /* struct name (TY_STRUCT); NULL = anonymous */
    Type *elem;             /* TY_LIST element type */
    char **names;           /* struct field names (TY_STRUCT) */
    Type **types;           /* field types (TY_STRUCT) or param types (TY_FUNC) */
    int count;
    int cap;
    Type *ret;              /* TY_FUNC return type */
    struct Node *func;      /* TY_FUNC only: the node (N_FUNC_LIT or
                             * N_FUNC_DECL) this function type was spelled from.
                             * The native backends thread a closure's lambda
                             * node through here so a closure-variable call can
                             * recover the lambda's own param/ret types. NULL
                             * for a plain `func` name reference. */
    struct Type *tnext;     /* type_release_all()'s reclaim list — internal,
                             * not part of any type's meaning */
} Type;

Type *type_prim(TypeKind kind);
Type *type_list(Type *elem);
Type *type_struct(const char *name);   /* named reference, resolved by checker */
Type *type_anon_struct(void);
Type *type_enum(const char *name);     /* enum reference, resolved by checker */
Type *type_func(int arity, Type **params, Type *ret);
Type *type_result(void);
void type_add_member(Type *t, const char *name, Type *ty);
void type_print(Type *t);              /* type text into stdout (debug) */
/* Reclaim every Type the checker allocated, in one pass.
 *
 * Types form a shared graph: the AST fields, the checker's scope entries and
 * the module export table all point at the same Type blocks, and resolve()
 * hands out definitions that outlive the check that built them. So there is
 * no ownership to walk here — every Type ever allocated since the last call
 * is dead the moment the loader finishes. calloc'd *Type objects, their
 * strdup'd names and their Type** shells all go in this one sweep. The
 * static ANY_TYPE singleton returned by any_type() is never on the list.
 * Must run after the interpreter is done with annotated/ret fields. */
void type_release_all(void);

/* Static type checking pass (compile-time). Returns false + errbuf on the
 * first error. Runs after parsing, before execution. */
bool type_check_program(Node *prog, char *errbuf, size_t errbuf_size);
bool is_builtin_name(const char *name);

/* Local functions (SPEC §4.14): rewrite `func` declarations nested inside a
 * function body into hoisted module-level functions.
 *
 * Every nested N_FUNC_DECL gets a synthetic name (`cname`), and every call
 * inside its enclosing function that names the local is rewritten to that
 * synthetic name. The local therefore resolves nowhere else — a top-level
 * call to the source name is an undefined variable in all three backends,
 * which is exactly the scoping a local declaration should have. Locals do
 * not capture: they compile like any other module function.
 *
 * Idempotent (cnames are stamped once; already-rewritten calls no longer
 * match the source name). Returns false + errbuf when two locals in one
 * enclosing function share a name — the rewrite would otherwise rename
 * both call sets to the second cname. Runs before the checker and again
 * (as a guard) before native codegen. */
bool lume_resolve_localfns(Node *prog, char *errbuf, size_t errbuf_size);

/* Every N_FUNC_DECL in the program, top-level and nested (locals), in
 * pre-order. Writes at most `cap` entries into `out` (which may be NULL);
 * always returns the full count, so callers size their array with a NULL
 * first pass. Shared by the checker's registration pass and both native
 * backends. */
int lume_collect_funcs(Node *prog, Node **out, int cap);

/* Module-aware variant (loader.c): self = module being checked, mods =
 * registry of every loaded module (used to resolve `import "x" as ns`). */
bool type_check_module(struct Module *self, struct Module **mods, int mod_count,
                       Node *prog, char *errbuf, size_t errbuf_size);

/* ===================== values / objects / GC ===================== */

/* VAL_INT vs VAL_FLOAT mirror TY_INT / TY_FLOAT: `int` is a 64-bit two's
 * complement integer and `float` is an IEEE double (docs/SPEC.md). They used to
 * share one VAL_NUM double, which quietly rounded every int past 2^53 and made
 * the interpreter disagree with both native backends. IS_NUM/AS_NUM still
 * accept either kind so the ~70 numeric call sites do not all have to care;
 * new code that needs to tell them apart should use IS_INT/IS_FLOAT. */
typedef enum {
    VAL_NULL, VAL_BOOL, VAL_INT, VAL_FLOAT, VAL_OBJ
} ValType;

typedef struct Value Value;
typedef struct Obj Obj;
typedef struct Env Env;
typedef struct VM VM;

/* Simple open-addressed-ish parallel-array key/value store shared by
 * environments and map objects. Keys are strdup'd and owned by the store;
 * Values are GC roots only while reachable from the store. */
typedef struct {
    char **keys;
    Value *vals;
    int count;
    int cap;
} KVPair;

typedef enum {
    OBJ_STRING, OBJ_MAP, OBJ_LIST, OBJ_FUNC, OBJ_NATIVE, OBJ_ENV
} ObjType;

typedef struct {
    char *data;
    size_t len;
    bool trusted_html;   /* set by html(): already-rendered markup, slots
                          * inject it raw instead of escaping it */
} ObjString;

typedef struct {
    char *name;
    char **params;
    int arity;
    void *body;            /* Node* block — set in interp, opaque here */
    Env *closure;
} ObjFunc;

typedef Value (*NativeFn)(VM *vm, int argc, Value *args);

typedef struct {
    char *name;
    NativeFn fn;
} ObjNative;

typedef struct {
    Value *items;
    int count;
    int cap;
} ObjList;

struct Obj {
    ObjType type;
    bool is_marked;
    struct Obj *next;      /* GC free-list sweep chain */
    union {
        ObjString str;
        KVPair map;        /* OBJ_MAP */
        ObjList list;      /* OBJ_LIST */
        ObjFunc fn;        /* OBJ_FUNC */
        ObjNative native;  /* OBJ_NATIVE */
        struct {
            KVPair vars;   /* local bindings */
            Env *parent;   /* lexical scope */
        } env;             /* OBJ_ENV */
    } as;
    /* Static struct name for a map that is really a value of a named type
     * (`let p: P = {...}` stamps it). NULL for an anonymous map. Only the
     * interpreter needs it: both native backends carry the type on their own
     * value record, and it is what keeps `p.len` on `type P = { len: int }`
     * from collapsing to the member count on the interpreter side. Not a
     * GC root and not freed — it points into the type table, which outlives
     * every value built from it. */
    const char *sname;
};

struct Value {
    ValType type;
    union {
        bool b;
        long long i;/* VAL_INT */
        double n;      /* VAL_FLOAT */
        Obj *o;
    } as;
};

/* AS_NUM widens an int to double. It is lossy past 2^53 by construction, so
 * anything that must stay exact (integer arithmetic, printing, int keys) has
 * to use AS_INT instead. */
#define AS_BOOL(v) ((v).as.b)
#define AS_INT(v)  ((v).as.i)
#define AS_NUM(v)  ((v).type == VAL_INT ? (double)(v).as.i : (v).as.n)
#define AS_OBJ(v)  ((v).as.o)

#define IS_NULL(v)  ((v).type == VAL_NULL)
#define IS_BOOL(v)  ((v).type == VAL_BOOL)
#define IS_INT(v)   ((v).type == VAL_INT)
#define IS_FLOAT(v) ((v).type == VAL_FLOAT)
/* "Is a number of either kind" — the test the ~70 numeric call sites want. */
#define IS_NUM(v)   ((v).type == VAL_INT || (v).type == VAL_FLOAT)
#define IS_OBJ(v)   ((v).type == VAL_OBJ)

static inline Value val_bool(bool b)  { Value v = {VAL_BOOL, {.b = b}}; return v; }
static inline Value val_null(void)    { Value v = {VAL_NULL, {.i = 0}}; return v; }
static inline Value val_float(double n) { Value v = {VAL_FLOAT, {.n = n}}; return v; }
static inline Value val_int(long long i) { Value v = {VAL_INT, {.i = i}};  return v; }
static inline Value val_obj(Obj *o)   { Value v = {VAL_OBJ, {.o = o}};  return v; }

/* Kept as the widening constructor for the many call sites that genuinely
 * mean "a number, precision already decided" (math builtins, str() of a
 * float, ...). Code that means int must call val_int. */
static inline Value val_num(double n) { return val_float(n); }

enum ObjKind { OBJK_STRING, OBJK_MAP, OBJK_LIST, OBJK_FUNC, OBJK_NATIVE };

struct Env {
    Obj header;
    KVPair vars;
    Env *parent;
    Env *next_active;    /* dynamic frame chain for GC traversal */
    bool is_boundary;    /* function frame / module env: assignment never
                          * resolves past this point, so "assigning a name
                          * that only exists further out defines a local"
                          * (top-level names inside a function, globals
                          * inside a module) keeps working. */
};

/* ---- GC / heap ---- */
Obj *gc_alloc(VM *vm, ObjType type, size_t size);
void gc_collect(VM *vm);
char *gc_strdup(VM *vm, const char *s, size_t n);
char *gc_cstr(VM *vm, const char *s);

/* The one escape table for string literals: \n \t \r \0 \\ \" (an unknown
 * escape keeps the character verbatim). Interpreter and lexer share it so the
 * two can never drift — the native backends unescape through it too, which is
 * what keeps `"a\nb"` from printing four characters there. Input is the raw
 * body WITHOUT quotes; the result is malloc'd (caller frees) and NUL-safe
 * only as a convenience (a literal \0 keeps its length). */
char *lume_unescape(const char *s, size_t n, size_t *out_len);

/* ---- value constructors (allocate on the VM heap) ---- */
Value make_string(VM *vm, const char *s, size_t n);
Value make_string_cstr(VM *vm, const char *s);
Value make_map(VM *vm);
Value make_list(VM *vm);
Value make_func(VM *vm, const char *name, char **params, int arity,
                void *body, Env *closure);
Value make_native(VM *vm, const char *name, NativeFn fn);
Value make_env(VM *vm, Env *parent);

/* ---- map / env access ---- */
Value map_get(VM *vm, Obj *map, const char *key, int *found);
void map_set(VM *vm, Obj *map, const char *key, Value v);
Obj *map_shallow_copy(VM *vm, Obj *src); /* `{ p with ... }` — see value.c */
Value env_get(Env *env, const char *name, int *found);
void env_set(VM *vm, Env *env, const char *name, Value v);

/* String helpers on GC string objects. `obj_string` returns `char *`, not
 * `const char *`: a string's bytes live inline behind the Obj and are mutable
 * (see value.c:make_string). Read-only callers bind it to a `const char *`
 * themselves; writing through it is exactly what the type is now honest about. */
char *obj_string(Obj *o);
size_t obj_string_len(Obj *o);

/* JSON-escaping helper shared by the stdio bridges (bridge_mcp.c /
 * bridge_lsp.c): writes a quoted JSON string literal for `s` into `b`. */
void json_esc(sbuf *b, const char *s);

/* Truthiness for `if` / `and` / `or`. */
bool value_truthy(Value v);

/* ---- JSON (sub)set parser producing native values; on error errbuf gets a
 *      message and NO value is pushed. Parses objects, arrays, strings,
 *      numbers, true/false/null — enough for tool-call arguments and the
 *      `json()` builtin. Reachable only through json_parse(). ---- */
void json_parse(VM *vm, const char *src, char *errbuf, size_t errbuf_size);
/* JSON-encode a value into a growable sbuf (from libagenthttpd minijson). */
void json_append_value(VM *vm, void *sbuf, Value v);
/* Structural equality (numbers compare numerically; strings by content). */
bool values_equal(Value a, Value b);

/* ===================== VM ===================== */

/* Bridge bookkeeping: registered route/tool handlers and their roots, plus
 * the server config map. Storing handler Values here keeps them alive —
 * an anonymous function object referenced only by the C callback table
 * would otherwise be collected between registrations. */
/* Route table cap. Was 32, which a generated CRUD app outgrows the moment it
 * carries more than two modules (product+order+pay+shop already sums to 31 in
 * lume-nsgm's PayPal variant; a third module silently fails registration with
 * a message that blames the last route, not the full table). 128 leaves room
 * for several modules per app; RouteRec is ~40B, so the VM grows ~4KB. */
#define MAX_AL_ROUTES 128
#define MAX_AL_TOOLS 96

typedef struct {
    char *method;      /* strdup'd at registration, owned here */
    char *path;        /* ditto */
    char *label;       /* verbs map value (method -> label); NULL if none */
    Value handler;
} RouteRec;

typedef struct {
    char name[65];
    char desc[161];
    char params[4097];
    Value handler;         /* DSL function executed on tool calls (GC root) */
} ToolRec;

#define AL_STACK_MAX 1024
#define AL_FRAME_MAX 64

struct VM {
    Obj *objs;                /* GC-linked heap */
    size_t bytes_allocated;
    size_t gc_threshold;

    Value stack[AL_STACK_MAX];
    int stack_count;

    Env *globals;
    Env *active_envs;         /* dynamic call chain (GC roots) */

    jmp_buf jump_bufs[AL_FRAME_MAX]; /* return-unwind stack */
    int jump_depth;

    bool error;
    char error_msg[512];

    bool loop_break;       /* `break` inside a loop body */
    bool loop_continue;    /* `continue` inside a loop body */

    Value call_result;        /* `return expr` target, set before unwind */

    /* The value of a top-level `main()` call, i.e. the status the process
     * exits with. Both native backends forward this same call out of their C
     * entry, so the interpreter records it here to report the same exit code
     * as the three legs. Set only by exec_program, and only for the entry
     * module — a `main()` called from inside a function is an ordinary call. */
    int main_status;

    /* bridge state */
    RouteRec routes[MAX_AL_ROUTES];
    int route_count;
    Value default_handler;    /* `write "/p";` handler-less route response */
    ToolRec tool_records[MAX_AL_TOOLS];
    int tool_count;
    Obj *server_config;       /* `server { ... }` results (GC root) */
    bool run_called;
    /* serve() sessions: id -> session map, shared across requests (GC root).
     * Per request, session_new is set when a fresh session was created and
     * session_id holds the id for the Set-Cookie response. */
    Obj *session_table;
    bool session_new;
    char session_id[64];
    /* Opt-in filesystem lock for untrusted scripts. Default is OFF, i.e. the
     * interpreter is NOT a sandbox: read_file/write_file/files/mkdir still
     * reach any path the process can open. Turn it on (--no-fs, or
     * LUME_NO_FS=1) and the path-taking builtins fail at runtime instead.
     * Deliberately a runtime flag rather than a root allowlist: locks the
     * language surface without breaking the product tools that legitimately
     * write .env / report files outside the cwd. */
    bool no_fs;

    /* Same idea as no_fs, for the outbound side: http_get() is opt-out here
     * too (--no-net, LUME_NO_NET=1). A script that cannot read or write the
     * filesystem should not be able to phone home to an attacker-hosted
     * collector either — the SSRF guards below默认 deny private ranges, but
     * they deliberately allow public ones, so the kill switch is the only
     * total answer. */
    bool no_net;

    /* module system (loader.c). The registry is populated once, before
     * agenthttpd_run forks workers; modules and their top-level envs are
     * immutable afterwards. */
    struct Module **modules;
    int module_count;
    /* Only ever strcmp'd and realloc'd wholesale, never written to, so the
     * entries keep the const the caller declared. */
    const char **load_stack;  /* paths currently being loaded (cycle check) */
    int load_depth;
    Env *export_env;          /* export table of the module being executed */

    /* CLI script arguments after the script name (`lume-core script.lume a b`
     * → ["a", "b"]), exposed to scripts as argv(). GC root — the list is
     * reachable only through this field once seeded by main(). */
    Obj *argv;
};

/* ===================== modules / loader ===================== */

/* One source file = one module: its own top-level Env and its own export
 * table. importers bind `import "x.lume" as ns` to the module's exports Env. */
typedef struct Module {
    char *path;               /* canonical absolute path (strdup'd, owned) */
    Node *prog;               /* parsed program */
    char **import_paths;      /* raw import strings, relative to this file's dir */
    char **ns_names;          /* parallel `as` aliases */
    int import_count;
    Env *env;                 /* module top-level environment (OBJ_ENV) */
    Env *exports;             /* exported bindings (OBJ_ENV) */
    struct { char **names; Type **types; int count; } export_types;
    bool loading;             /* on the load stack (cycle detect) */
    bool typechecked;
    bool executed;
    bool is_entry;            /* the script the process is running: only its
                               * top level decides the exit status         */
} Module;

/* Load the entry script and every module it transitively imports. Modules are
 * parsed, type-checked (dependencies first) and — unless check_only — their
 * top levels executed in dependency order (each exactly once). Entry module's
 * top level runs last, leaving vm->globals bound to it. Returns 0 on success,
 * nonzero + errbuf on failure. */
int loader_run(VM *vm, const char *entry_path, bool check_only,
               char *errbuf, size_t errbuf_size);
/* Registry lookup by canonical path; NULL if not loaded. */
Module *loader_find(VM *vm, const char *canon_path);
/* Tear the loader down: every module, its AST, its export table and the
 * registry array. Safe to call twice; run it before vm_free() (modules and
 * the AST they own can still be reached through VM roots). */
void loader_free(VM *vm);

/* ===================== parser ===================== */

/* AST node. One struct, union-tagged; the tree is built once, before
 * agenthttpd_run forks workers, and per-request evaluation reuses the same
 * immutable tree. It used to live for the process lifetime — arena_reset()
 * now reclaims it, so a reload or a test case can drop a whole program. */
typedef enum {
    N_PROGRAM, N_BLOCK, N_LET, N_IF, N_WHILE, N_FOR, N_BREAK, N_CONTINUE,
    N_RETURN, N_EXPR_STMT,
    N_SERVER, N_ROUTE, N_TOOL, N_VERBS, N_FUNC_DECL, N_TYPE_DECL, N_ENUM_DECL,
    N_VAR, N_ASSIGN, N_ASSIGN_MEMBER,
    N_LITERAL, N_MAP_LIT, N_LIST_LIT, N_FUNC_LIT,
    N_CALL, N_MEMBER, N_INDEX,
    N_UNARY, N_BINARY, N_TERNARY,
    N_MATCH, N_CTOR, N_IF_LET,
    N_LET_PAT, /* `let { a, b } = e;` — record destructuring */
    N_LET_EXPR, /* `let x = e1 in e2` in expression position — Lean's let-in */
    N_STRUCT_UPDATE, /* `{ base with f: e, ... }` — Lean's structure update */
    N_IMPORT   /* `import "path" as ns;` — top-level only */
} NodeType;

typedef enum {
    LIT_NUM, LIT_STR, LIT_TRUE, LIT_FALSE, LIT_NULL
} LitKind;

typedef enum {
    OP_ADD, OP_SUB, OP_MUL, OP_DIV, OP_MOD,
    OP_EQ, OP_NE, OP_LT, OP_LE, OP_GT, OP_GE,
    OP_AND, OP_OR, OP_NEG, OP_NOT
} Op;

/* One alternative of a match arm's pattern: a variant name (or "_" for the
 * catch-all) plus the positional binds. An or-pattern arm (`A | B => body`)
 * carries one MatchPat per alternative; all alternatives must bind the same
 * names (types unified per name), so the arm has a single binding set — the
 * Lean rule where each alternative may bind differently-typed names is an
 * MVP restriction here (see docs/SPEC.md 3.7).
 * vfields/vfieldc are stamped by parser.c's resolve_ctors: the variant's own
 * field names, borrowed from the enum decl (not owned — freed with it). */
typedef struct MatchPat {
    char *vname;
    char **binds;
    int bindc;
    char **vfields;   /* resolve_ctors stamp; borrowed from the enum decl */
    int vfieldc;
    Type **vfield_types;  /* same stamp, payload types (scalars only) */
} MatchPat;

/* One `Pattern | Pattern [if guard] => body` arm of a match. `guard` is NULL
 * when the arm is unconditional. A guarded arm does not count towards
 * exhaustiveness (Lean's rule) and never conflicts with a later arm. */
typedef struct MatchArm {
    MatchPat *pats;
    int patc;
    struct Node *guard;
    struct Node *body;
    size_t line;
} MatchArm;

typedef struct Node {
    NodeType type;
    size_t line;
    bool is_export;   /* `export` prefix: top-level let/func/type visible to
                       * importers (funcs/lets bind into the module's export
                       * env; types publish a signature only) */
    union {
        struct { struct Node **stmts; int count; } program;
        struct { struct Node **stmts; int count; } block;
        struct { char *name; Type *annot; struct Node *init; } let;
        struct { char *name; Type *annot; struct Node *init; struct Node *body;
                 Type *ty; } letx;   /* ty: checker-stamped body type (SPEC 4.15) */
        struct { struct Node *cond, *then, *els; } ifs;
        struct { struct Node *cond, *body; } whiles;
        /* for: C-style uses init/cond/incr (is_in=false); for-in uses
         * var/iterable (is_in=true). */
        struct { struct Node *init, *cond, *incr, *body;
                 char *var; struct Node *iterable; bool is_in; } fors;
        struct { struct Node *expr; } ret;      /* expr may be NULL */
        struct { struct Node *expr; } expr_stmt;
        struct { struct Node **assigns; int count; } server; /* each N_ASSIGN */
        /* `method` and `alias` are mutually exclusive: a plain route stores the
         * method string, a `verbs`-alias route stores the alias name and
         * resolves it to one or more methods at registration time. */
        struct { char *method; char *alias; char *path;
                 struct Node *handler; } route;
        struct { char *name; struct Node *methods; } verbs;
        struct { char *name; char *desc; struct Node *params;
                 struct Node *handler; } tool;
        struct { char *name; char **names; Type **param_types; Type *ret;
                 int arity; struct Node *body;
                 char *cname; /* local function only: synthetic module-level
                               * name assigned by lume_resolve_localfns();
                               * call sites inside the enclosing function are
                               * rewritten to it, so the local stays invisible
                               * everywhere else. NULL = top-level function. */
               } func;
        struct { char *name; char **field_names; Type **field_types;
                 int count; } type_decl;
        /* `enum Name { V | V(f: t) | ... }` — variants in source order.
         * vnames[i] is the variant name; vfields[i]/vfield_types[i] are its
         * payload field names/types (NULL when the variant carries nothing)
         * and vfield_counts[i] its arity. */
        struct { char *name; char **vnames; char ***vfields;
                 Type ***vfield_types; int *vfield_counts; int vcount;
                 } enum_decl;
        /* A constructor application `V(args...)` / bare `V` — created by
         * parser.c's post-parse rewrite from N_CALL/N_VAR once the file's
         * enum table is known. `name` is the *variant* name; the ctor's arity
         * and payload types come from the enum decl. `fields`/`fieldc` and
         * `ename` are resolve_ctors stamps: the payload field names (borrowed
         * from the enum decl) and the enum's own name, so the interpreter and
         * both emitters build values with no enum table of their own. */
        struct { char *name; struct Node **args; int argc;
                 char **fields; int fieldc; char *ename; } ctor;
        /* `match (scrutinee) { arm, arm, ... }` — an expression. */
        struct { struct Node *scrutinee; struct MatchArm *arms; int armc;
                 } matchn;
        /* `if let Pattern [| Pattern] = value { then } [else ...]` — a
         * statement. Binds live in the then-branch only; guards are not
         * part of the form. patc >= 1. */
        struct { MatchPat *pats; int patc; struct Node *value,
                 *then, *els; } iflet;
        /* `let { a, b } = e;` — record destructuring (Lean's
         * `let ⟨x, y⟩ := p`). Binds every name to the like-named key of
         * the map e evaluates to, in the CURRENT scope (if-let's binds
         * live in the then-branch only), with e evaluated once —
         * equivalent to `let a = e.a; let b = e.b;` sharing one value.
         * types[i] is stamped by the checker with the field's value type,
         * exactly as N_MEMBER.type carries it; NULL for a map whose shape
         * is not statically known (the native backends refuse those, the
         * same way they refuse `m.x` on a dynamic map). names are owned
         * (ident_name copies); types are borrowed from the type tables. */
        struct { char **names;     /* bound names (always owned) */
                 char **fields;    /* struct field read per name
                                    * (rename form `{ f: n }`); for a bare
                                    * `{ a }` fields[i] == names[i] but is a
                                    * SEPARATE strdup so both arrays free
                                    * independently). */
                 Type **types; int count;
                 struct Node *init; } letpat;
        struct { char *name; } var;
        struct { char *name; struct Node *value; } assign;
        struct { struct Node *obj; char *name; struct Node *value; } assign_mem;
        /* `{ base with k: v, ... }`: a fresh map/struct with base's slots
         * shallow-copied and the listed keys overwritten in order. The base is
         * never mutated (functional semantics, like Lean's `{ p with y := 3 }`).
         * keys are owned (ident_name copies); vals are children. */
        struct { struct Node *base; char **keys; struct Node **vals;
                 int count; } supd;
        /* lit.text: `char *`, NOT `const char *`. Only LIT_STR sets it
         * (parser_expr.c:parse_primary arena-strdups a quote-including copy
         * and grows it while merging adjacent literals), so the field lives
         * in the parse arena — the `const` was a stale claim from when it
         * borrowed into the source buffer, and it forced the old free() site
         * to lie with a cast. The other kinds leave it
         * unset; every reader is gated on `kind`. */
        /* LIT_NUM: `inum` is the value when is_float == 0 (int is i64 — see
         * docs/SPEC.md); `num` is the value when is_float == 1. Both are kept
         * because the float-widening paths and --dump still want a double, and
         * casting a >2^53 int through `num` is the precision loss this
         * i64 field exists to avoid. */
        struct { LitKind kind; double num; long long inum; char *text; int len;
                 bool is_float;
                 bool neg_min; /* magnitude was 2^63; only a unary minus may
                                 * consume it, and parse_unary clears this
                                 * when it does. Still set at type-check time
                                 * means the source had no such minus. */
               } lit; /* is_float: LIT_NUM came from `1.5` */
        struct { char **keys; struct Node **vals; int count;
                 bool is_tuple; /* set by the `(a, b)` literal sugar. A tuple
                                 * is anonymous by default — resolve_struct_lit
                                 * names a literal only from its context — and
                                 * infer_list_elem uses the flag to type a
                                 * homogeneous list of tuples by their shape. */ } map;
        struct { struct Node **items; int count; } list;
        struct { char **names; Type **param_types; Type *ret; int arity;
                 struct Node *body;
                 char *cname; /* synthetic mangled name for the closure's
                               * emitted `define`, assigned by the signature
                               * pass; NULL until then */
                 /* Phase-2 free-variable capture: the outer locals (of the
                  * enclosing function) the body reads, name + type each.
                  * Filled by the capture pass in codegen_sig.c; NULL/0 when
                  * the lambda captures nothing. Not owned: names are
                  * arena_strdup'd by the pass; the Type pointers are
                  * borrowed from the outer scope. */
                 char **cap_names; Type **cap_types; int cap_count;
               } funclit;
        struct { struct Node *callee; struct Node **args; int argc;
                 bool propagate; } call; /* propagate: trailing `?` */
        struct { struct Node *obj; char *name; struct Type *type; } member;
        /* `m["k"]` / `l[0]`: `obj[index]`. Kept apart from `member` rather
         * than folded into it because the key is an arbitrary expression, not
         * an identifier -- a runtime value that has to be evaluated, and whose
         * type decides which accessor reads it. */
        struct { struct Node *obj; struct Node *index; } index;
        struct { Op op; struct Node *operand; } unary;
        struct { Op op; struct Node *left, *right; } binary;
        /* `cond ? then : els` — 右结合；else 分支不逃逸求值（只算选中那支）。
         * 与 `f()?` 传播后缀的歧义由 parser_expr.c 的 ternary_colon_ahead
         * 前瞻消解。 */
        struct { struct Node *cond, *then, *els; } ternary;
        /* import: path is the raw string literal from the source (resolved
         * against the importing file's directory by the loader); ns is the
         * `as` alias. */
        struct { char *path; char *ns; } imp;
    } as;
} Node;

/* Parse `source` (NUL-terminated). Returns the program node, or NULL with a
 * human-readable error in errbuf (errbuf_size). */
Node *parse_program(const char *source, char *errbuf, size_t errbuf_size);

/* Builtin name table (typecheck.c): shared with the LSP bridge for hover /
 * completion. Count is derived at compile time. */
extern const char *const LUME_BUILTIN_NAMES[];
extern const size_t LUME_BUILTIN_COUNT;

/* Shared lexer entry (also used by the REPL builtins). Caller frees the
 * returned token array. NULL + errbuf filled on lexical errors. */
Token *al_lex(const char *source, char *errbuf, size_t errbuf_size,
              int *out_count);

void node_print(Node *n, int indent);   /* debugging dump */
/* The AST is arena-allocated (TODO.md P2, docs/ARENA.md): nothing the
 * parser builds is freed individually. Reclaim it with arena_reset() at
 * the points where the tree dies today — the same call sites that used to
 * run node_free() — after nothing can walk the tree anymore (ObjFunc.body
 * keeps a Node*, so the VM heap must be gone first). The Type objects
 * behind param_types/field_types/annot keep their own sweep,
 * type_release_all(). */

/* ===================== interpreter / bridge ===================== */

void vm_init(VM *vm);
/* Tear the VM down: the GC heap with every Obj it still holds (and the
 * strdup'd map/env keys copied into those), the strdup'd route records
 * and the load stack. Does NOT free the AST or the Type graph — those are
 * loader_free()'s and type_release_all()'s. Idempotent. */
void vm_free(VM *vm);
/* Execute the top-level of a parsed program (routes/tools registered here,
 * before run() is ever called). Sets vm.error on failure. */
void exec_program(VM *vm, Node *prog);
/* Call the function value whose callee and argc args are already stacked
 * (in that order). Replaces the [callee, a0..an-1] region with one result. */
void call_function(VM *vm, Value callee, int argc);
/* VM stack helpers used by natives & the bridge. */
void vm_push(VM *vm, Value v);
Value vm_pop(VM *vm);
Value vm_peek(VM *vm, int depth);
void vm_set_error(VM *vm, const char *fmt, ...);

/* bridge.c — translate DSL constructs into libagenthttpd calls. */
void bridge_init(VM *vm);                      /* bind the shim's VM pointer */
void bridge_seed_builtins(VM *vm);
int bridge_define_route(VM *vm, const char *method, const char *path, Value handler,
                        const char *label);
int bridge_define_tool(VM *vm, const char *name, const char *desc,
                       const char *params_json, Value handler);
void bridge_run(VM *vm);
void mcp_run(VM *vm);
void lsp_run(VM *vm);

#endif /* LUME_H */
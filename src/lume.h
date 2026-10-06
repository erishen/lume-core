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

/* This tree is the standalone-language build. The host build (work/research/lume)
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

    TOK_IDENT, TOK_NUMBER, TOK_STRING,

    /* keywords */
    TOK_SERVER, TOK_ROUTE, TOK_TOOL, TOK_FUNC, TOK_RETURN,
    TOK_IF, TOK_ELSE, TOK_WHILE, TOK_FOR, TOK_IN, TOK_BREAK, TOK_CONTINUE,
    TOK_LET,
    TOK_IMPORT, TOK_EXPORT, TOK_AS,
    TOK_TRUE, TOK_FALSE, TOK_NULL, TOK_AND, TOK_OR, TOK_NOT,
    TOK_TYPE, TOK_INT, TOK_FLOAT, TOK_KW_STRING, TOK_BOOL, TOK_RESULT,
    TOK_GET, TOK_HEAD, TOK_POST, TOK_PUT, TOK_PATCH, TOK_DELETE, TOK_OPTIONS,
    TOK_VERBS,

    TOK_LPAREN, TOK_RPAREN, TOK_LBRACE, TOK_RBRACE,
    TOK_LBRACKET, TOK_RBRACKET,
    TOK_COMMA, TOK_SEMI, TOK_DOT, TOK_COLON, TOK_QUESTION,

    TOK_EQ, TOK_EQEQ, TOK_NEQ, TOK_LT, TOK_LE, TOK_GT, TOK_GE, TOK_ARROW,
    TOK_PLUS, TOK_MINUS, TOK_STAR, TOK_SLASH, TOK_PERCENT
} TokenType;

typedef struct {
    TokenType type;
    const char *start;   /* into the source buffer (never freed) */
    int length;
    size_t line;
    double num;          /* TOK_NUMBER */
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
    struct Type *tnext;     /* type_release_all()'s reclaim list — internal,
                             * not part of any type's meaning */
} Type;

Type *type_prim(TypeKind kind);
Type *type_list(Type *elem);
Type *type_struct(const char *name);   /* named reference, resolved by checker */
Type *type_anon_struct(void);
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
/* Module-aware variant (loader.c): self = module being checked, mods =
 * registry of every loaded module (used to resolve `import "x" as ns`). */
bool type_check_module(struct Module *self, struct Module **mods, int mod_count,
                       Node *prog, char *errbuf, size_t errbuf_size);

/* ===================== values / objects / GC ===================== */

typedef enum {
    VAL_NULL, VAL_BOOL, VAL_NUM, VAL_OBJ
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
        double n;
        Obj *o;
    } as;
};

#define AS_BOOL(v) ((v).as.b)
#define AS_NUM(v)  ((v).as.n)
#define AS_OBJ(v)  ((v).as.o)

#define IS_NULL(v)  ((v).type == VAL_NULL)
#define IS_BOOL(v)  ((v).type == VAL_BOOL)
#define IS_NUM(v)   ((v).type == VAL_NUM)
#define IS_OBJ(v)   ((v).type == VAL_OBJ)

static inline Value val_bool(bool b)  { Value v = {VAL_BOOL, {.b = b}}; return v; }
static inline Value val_null(void)    { Value v = {VAL_NULL, {.n = 0}}; return v; }
static inline Value val_num(double n) { Value v = {VAL_NUM, {.n = n}};  return v; }
static inline Value val_obj(Obj *o)   { Value v = {VAL_OBJ, {.o = o}};  return v; }

/* Marshal an integer without FP rounding surprises (transfer doubles). */
static inline Value val_int(long long i) { return val_num((double)i); }

enum ObjKind { OBJK_STRING, OBJK_MAP, OBJK_LIST, OBJK_FUNC, OBJK_NATIVE };

struct Env {
    Obj header;
    KVPair vars;
    Env *parent;
    Env *next_active;    /* dynamic frame chain for GC traversal */
};

/* ---- GC / heap ---- */
Obj *gc_alloc(VM *vm, ObjType type, size_t size);
void gc_collect(VM *vm);
char *gc_strdup(VM *vm, const char *s, size_t n);
char *gc_cstr(VM *vm, const char *s);

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
Value env_get(Env *env, const char *name, int *found);
void env_set(VM *vm, Env *env, const char *name, Value v);

/* String helpers on GC string objects. `obj_string` returns `char *`, not
 * `const char *`: a string's bytes live inline behind the Obj and are mutable
 * (see value.c:make_string). Read-only callers bind it to a `const char *`
 * themselves; writing through it is exactly what the type is now honest about. */
char *obj_string(Obj *o);
size_t obj_string_len(Obj *o);

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
#define MAX_AL_ROUTES 32
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
 * immutable tree. It used to live for the process lifetime — node_free() now
 * releases it, so a reload or a test case can drop a whole program. */
typedef enum {
    N_PROGRAM, N_BLOCK, N_LET, N_IF, N_WHILE, N_FOR, N_BREAK, N_CONTINUE,
    N_RETURN, N_EXPR_STMT,
    N_SERVER, N_ROUTE, N_TOOL, N_VERBS, N_FUNC_DECL, N_TYPE_DECL,
    N_VAR, N_ASSIGN, N_ASSIGN_MEMBER,
    N_LITERAL, N_MAP_LIT, N_LIST_LIT, N_FUNC_LIT,
    N_CALL, N_MEMBER,
    N_UNARY, N_BINARY,
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
                 int arity; struct Node *body; } func;
        struct { char *name; char **field_names; Type **field_types;
                 int count; } type_decl;
        struct { char *name; } var;
        struct { char *name; struct Node *value; } assign;
        struct { struct Node *obj; char *name; struct Node *value; } assign_mem;
        /* lit.text: `char *`, NOT `const char *`. Only LIT_STR sets it
         * (parser_expr.c:parse_primary mallocs a quote-including copy and
         * reallocs it while merging adjacent literals), so the field owns
         * heap memory and node_free_own() frees it — the `const` was a stale
         * claim from when it borrowed into the source buffer, and it forced
         * that free() site to lie with a cast. The other kinds leave it
         * unset; every reader is gated on `kind`. */
        struct { LitKind kind; double num; char *text; int len;
                 bool is_float; } lit; /* is_float: LIT_NUM came from `1.5` */
        struct { char **keys; struct Node **vals; int count; } map;
        struct { struct Node **items; int count; } list;
        struct { char **names; Type **param_types; Type *ret; int arity;
                 struct Node *body; } funclit;
        struct { struct Node *callee; struct Node **args; int argc;
                 bool propagate; } call; /* propagate: trailing `?` */
        struct { struct Node *obj; char *name; struct Type *type; } member;
        struct { Op op; struct Node *operand; } unary;
        struct { Op op; struct Node *left, *right; } binary;
        /* import: path is the raw string literal from the source (resolved
         * against the importing file's directory by the loader); ns is the
         * `as` alias. */
        struct { char *path; char *ns; } imp;
    } as;
} Node;

/* Parse `source` (NUL-terminated). Returns the program node, or NULL with a
 * human-readable error in errbuf (errbuf_size). */
Node *parse_program(const char *source, char *errbuf, size_t errbuf_size);

/* Shared lexer entry (also used by the REPL builtins). Caller frees the
 * returned token array. NULL + errbuf filled on lexical errors. */
Token *al_lex(const char *source, char *errbuf, size_t errbuf_size,
              int *out_count);

void node_print(Node *n, int indent);   /* debugging dump */
/* Free a whole AST subtree. Reclaims the nodes themselves, every
 * ident_name()/path string inside them and the char**, Node** and Type**
 * array shells,
 * but NOT the Type objects behind those pointers — those belong to
 * type_release_all(). Safe to call only once nothing can walk the tree
 * again: ObjFunc.body keeps a Node*, so free the VM heap first if you free
 * the AST through it. */
void node_free(Node *n);

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

#endif /* LUME_H */
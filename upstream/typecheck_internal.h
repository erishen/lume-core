#ifndef LUME_TYPECHECK_INTERNAL_H
#define LUME_TYPECHECK_INTERNAL_H

#include "lume.h"

/* 类型检查器内部结构与跨文件共享声明(typecheck.c / typecheck_expr.c /
 * typecheck_stmt.c)。Checker 在 type_check_module 中一次性构造,
 * 表达式与语句检查共享同一 scope / struct 表 / 错误状态。 */

/* ===================== checker state ===================== */

typedef struct CSym {
    char *name;
    Type *type;          /* can be NULL while a struct def is being filled */
    struct CSym *next;
} CSym;

/* A module namespace: `import "x.lume" as ns` registers ns here; member
 * lookups resolve through the module's export signatures. */
typedef struct CNS {
    char *name;
    struct Module *mod;
    struct CNS *next;
} CNS;

typedef struct CScope {
    struct CScope *parent;
    CSym *syms;
    CNS *nss;
} CScope;

typedef struct StructDef {
    char *name;
    Type *type;
    struct StructDef *next;
} StructDef;

typedef struct {
    CScope *scope;
    StructDef *structs;
    Type *cur_ret;       /* enclosing function return type; NULL outside func */
    bool in_func;        /* inside a function body */
    int loop_depth;      /* nested for/while depth (for break/continue) */
    Type *param_hint;    /* one-shot: force a literal's first unannotated param */
    char *errbuf;
    size_t errbuf_size;
    bool failed;
    /* module context (NULL for single-file / REPL checks): the module being
     * checked and the loader registry used to resolve `import "x" as ns`. */
    struct Module *self;
    struct Module **mods;
    int mod_count;
} Checker;

/* ---- 基础(typecheck.c) ---- */
Type *any_type(void);
const char *ty_str(Type *t);
void ck_fail(Checker *c, size_t line, const char *fmt, ...);
void export_add(Checker *c, const char *name, Type *t);
CScope *scope_new(CScope *parent);
void scope_put(CScope *s, const char *name, Type *t);
void scope_decl(Checker *c, CScope *s, const char *name, Type *t,
                size_t line); /* 同层重名编译错(用户声明之间, 内置名可遮蔽) */
bool is_builtin_name(const char *name);
Type *scope_get(CScope *s, const char *name);
void scope_put_ns(CScope *s, const char *name, Module *m);
Module *scope_get_ns(CScope *s, const char *name);
StructDef *find_struct(Checker *c, const char *name);
void add_struct(Checker *c, const char *name, Type *t);
Type *resolve(Checker *c, Type *t, size_t line);
bool type_compat(Checker *c, Type *src, Type *dst, size_t line);
bool is_bool_ok(Checker *c, Type *t, size_t line);
void expect_compat(Checker *c, Type *actual, Type *expected,
                   size_t line, const char *what);
Type *arith_result(Type *a, Type *b);

/* ---- 表达式 / 语句检查(typecheck_expr.c / typecheck_stmt.c) ---- */
Type *ck_expr(Checker *c, Node *n, Type *expected);
void ck_stmt(Checker *c, Node *n);
void ck_blk(Checker *c, Node *n);
void ck_fn(Checker *c, char **names, Type *ft, Node *body);

#endif /* LUME_TYPECHECK_INTERNAL_H */

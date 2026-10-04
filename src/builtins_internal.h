#ifndef LUME_BUILTINS_INTERNAL_H
#define LUME_BUILTINS_INTERNAL_H

#include "builtins.h"
/* The host build reached minijson.h / tools.h / skills.h / sqlite_tool.h
 * through agenthttpd.h. This tree has no agent-httpd. The discovery builtins
 * below (tools() / skills() / mcps() / catalog()) keep the same surface, but
 * the tool & skill tables behind them are this tree's own (see the registry
 * block further down) — nothing is read out of a host static library. */

/* 内建函数实现的跨文件共享声明(builtins.c / builtins_sql.c /
 * builtins_fs.c / builtins_catalog.c / builtins_hof.c)。
 * native_* 是各片的实现,b_* 包装(builtins.c)与片间 helper
 * (list_push / 排序比较器)都经此声明;仅片内自用的 static 不在此列。 */

/* ---- 基础 helper(builtins.c) ---- */
bool arg_string(VM *vm, Value v, const char **out);
void str_of_value(VM *vm, Value v, Value *out);
int value_from_map(VM *vm, int argc, Value *args, Value *v);

/* ---- 列表 / 排序 helper(builtins_fs.c) ---- */
void list_push(VM *vm, Obj *list, Value v);
int str_entry_cmp(const void *a, const void *b);
int skill_entry_cmp(const void *a, const void *b);

/* ---- 文件 / 环境 / 锁 / 时间(builtins_fs.c) ---- */
void native_env(VM *vm, int argc, Value *args, Value *out);
void native_files(VM *vm, int argc, Value *args, Value *out);
void native_read_file(VM *vm, int argc, Value *args, Value *out);
void native_write_file(VM *vm, int argc, Value *args, Value *out);
void native_mkdir(VM *vm, int argc, Value *args, Value *out);
void native_lock_file(VM *vm, int argc, Value *args, Value *out);
void native_unlock_file(VM *vm, int argc, Value *args, Value *out);
void native_strftime(VM *vm, int argc, Value *args, Value *out);
void native_put(VM *vm, int argc, Value *args, Value *out);

/* ---- 工具 / 技能 / MCP / 目录(builtins_catalog.c) ---- */
/* The host read these out of agent-httpd's tool/skill tables. Here they are
 * this tree's own registry: same names, same accessors, same empty-by-default
 * behaviour — the standalone compiler has no agent tools to index, so
 * tools()/skills()/catalog() report empty lists rather than being removed
 * (removing them would fork the language surface).
 * SKILL_DESC_MAX is the /discovery view's description cap; catalog() carries
 * the full text. */
#define TOOL_MAX   64
#define SKILL_MAX  256
#define SKILL_DESC_MAX 64

typedef struct {
    const char *name;
    const char *desc;   /* may be NULL */
    const char *path;   /* may be NULL: not indexed from disk */
} SkillInfo;

/* ToolDef mirrors the host's agent-tool record minus the function pointer:
 * a tool here is a name, a description and an OpenAI-style params schema —
 * this tree can only report on tools, not execute them. */
typedef struct {
    const char *name;
    const char *desc;
    const char *params; /* JSON schema, "{}" when unknown; may be NULL */
    const char *path;   /* may be NULL */
} ToolDef;

int  tools_count(void);
const ToolDef *tools_get(int i);
int  skills_count(void);
const SkillInfo *skills_get(int i);
/* Registering a tool or a skill: the host did this at start-up from its own
 * tables. Nothing registers here, so the counts stay 0 until this tree grows
 * an agent surface of its own. */
void tool_register(const char *name, const char *desc);
void skill_register(const char *name, const char *desc, const char *path);

void native_tools(VM *vm, int argc, Value *args, Value *out);
void native_skills(VM *vm, int argc, Value *args, Value *out);
void native_mcps(VM *vm, int argc, Value *args, Value *out);
void native_discovery_endpoints(VM *vm, int argc, Value *args, Value *out);
void native_catalog(VM *vm, int argc, Value *args, Value *out);

/* ---- 高阶集合函数(builtins_hof.c) ---- */
void native_range(VM *vm, int argc, Value *args, Value *out);
void native_map(VM *vm, int argc, Value *args, Value *out);
void native_filter(VM *vm, int argc, Value *args, Value *out);
void native_reduce(VM *vm, int argc, Value *args, Value *out);

#endif /* LUME_BUILTINS_INTERNAL_H */

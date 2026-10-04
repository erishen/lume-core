/* 工具 / 技能 / MCP / 发现端点 / catalog 内建——从 builtins.c 拆出
 * (2026-09-27)。MCP args 在对外发布前一律脱敏(<redacted>)。 */

#include "builtins_internal.h"
#include <errno.h>
#include <sys/stat.h>

/* ---- fork-local tool / skill registry -------------------------------
 * The host build read these tables out of agent-httpd (tools.c, populated at
 * start-up from the agent's own registrations). This tree registers nothing,
 * so both tables start and stay empty: tools()/skills()/catalog() report
 * empty lists instead of the builtins being dropped. The accessors keep the
 * same names/semantics so builtins_fs.c's skill_entry_cmp and native_catalog
 * need no changes. */
static ToolDef g_tools[TOOL_MAX];
static SkillInfo g_skills[SKILL_MAX];
static int g_tools_n;
static int g_skills_n;

int tools_count(void) { return g_tools_n; }

const ToolDef *tools_get(int i)
{
    return (i >= 0 && i < g_tools_n) ? &g_tools[i] : NULL;
}

int skills_count(void) { return g_skills_n; }

const SkillInfo *skills_get(int i)
{
    return (i >= 0 && i < g_skills_n) ? &g_skills[i] : NULL;
}

/* Nothing calls these yet — they exist so this tree can grow an agent
 * surface without touching the builtins again. */
void tool_register(const char *name, const char *desc)
{
    if (g_tools_n >= TOOL_MAX) return;
    g_tools[g_tools_n].name = name;
    g_tools[g_tools_n].desc = desc;
    g_tools[g_tools_n].path = NULL;
    g_tools_n++;
}

void skill_register(const char *name, const char *desc, const char *path)
{
    if (g_skills_n >= SKILL_MAX) return;
    g_skills[g_skills_n].name = name;
    g_skills[g_skills_n].desc = desc;
    g_skills[g_skills_n].path = path;
    g_skills_n++;
}

/* Sorted list of every registered agent tool (local builtins, DSL `tool`
 * registrations, MCP <server>/<tool>, router proxies). */
void native_tools(VM *vm, int argc, Value *args, Value *out) {
    (void)argc; (void)args;
    Obj *list = AS_OBJ(make_list(vm));
    vm_push(vm, val_obj((Obj *)list)); /* root while filling */
    const char *names[TOOL_MAX];
    int n = 0;
    int full = tools_count();
    for (int i = 0; i < full && n < TOOL_MAX; i++) names[n++] = tools_get(i)->name;
    qsort(names, n, sizeof names[0], str_entry_cmp);
    for (int i = 0; i < n; i++) list_push(vm, list, make_string_cstr(vm, names[i]));
    *out = vm_pop(vm);
}

/* Sorted list of { name, desc } for every indexed skill. */
void native_skills(VM *vm, int argc, Value *args, Value *out) {
    (void)argc; (void)args;
    Obj *list = AS_OBJ(make_list(vm));
    vm_push(vm, val_obj((Obj *)list)); /* root while filling */
    const SkillInfo *items[256];
    int n = 0;
    int full = skills_count();
    for (int i = 0; i < full && n < 256; i++) items[n++] = skills_get(i);
    qsort(items, n, sizeof items[0], skill_entry_cmp);
    for (int i = 0; i < n; i++) {
        Obj *m = AS_OBJ(make_map(vm));
        vm_push(vm, val_obj((Obj *)m)); /* root the map while filling */
        map_set(vm, m, "name", make_string_cstr(vm, items[i]->name));
        map_set(vm, m, "desc", make_string_cstr(vm, items[i]->desc));
        vm_pop(vm);                    /* now owned by the rooted list */
        list_push(vm, list, val_obj((Obj *)m));
    }
    *out = vm_pop(vm);
}

/* MCP entries must never be published over HTTP verbatim: `args` may carry
 * absolute host filesystem paths (machine/user layout) and, if a deployment
 * ever inlines a credential, the token too. Replace it with a fixed marker
 * and keep the safe fields (id/transport/command/source/approval). */
#define MCP_ARGS_REDACTED "<redacted>"

static void redact_mcp_args(VM *vm, Obj *m) {
    int found = 0;
    map_get(vm, m, "args", &found);
    if (found) map_set(vm, m, "args", make_string_cstr(vm, MCP_ARGS_REDACTED));
}

static void redact_mcp_list(VM *vm, Value v) {
    if (!IS_OBJ(v) || AS_OBJ(v)->type != OBJ_LIST) return;
    Obj *list = AS_OBJ(v);
    for (int i = 0; i < list->as.list.count; i++) {
        Value item = list->as.list.items[i];
        if (!IS_OBJ(item) || AS_OBJ(item)->type != OBJ_MAP) continue;
        redact_mcp_args(vm, AS_OBJ(item));
    }
}

/* Read <cwd>/.data/mcp-servers-router.json and parse it as JSON.
 * Returns null when the file does not exist or is invalid (the router
 * catalog is optional — only present after a router sync ran). */
void native_mcps(VM *vm, int argc, Value *args, Value *out) {
    (void)vm; (void)argc; (void)args;
    const char *path = ".data/mcp-servers-router.json";
    FILE *f = fopen(path, "rb");
    if (!f) {
        /* No sync file -> "no MCP servers known", which is an empty list, not
         * null. Returning null here made /discovery publish "mcps": null and
         * the hub pages crashed on data.mcps.length; a missing sync is a
         * normal state (first boot, router down), not a read error. */
        Obj *empty = AS_OBJ(make_list(vm));
        vm_push(vm, val_obj((Obj *)empty));
        *out = vm_pop(vm);
        return;
    }
    sbuf b = {0};
    char buf[16384];
    size_t got;
    while ((got = fread(buf, 1, sizeof buf, f)) > 0) {
        if (b.len + got > (16u << 20)) {
            fclose(f);
            free(b.p);
            vm_set_error(vm, "mcps() too large");
            *out = val_null();
            return;
        }
        sb_mem(&b, buf, got);
    }
    fclose(f);
    if (b.oom || !b.p) { free(b.p); *out = val_null(); return; }
    char err[256] = {0};
    json_parse(vm, b.p, err, sizeof(err));
    free(b.p);
    if (vm->error) { *out = val_null(); return; }
    *out = vm_pop(vm);
    redact_mcp_list(vm, *out);
}

/* discovery_endpoints(): { llm, router, model } safe for publication.
 * The raw URLs are deliberately omitted — they expose the internal service
 * topology (host.docker.internal, private ports, /v1 paths) and can carry
 * credentials in the query string. The discovery UI only needs to know
 * whether an upstream is wired and which model runs. */
void native_discovery_endpoints(VM *vm, int argc, Value *args, Value *out) {
    (void)argc; (void)args;
    const char *llm = getenv("LLM_API_URL");
    const char *router = getenv("ROUTER_API_URL");
    const char *model = getenv("LLM_MODEL");
    Obj *m = AS_OBJ(make_map(vm));
    vm_push(vm, val_obj((Obj *)m)); /* root while filling */
    map_set(vm, m, "llm", (llm && llm[0]) ? make_string_cstr(vm, "configured")
                                           : val_null());
    map_set(vm, m, "router", (router && router[0]) ? make_string_cstr(vm, "configured")
                                                    : val_null());
    map_set(vm, m, "model", (model && model[0]) ? make_string_cstr(vm, model)
                                                 : val_null());
    vm_pop(vm);
    *out = val_obj((Obj *)m);
}

/* ---------- catalog(): rich registry snapshot -----------------------
 * /discovery stays a light debug view (tools as name strings, skills'
 * desc capped at SKILL_DESC_MAX); catalog() is the full 台账:
 *   skills: name + FULL description (re-read from SKILL.md) + path
 *   tools:  name + desc + params schema (OpenAI-style JSON)
 *   mcps:   gateway (.data/mcp-servers-router.json) + local
 *           (.data/mcp-servers.json) merged, each tagged with "source".
 * Backed by the same registries /discovery reads, so a profile allow-list
 * is reflected here too. */

/* Read a whole file into an sbuf (16 MiB cap); 1 ok, 0 otherwise. */
static int file_read_sbuf(const char *path, sbuf *b) {
    FILE *f = fopen(path, "rb");
    if (!f) return 0;
    char buf[16384];
    size_t got;
    while ((got = fread(buf, 1, sizeof buf, f)) > 0) {
        if (b->len + got > (16u << 20)) {
            fclose(f);
            free(b->p);
            return 0;
        }
        sb_mem(b, buf, got);
    }
    fclose(f);
    return b->p != NULL && !b->oom;
}

/* The in-memory SkillInfo.desc is capped at SKILL_DESC_MAX; re-read the
 * SKILL.md frontmatter to hand out the full one (caller frees). */
static char *skill_full_desc(const char *path) {
    sbuf b = {0};
    if (!path || !file_read_sbuf(path, &b)) return NULL;
    char *p = b.p, *end = b.p + b.len;
    int in_fm = 0;
    while (p < end) {
        char *nl = memchr(p, '\n', (size_t)(end - p));
        size_t linelen = nl ? (size_t)(nl - p) : (size_t)(end - p);
        const char *s = p;
        while (s < p + linelen && (*s == ' ' || *s == '\t')) s++;
        if (s >= p + linelen) { p += linelen + 1; continue; }
        if (strncmp(s, "---", 3) == 0) {
            if (!in_fm) { in_fm = 1; }
            p += linelen + 1;
            continue;
        }
        if (in_fm && strncmp(s, "description:", 12) == 0) {
            s += 12;
            while (s < p + linelen && (*s == ' ' || *s == '\t')) s++;
            size_t n = (size_t)((p + linelen) - s);
            char *desc = malloc(n + 1);
            if (desc) { memcpy(desc, s, n); desc[n] = '\0'; }
            free(b.p);
            return desc;
        }
        p += linelen + 1;
    }
    free(b.p);
    return NULL;
}

/* Parse <path> as JSON, loading the result onto the VM stack (rooted).
 * Returns 1 when a value was pushed; 0 (nothing pushed) when the file is
 * missing. A parse failure surfaces through vm->error like any other. */
static int json_file_push(VM *vm, const char *path) {
    FILE *f = fopen(path, "rb");
    if (!f) return 0;
    sbuf b = {0};
    char buf[16384];
    size_t got;
    while ((got = fread(buf, 1, sizeof buf, f)) > 0) {
        if (b.len + got > (16u << 20)) {
            fclose(f);
            free(b.p);
            vm_set_error(vm, "catalog(): file too large");
            return 0;
        }
        sb_mem(&b, buf, got);
    }
    fclose(f);
    if (b.oom || !b.p) { free(b.p); return 0; }
    char err[256] = {0};
    json_parse(vm, b.p, err, sizeof(err));
    free(b.p);
    /* success leaves the parsed value on the VM stack */
    return !vm->error;
}

static int tool_def_cmp(const void *a, const void *b) {
    const ToolDef *x = *(const ToolDef *const *)a;
    const ToolDef *y = *(const ToolDef *const *)b;
    return strcmp(x->name, y->name);
}

/* Append every map in a parsed JSON array to `list`, tagging source. */
static void mcps_merge(VM *vm, Obj *list, Value v, const char *source) {
    if (!IS_OBJ(v) || AS_OBJ(v)->type != OBJ_LIST) return;
    Obj *src = AS_OBJ(v);
    for (int i = 0; i < src->as.list.count; i++) {
        Value item = src->as.list.items[i];
        if (!IS_OBJ(item) || AS_OBJ(item)->type != OBJ_MAP) continue;
        map_set(vm, AS_OBJ(item), "source", make_string_cstr(vm, source));
        redact_mcp_args(vm, AS_OBJ(item));
        list_push(vm, list, item);
    }
}

/* Build the rich catalog: { skills:[{name,desc,path}], tools:[{name,desc,
 * schema}], mcps:[{id,source,...}] }. The result map is left rooted ON the
 * VM stack (caller pops it after use) — returned via *out. */
static void build_catalog(VM *vm, Value *out) {
    /* skills: full desc + path, sorted by name */
    Obj *skills = AS_OBJ(make_list(vm));
    vm_push(vm, val_obj((Obj *)skills)); /* root while filling */
    const SkillInfo *sitems[256];
    int sfull = skills_count();
    int sn = 0;
    for (int i = 0; i < sfull && sn < 256; i++) sitems[sn++] = skills_get(i);
    qsort(sitems, sn, sizeof sitems[0], skill_entry_cmp);
    for (int i = 0; i < sn; i++) {
        char *full = skill_full_desc(sitems[i]->path);
        Obj *m = AS_OBJ(make_map(vm));
        vm_push(vm, val_obj((Obj *)m));
        map_set(vm, m, "name", make_string_cstr(vm, sitems[i]->name));
        map_set(vm, m, "desc",
                make_string_cstr(vm, full ? full : sitems[i]->desc));
        map_set(vm, m, "path", make_string_cstr(vm, sitems[i]->path));
        free(full);
        vm_pop(vm);
        list_push(vm, skills, val_obj((Obj *)m));
    }

    /* tools: name/desc/schema, sorted by name */
    Obj *tools = AS_OBJ(make_list(vm));
    vm_push(vm, val_obj((Obj *)tools));
    const ToolDef *titems[TOOL_MAX];
    int tfull = tools_count();
    int tn = 0;
    for (int i = 0; i < tfull && tn < TOOL_MAX; i++) titems[tn++] = tools_get(i);
    qsort(titems, tn, sizeof titems[0], tool_def_cmp);
    for (int i = 0; i < tn; i++) {
        Obj *m = AS_OBJ(make_map(vm));
        vm_push(vm, val_obj((Obj *)m));
        map_set(vm, m, "name", make_string_cstr(vm, titems[i]->name));
        map_set(vm, m, "desc", make_string_cstr(vm, titems[i]->desc));
        map_set(vm, m, "schema",
                make_string_cstr(vm, titems[i]->params[0] ? titems[i]->params
                                                          : "{}"));
        vm_pop(vm);
        list_push(vm, tools, val_obj((Obj *)m));
    }

    /* mcps: gateway + local, merged with a source tag */
    Obj *mcps = AS_OBJ(make_list(vm));
    vm_push(vm, val_obj((Obj *)mcps));
    int gw = json_file_push(vm, ".data/mcp-servers-router.json");
    if (gw) {
        mcps_merge(vm, mcps, vm->stack[vm->stack_count - 1], "gateway");
        vm_pop(vm);
    }
    int loc = json_file_push(vm, ".data/mcp-servers.json");
    if (loc) {
        mcps_merge(vm, mcps, vm->stack[vm->stack_count - 1], "local");
        vm_pop(vm);
    }

    Obj *r = AS_OBJ(make_map(vm));
    vm_push(vm, val_obj((Obj *)r));
    map_set(vm, r, "skills", val_obj((Obj *)skills));
    map_set(vm, r, "tools", val_obj((Obj *)tools));
    map_set(vm, r, "mcps", val_obj((Obj *)mcps));
    *out = val_obj((Obj *)r); /* left on the stack; caller pops */
}

void native_catalog(VM *vm, int argc, Value *args, Value *out) {
    (void)argc; (void)args;
    build_catalog(vm, out);
    vm_pop(vm); /* r */
    vm_pop(vm); /* mcps */
    vm_pop(vm); /* tools */
    vm_pop(vm); /* skills */
}

/* Fork-local stand-in for the host tree's src/bridge.c.
 *
 * In work/research/lume, bridge_define_route()/bridge_define_tool() push the
 * DSL's `route {}` / `tool {}` registrations into agenthttpd's routing and
 * tool tables, and bridge_run() starts the agent-httpd server. This tree has
 * no agent-httpd, so:
 *
 *   - route/tool registration is recorded into the VM's own tables (they
 *     already exist in VM: RouteRec routes[] / ToolRec tool_records[] hold
 *     the handler as a GC root, exactly as they do in the host). Recording is
 *     the honest thing — `route {}` / `tool {}` keep meaning "register", they
 *     no longer mean "reachable from an HTTP request", because there is no
 *     server in this build to serve them.
 *   - bridge_run() refuses loudly instead of pretending to serve. The `run`
 *     builtin calls it; a compiler that silently "succeeds" at starting a
 *     server it does not have is worse than one that says no.
 */

#include "lume.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

void bridge_init(VM *vm)
{
    (void)vm;
    /* The host bound a file-static VM pointer here because agenthttpd's C
     * callbacks had no per-call VM. The recording functions below are called
     * with vm explicitly, and there is no callback table to keep in sync. */
}

int bridge_define_route(VM *vm, const char *method, const char *path,
                        Value handler, const char *label)
{
    if (vm->route_count >= MAX_AL_ROUTES) return -1;
    if (!method || !path) return -1;
    if (!IS_OBJ(handler) ||
        (AS_OBJ(handler)->type != OBJ_FUNC && AS_OBJ(handler)->type != OBJ_NATIVE))
        return -1;

    RouteRec *r = &vm->routes[vm->route_count++];
    r->method = strdup(method);
    r->path = strdup(path);
    r->label = label ? strdup(label) : NULL;
    r->handler = handler;
    return 0;
}

int bridge_define_tool(VM *vm, const char *name, const char *desc,
                       const char *params_json, Value handler)
{
    if (vm->tool_count >= MAX_AL_TOOLS) return -1;
    if (!name) return -1;
    if (!IS_OBJ(handler) ||
        (AS_OBJ(handler)->type != OBJ_FUNC && AS_OBJ(handler)->type != OBJ_NATIVE))
        return -1;

    ToolRec *rec = &vm->tool_records[vm->tool_count];
    memset(rec, 0, sizeof(*rec));
    snprintf(rec->name, sizeof(rec->name), "%s", name);
    snprintf(rec->desc, sizeof(rec->desc), "%s", desc ? desc : "");
    snprintf(rec->params, sizeof(rec->params), "%s",
             params_json ? params_json : "{}");
    rec->handler = handler;
    vm->tool_count++;
    return 0;
}

void bridge_run(VM *vm)
{
    (void)vm;
    fprintf(stderr,
            "lume: run() needs the agent-httpd host build (work/research/lume).\n"
            "      This tree is the standalone compiler: routes and tools are\n"
            "      registered but nothing serves them. Use --compile or run the\n"
            "      script with the interpreter instead.\n");
    exit(2);
}

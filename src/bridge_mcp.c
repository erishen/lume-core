/* bridge_mcp.c — MCP stdio server for lume-core (`--mcp`).
 *
 * Model Context Protocol over the stdio transport: newline-delimited JSON-RPC
 * 2.0 on stdin/stdout. The tool table is the VM's own ToolRec[] filled by DSL
 * `tool "name", "desc", {params}, handler;` statements, so a script can expose
 * any lume function as an AI-callable tool.
 *
 *   initialize       -> capabilities (tools)
 *   tools/list       -> registered tools (name / description / inputSchema)
 *   tools/call       -> decode arguments (JSON) and run the handler
 *
 * Windows: stdin/stdout are switched to binary mode so \n framing is exact.
 */

#include "lume.h"
#include "sbuf.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#ifdef _WIN32
#include <fcntl.h>
#include <io.h>
#endif

/* JSON-escape a C string into a JSON string literal. */
static void json_esc(sbuf *b, const char *s) {
    sb_chr(b, '"');
    for (const char *p = s; *p; p++) {
        unsigned char c = (unsigned char)*p;
        switch (c) {
        case '"':  sb_str(b, "\\\""); break;
        case '\\': sb_str(b, "\\\\"); break;
        case '\n': sb_str(b, "\\n");  break;
        case '\r': sb_str(b, "\\r");  break;
        case '\t': sb_str(b, "\\t");  break;
        default:
            if (c < 0x20) {
                char esc[7];
                snprintf(esc, sizeof esc, "\\u%04x", c);
                sb_str(b, esc);
            } else {
                sb_chr(b, (char)c);
            }
        }
    }
    sb_chr(b, '"');
}

/* Write one JSON-RPC response line: `result` member, or `error` member. */
static void mcp_write(long long id, const char *result_json,
                      int err_code, const char *err_msg)
{
    char line[65536];
    if (err_code) {
        sbuf b = {0};
        sb_str(&b, "{\"jsonrpc\":\"2.0\",\"id\":");
        char idb[32];
        snprintf(idb, sizeof idb, "%lld", id);
        sb_str(&b, idb);
        sb_str(&b, ",\"error\":{\"code\":");
        char cb[16];
        snprintf(cb, sizeof cb, "%d", err_code);
        sb_str(&b, cb);
        sb_str(&b, ",\"message\":");
        json_esc(&b, err_msg ? err_msg : "error");
        sb_str(&b, "}}\n");
        fwrite(b.p ? b.p : "", 1, b.len, stdout);
        free(b.p);
    } else {
        snprintf(line, sizeof line,
                 "{\"jsonrpc\":\"2.0\",\"id\":%lld,\"result\":%s}\n",
                 id, result_json ? result_json : "{}");
        fputs(line, stdout);
    }
    fflush(stdout);
}

/* Serialise a lume value to JSON text (append into b). */
static void value_to_json(VM *vm, sbuf *b, Value v) {
    json_append_value(vm, b, v);
}

/* Convert a tool's simplified params map ({"k":"string",...}, as written in
 * `tool "n", "d", {..}, h;`) into a standard JSON Schema object:
 * {"type":"object","properties":{"k":{"type":"..."}}}. Mainstream MCP
 * clients (Claude Desktop etc.) validate tools/list against JSON Schema, so
 * the shorthand is normalised here. All properties are optional: handlers
 * fall back to defaults via get(params, k, def). */
static void mcp_schema_json(VM *vm, const char *params_json, sbuf *b) {
    if (!params_json || !params_json[0]) {
        sb_str(b, "{\"type\":\"object\"}");
        return;
    }
    char jerr[256] = {0};
    json_parse(vm, params_json, jerr, sizeof jerr);
    if (vm->error) {
        vm->error = false;
        vm->error_msg[0] = '\0';
        sb_str(b, "{\"type\":\"object\"}");
        return;
    }
    Value mv = vm_pop(vm);
    sb_str(b, "{\"type\":\"object\",\"properties\":{");
    if (IS_OBJ(mv) && AS_OBJ(mv)->type == OBJ_MAP) {
        Obj *m = AS_OBJ(mv);
        int first = 1;
        for (int i = 0; i < m->as.map.count; i++) {
            const char *k = m->as.map.keys[i];
            Value v = m->as.map.vals[i];
            const char *t = "string";
            if (IS_OBJ(v) && AS_OBJ(v)->type == OBJ_STRING) {
                const char *s = obj_string(AS_OBJ(v));
                if (strcmp(s, "number") == 0 || strcmp(s, "float") == 0)
                    t = "number";
                else if (strcmp(s, "int") == 0) t = "integer";
                else if (strcmp(s, "bool") == 0) t = "boolean";
                else if (strcmp(s, "array") == 0) t = "array";
                else if (strcmp(s, "object") == 0) t = "object";
            }
            if (!first) sb_str(b, ",");
            first = 0;
            json_esc(b, k);
            sb_str(b, ":{\"type\":");
            json_esc(b, t);
            sb_str(b, "}");
        }
    }
    sb_str(b, "}}");
}

/* tools/list: name / description / inputSchema for every registered tool. */
static void mcp_tools_list(VM *vm, long long id) {
    sbuf b = {0};
    sb_str(&b, "{\"tools\":[");
    for (int i = 0; i < vm->tool_count; i++) {
        ToolRec *r = &vm->tool_records[i];
        if (i) sb_str(&b, ",");
        sb_str(&b, "{\"name\":");
        json_esc(&b, r->name);
        sb_str(&b, ",\"description\":");
        json_esc(&b, r->desc);
        sb_str(&b, ",\"inputSchema\":");
        /* params was stored as JSON text (bridge_define_tool snprintf'd it);
         * normalise the shorthand into a standard JSON Schema. */
        mcp_schema_json(vm, r->params, &b);
        sb_str(&b, "}");
    }
    sb_str(&b, "]}");
    mcp_write(id, b.p ? b.p : "{}", 0, NULL);
    free(b.p);
}

/* tools/call: find the tool by name and run its handler with the decoded
 * arguments (one map argument; the handler sees `(params) => ...`). */
static void mcp_tools_call(VM *vm, long long id, Obj *params) {
    int found = 0;
    Value namev = map_get(vm, params, "name", &found);
    if (!found || !IS_OBJ(namev) || AS_OBJ(namev)->type != OBJ_STRING) {
        mcp_write(id, NULL, -32602, "missing tool name");
        return;
    }
    const char *name = obj_string(AS_OBJ(namev));

    ToolRec *rec = NULL;
    for (int i = 0; i < vm->tool_count; i++) {
        if (strcmp(vm->tool_records[i].name, name) == 0) { rec = &vm->tool_records[i]; break; }
    }
    if (!rec) {
        mcp_write(id, NULL, -32602, "unknown tool");
        return;
    }

    Value argv = val_null();
    int af = 0;
    Value av = map_get(vm, params, "arguments", &af);
    if (af) argv = av;

    /* callee + one argument on the stack, then call. */
    vm_push(vm, rec->handler);
    vm_push(vm, argv);
    call_function(vm, rec->handler, 1);
    Value result = vm_pop(vm);

    sbuf b = {0};
    sb_str(&b, "{\"content\":[{\"type\":\"text\",\"text\":");
    if (vm->error) {
        json_esc(&b, vm->error_msg[0] ? vm->error_msg : "tool error");
        sb_str(&b, "}],\"isError\":true}");
        vm->error = false;
        vm->error_msg[0] = '\0';
    } else {
        /* string result -> plain text; map/list/number -> JSON text */
        if (IS_OBJ(result) && AS_OBJ(result)->type == OBJ_STRING) {
            json_esc(&b, obj_string(AS_OBJ(result)));
        } else {
            sbuf rj = {0};
            value_to_json(vm, &rj, result);
            json_esc(&b, rj.p ? rj.p : "null");
            free(rj.p);
        }
        sb_str(&b, "}],\"isError\":false}");
    }
    mcp_write(id, b.p ? b.p : "{}", 0, NULL);
    free(b.p);
}

void mcp_run(VM *vm) {
#ifdef _WIN32
    _setmode(_fileno(stdin), _O_BINARY);
    _setmode(_fileno(stdout), _O_BINARY);
#endif
    /* stdout carries the JSON-RPC protocol; stderr is free for diagnostics
     * so a client can see why a connection is not answering. */
    fprintf(stderr, "lume-mcp: %d tool(s) registered, ready on stdio\n",
            vm->tool_count);
    char line[65536];
    while (fgets(line, sizeof line, stdin)) {
        size_t n = strlen(line);
        while (n && (line[n - 1] == '\n' || line[n - 1] == '\r')) line[--n] = '\0';
        if (n == 0) continue;

        char jerr[256] = {0};
        json_parse(vm, line, jerr, sizeof jerr);
        if (vm->error) {
            mcp_write(0, NULL, -32700, jerr[0] ? jerr : "parse error");
            vm->error = false;
            vm->error_msg[0] = '\0';
            continue;
        }
        Value msg = vm_pop(vm); /* json_parse pushed the decoded message */
        if (!IS_OBJ(msg) || AS_OBJ(msg)->type != OBJ_MAP) {
            mcp_write(0, NULL, -32600, "invalid request");
            continue;
        }
        Obj *m = AS_OBJ(msg);

        /* id: int or absent (notification) — JSON numbers decode as VAL_FLOAT
         * via strtod, so accept both int and float. */
        long long id = 0;
        int idf = 0;
        Value idv = map_get(vm, m, "id", &idf);
        if (idf && IS_INT(idv)) id = AS_INT(idv);
        else if (idf && IS_FLOAT(idv)) id = (long long)AS_NUM(idv);
        int notif = !idf;

        int mf = 0;
        Value methodv = map_get(vm, m, "method", &mf);
        const char *method = NULL;
        if (mf && IS_OBJ(methodv) && AS_OBJ(methodv)->type == OBJ_STRING)
            method = obj_string(AS_OBJ(methodv));

        if (!method) {
            if (!notif) mcp_write(id, NULL, -32600, "missing method");
            continue;
        }

        /* params: map (optional) */
        Obj *params = NULL;
        int pf = 0;
        Value pv = map_get(vm, m, "params", &pf);
        if (pf && IS_OBJ(pv) && AS_OBJ(pv)->type == OBJ_MAP) params = AS_OBJ(pv);

        if (strcmp(method, "initialize") == 0) {
            mcp_write(id,
                "{\"protocolVersion\":\"2024-11-05\","
                "\"capabilities\":{\"tools\":{\"listChanged\":false}},"
                "\"serverInfo\":{\"name\":\"lume-core\",\"version\":\"0.5.0\"}}",
                0, NULL);
        } else if (strcmp(method, "notifications/initialized") == 0) {
            /* no reply to notifications */
        } else if (strcmp(method, "tools/list") == 0) {
            mcp_tools_list(vm, id);
        } else if (strcmp(method, "tools/call") == 0) {
            if (!params) mcp_write(id, NULL, -32602, "missing params");
            else mcp_tools_call(vm, id, params);
        } else {
            if (!notif) mcp_write(id, NULL, -32601, "method not found");
        }
    }
    fprintf(stderr, "lume-mcp: stdin EOF, exiting\n");
}

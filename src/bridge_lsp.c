/* bridge_lsp.c — Language Server Protocol for lume-core (`--lsp`).
 *
 * A stdio JSON-RPC 2.0 server using LSP framing (Content-Length headers),
 * the same transport family as bridge_mcp.c. Minimal-but-useful set:
 *
 *   initialize / initialized / shutdown / exit
 *   textDocument/didOpen | didChange  -> parse + typecheck -> publishDiagnostics
 *   textDocument/hover                -> builtin name note
 *   textDocument/completion           -> builtin name list
 *
 * Diagnostics come from the "line N: ..." error prefix emitted by
 * parse_program / type_check_program (1-based line; no column yet), mapped
 * onto the 0-based LSP line. Open documents are kept per-uri so didChange
 * can re-check the latest text (full sync, textDocumentSync = 1).
 */

#include "lume.h"
#include "sbuf.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <ctype.h>
#ifdef _WIN32
#include <fcntl.h>
#include <io.h>
#endif

/* ---- open documents (uri -> latest text) ---- */
typedef struct LspDoc {
    char *uri;
    char *text;
    struct LspDoc *next;
} LspDoc;
static LspDoc *s_docs = NULL;

static LspDoc *lsp_doc_get(const char *uri) {
    for (LspDoc *d = s_docs; d; d = d->next)
        if (strcmp(d->uri, uri) == 0) return d;
    return NULL;
}

static LspDoc *lsp_doc_put(const char *uri, const char *text) {
    LspDoc *d = lsp_doc_get(uri);
    if (!d) {
        d = (LspDoc *)calloc(1, sizeof *d);
        d->uri = strdup(uri);
        d->next = s_docs;
        s_docs = d;
    }
    free(d->text);
    d->text = strdup(text);
    return d;
}

/* ---- open documents (uri -> latest text) ---- */
/* ---- LSP framing: Content-Length headers + JSON body ---- */
static char *lsp_read_frame(void) {
    long clen = -1;
    char line[1024];
    for (;;) {
        if (!fgets(line, sizeof line, stdin)) return NULL;
        if (line[0] == '\n' || line[0] == '\r') break;   /* blank line */
        if (strncmp(line, "Content-Length:", 15) == 0)
            clen = strtol(line + 15, NULL, 10);
    }
    if (clen < 0 || clen > (1 << 24)) return NULL;
    char *body = (char *)malloc((size_t)clen + 1);
    if (!body) return NULL;
    size_t got = 0;
    while (got < (size_t)clen) {
        size_t n = fread(body + got, 1, (size_t)clen - got, stdin);
        if (n == 0) { free(body); return NULL; }
        got += n;
    }
    body[clen] = '\0';
    return body;
}

static void lsp_write(const char *json) {
    printf("Content-Length: %zu\r\n\r\n%s", strlen(json), json);
    fflush(stdout);
}

/* ---- reply / notification builders ---- */
static void lsp_id_json(sbuf *b, Value idv, int idf) {
    if (!idf) { sb_str(b, "null"); return; }
    if (IS_INT(idv)) {
        char nb[32];
        snprintf(nb, sizeof nb, "%lld", AS_INT(idv));
        sb_str(b, nb);
    } else if (IS_FLOAT(idv)) {
        char nb[32];
        snprintf(nb, sizeof nb, "%lld", (long long)AS_NUM(idv));
        sb_str(b, nb);
    } else if (IS_OBJ(idv) && AS_OBJ(idv)->type == OBJ_STRING) {
        json_esc(b, obj_string(AS_OBJ(idv)));
    } else {
        sb_str(b, "null");
    }
}

static void lsp_reply(Value idv, int idf, const char *result_json) {
    sbuf b = {0};
    sb_str(&b, "{\"jsonrpc\":\"2.0\",\"id\":");
    lsp_id_json(&b, idv, idf);
    sb_str(&b, ",\"result\":");
    sb_str(&b, result_json);
    sb_str(&b, "}");
    lsp_write(b.p ? b.p : "");
    free(b.p);
}

static void lsp_error(Value idv, int idf, int code, const char *msg) {
    sbuf b = {0};
    sb_str(&b, "{\"jsonrpc\":\"2.0\",\"id\":");
    lsp_id_json(&b, idv, idf);
    sb_str(&b, ",\"error\":{\"code\":");
    char cb[16];
    snprintf(cb, sizeof cb, "%d", code);
    sb_str(&b, cb);
    sb_str(&b, ",\"message\":");
    json_esc(&b, msg);
    sb_str(&b, "}}");
    lsp_write(b.p ? b.p : "");
    free(b.p);
}

static void lsp_notify(const char *method, const char *params_json) {
    sbuf b = {0};
    sb_str(&b, "{\"jsonrpc\":\"2.0\",\"method\":");
    json_esc(&b, method);
    sb_str(&b, ",\"params\":");
    sb_str(&b, params_json);
    sb_str(&b, "}");
    lsp_write(b.p ? b.p : "");
    free(b.p);
}

/* ---- diagnostics ---- */
static void lsp_diagnose(VM *vm, const char *uri, const char *text) {
    (void)vm;   /* parse + typecheck are standalone: no VM state needed */
    char perr[512] = {0};
    Node *prog = parse_program(text, perr, sizeof perr);
    const char *msg = NULL;
    if (!prog) {
        msg = perr[0] ? perr : "parse error";
    } else {
        char terr[512] = {0};
        if (!type_check_program(prog, terr, sizeof terr))
            msg = terr[0] ? terr : "type error";
        node_free(prog);
        type_release_all();
    }
    /* "line N: ..." — 1-based; LSP wants 0-based. */
    size_t line = 0;
    if (msg) sscanf(msg, "line %zu: ", &line);
    if (!line) line = 1;

    sbuf b = {0};
    sb_str(&b, "{\"uri\":");
    json_esc(&b, uri);
    sb_str(&b, ",\"diagnostics\":[");
    if (msg) {
        char nb[32];
        snprintf(nb, sizeof nb, "%llu", (unsigned long long)(line - 1));
        sb_str(&b, "{\"range\":{\"start\":{\"line\":");
        sb_str(&b, nb);
        sb_str(&b, ",\"character\":0},\"end\":{\"line\":");
        sb_str(&b, nb);
        sb_str(&b, ",\"character\":0}},\"severity\":1,\"source\":\"lume-core\",\"message\":");
        json_esc(&b, msg);
        sb_str(&b, "}");
    }
    sb_str(&b, "]}");
    lsp_notify("textDocument/publishDiagnostics", b.p ? b.p : "");
    free(b.p);
}

/* ---- hover / completion ---- */
/* Identifier at (line0, char0) (0-based, LSP convention); malloc'd copy. */
static char *word_at(const char *text, long line0, long ch) {
    long cur = 0;
    const char *lstart = text;
    while (cur < line0 && *lstart) {
        if (*lstart == '\n') cur++;
        lstart++;
    }
    long n = 0;
    const char *p = lstart;
    while (*p && *p != '\n') {
        if (n == ch) break;
        n++;
        p++;
    }
    const char *s = p;
    while (s > lstart && (isalnum((unsigned char)s[-1]) || s[-1] == '_')) s--;
    const char *e = p;
    while (*e && (isalnum((unsigned char)*e) || *e == '_')) e++;
    if (e == s) return NULL;
    size_t len = (size_t)(e - s);
    char *out = (char *)malloc(len + 1);
    memcpy(out, s, len);
    out[len] = '\0';
    return out;
}

static void lsp_hover(VM *vm, Obj *params, Value idv, int idf) {
    Obj *td = NULL;
    int tf = 0;
    Value tv = map_get(vm, params, "textDocument", &tf);
    if (tf && IS_OBJ(tv) && AS_OBJ(tv)->type == OBJ_MAP) td = AS_OBJ(tv);
    if (!td) { lsp_error(idv, idf, -32602, "missing textDocument"); return; }
    int uf = 0;
    Value uv = map_get(vm, td, "uri", &uf);
    if (!uf || !IS_OBJ(uv) || AS_OBJ(uv)->type != OBJ_STRING) {
        lsp_error(idv, idf, -32602, "missing textDocument.uri");
        return;
    }
    const char *uri = obj_string(AS_OBJ(uv));
    LspDoc *doc = lsp_doc_get(uri);
    if (!doc) { lsp_reply(idv, idf, "null"); return; }

    long line = 0, ch = 0;
    int pf = 0;
    Value pv = map_get(vm, params, "position", &pf);
    if (pf && IS_OBJ(pv) && AS_OBJ(pv)->type == OBJ_MAP) {
        int lf = 0;
        Value lv = map_get(vm, AS_OBJ(pv), "line", &lf);
        if (lf && IS_NUM(lv)) line = (long)AS_NUM(lv);
        int hf = 0;
        Value hv = map_get(vm, AS_OBJ(pv), "character", &hf);
        if (hf && IS_NUM(hv)) ch = (long)AS_NUM(hv);
    }

    char *word = word_at(doc->text, line, ch);
    if (word && is_builtin_name(word)) {
        sbuf b = {0};
        sb_str(&b, "{\"contents\":{\"kind\":\"markdown\",\"value\":\"**lume-core** builtin: \\`");
        sb_str(&b, word);
        sb_str(&b, "\\`\\n\\nDeclared in the typechecker's builtin table; see docs/SPEC.md \\u00a76 for semantics.\"}}");
        lsp_reply(idv, idf, b.p ? b.p : "null");
        free(b.p);
    } else {
        lsp_reply(idv, idf, "null");
    }
    free(word);
}

static void lsp_completion(Value idv, int idf) {
    sbuf b = {0};
    sb_str(&b, "{\"isIncomplete\":false,\"items\":[");
    for (size_t i = 0; i < LUME_BUILTIN_COUNT; i++) {
        if (i) sb_str(&b, ",");
        sb_str(&b, "{\"label\":");
        json_esc(&b, LUME_BUILTIN_NAMES[i]);
        sb_str(&b, ",\"kind\":3}");
    }
    sb_str(&b, "]}");
    lsp_reply(idv, idf, b.p ? b.p : "");
    free(b.p);
}

/* ---- didOpen / didChange ---- */
static void lsp_did_open(VM *vm, Obj *params) {
    Obj *td = NULL;
    int tf = 0;
    Value tv = map_get(vm, params, "textDocument", &tf);
    if (tf && IS_OBJ(tv) && AS_OBJ(tv)->type == OBJ_MAP) td = AS_OBJ(tv);
    if (!td) return;
    int uf = 0;
    Value uv = map_get(vm, td, "uri", &uf);
    int xf = 0;
    Value xv = map_get(vm, td, "text", &xf);
    if (!uf || !xf || !IS_OBJ(uv) || AS_OBJ(uv)->type != OBJ_STRING ||
        !IS_OBJ(xv) || AS_OBJ(xv)->type != OBJ_STRING)
        return;
    const char *uri = obj_string(AS_OBJ(uv));
    const char *text = obj_string(AS_OBJ(xv));
    lsp_doc_put(uri, text);
    lsp_diagnose(vm, uri, text);
}

static void lsp_did_change(VM *vm, Obj *params) {
    Obj *td = NULL;
    int tf = 0;
    Value tv = map_get(vm, params, "textDocument", &tf);
    if (tf && IS_OBJ(tv) && AS_OBJ(tv)->type == OBJ_MAP) td = AS_OBJ(tv);
    if (!td) return;
    int uf = 0;
    Value uv = map_get(vm, td, "uri", &uf);
    if (!uf || !IS_OBJ(uv) || AS_OBJ(uv)->type != OBJ_STRING) return;
    const char *uri = obj_string(AS_OBJ(uv));
    /* contentChanges[0].text — full sync (textDocumentSync = 1) */
    const char *text = NULL;
    int cf = 0;
    Value cv = map_get(vm, params, "contentChanges", &cf);
    if (cf && IS_OBJ(cv) && AS_OBJ(cv)->type == OBJ_LIST &&
        AS_OBJ(cv)->as.list.count > 0) {
        Value c0 = AS_OBJ(cv)->as.list.items[0];
        if (IS_OBJ(c0) && AS_OBJ(c0)->type == OBJ_MAP) {
            int xf = 0;
            Value xv = map_get(vm, AS_OBJ(c0), "text", &xf);
            if (xf && IS_OBJ(xv) && AS_OBJ(xv)->type == OBJ_STRING)
                text = obj_string(AS_OBJ(xv));
        }
    }
    if (!text) return;
    lsp_doc_put(uri, text);
    lsp_diagnose(vm, uri, text);
}

/* ---- main loop ---- */
void lsp_run(VM *vm) {
#ifdef _WIN32
    _setmode(_fileno(stdin), _O_BINARY);
    _setmode(_fileno(stdout), _O_BINARY);
#endif
    fprintf(stderr, "lume-lsp: ready on stdio (Content-Length framing)\n");

    char *raw;
    while ((raw = lsp_read_frame()) != NULL) {
        char jerr[256] = {0};
        json_parse(vm, raw, jerr, sizeof jerr);
        free(raw);
        if (vm->error) {
            lsp_error(val_null(), 0, -32700, jerr[0] ? jerr : "parse error");
            vm->error = false;
            vm->error_msg[0] = '\0';
            continue;
        }
        Value msg = vm_pop(vm);
        if (!IS_OBJ(msg) || AS_OBJ(msg)->type != OBJ_MAP) {
            lsp_error(val_null(), 0, -32600, "invalid request");
            continue;
        }
        Obj *m = AS_OBJ(msg);

        int idf = 0;
        Value idv = map_get(vm, m, "id", &idf);

        int mf = 0;
        Value methodv = map_get(vm, m, "method", &mf);
        const char *method = NULL;
        if (mf && IS_OBJ(methodv) && AS_OBJ(methodv)->type == OBJ_STRING)
            method = obj_string(AS_OBJ(methodv));
        if (!method) {
            if (idf) lsp_error(idv, idf, -32600, "missing method");
            continue;
        }

        Obj *params = NULL;
        int pf = 0;
        Value pv = map_get(vm, m, "params", &pf);
        if (pf && IS_OBJ(pv) && AS_OBJ(pv)->type == OBJ_MAP) params = AS_OBJ(pv);

        if (strcmp(method, "initialize") == 0) {
            lsp_reply(idv, idf,
                "{\"capabilities\":{"
                "\"textDocumentSync\":1,"
                "\"hoverProvider\":true,"
                "\"completionProvider\":{\"triggerCharacters\":[\".\"]}"
                "},\"serverInfo\":{\"name\":\"lume-core\",\"version\":\"0.5.0\"}}");
        } else if (strcmp(method, "initialized") == 0) {
            /* notification: no reply */
        } else if (strcmp(method, "shutdown") == 0) {
            lsp_reply(idv, idf, "null");
            /* keep reading; LSP clients send exit next */
        } else if (strcmp(method, "exit") == 0) {
            break;
        } else if (strcmp(method, "textDocument/didOpen") == 0) {
            if (params) lsp_did_open(vm, params);
        } else if (strcmp(method, "textDocument/didChange") == 0) {
            if (params) lsp_did_change(vm, params);
        } else if (strcmp(method, "textDocument/hover") == 0) {
            if (params) lsp_hover(vm, params, idv, idf);
            else lsp_error(idv, idf, -32602, "missing params");
        } else if (strcmp(method, "textDocument/completion") == 0) {
            lsp_completion(idv, idf);
        } else {
            if (idf) lsp_error(idv, idf, -32601, "method not found");
        }
    }
    fprintf(stderr, "lume-lsp: stdin EOF, exiting\n");
}

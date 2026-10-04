/* Virtual DOM for Lume SSR (el / render / html) — split out of
 * interp.c (2026-09-25 refactor; behavior unchanged). el() builds a vnode
 * tree, render() serializes it to HTML; html() is the template-string shell
 * (see the block comment in native_html for slot rules). */

#include "builtins.h"
#include "sbuf.h"

/* ---------- Virtual DOM: el(tag, props, ...children) / render(vnode)
 * (reconstructed) ----------
 * SSR story: Lume builds a vnode tree with el() and render() serializes it
 * to HTML once (html() is the template-string shell that mixes markup and
 * slots, deferring to render logic for vnode/list slots). Text and attribute
 * values are escaped; `on*` props are dropped (event handlers run
 * client-side); `data_page` is emitted as `data-page` so it stays a valid
 * Lume identifier. */

static void el_grow(Obj *list) {
    if (list->as.list.count == list->as.list.cap) {
        list->as.list.cap = list->as.list.cap ? list->as.list.cap * 2 : 8;
        list->as.list.items = realloc(list->as.list.items,
                                      sizeof(Value) * (size_t)list->as.list.cap);
    }
}

static void html_escape(sbuf *b, const char *s, size_t n) {
    for (size_t i = 0; i < n; i++) {
        switch (s[i]) {
            case '&': sb_str(b, "&amp;"); break;
            case '<': sb_str(b, "&lt;"); break;
            case '>': sb_str(b, "&gt;"); break;
            case '"': sb_str(b, "&quot;"); break;
            case '\'': sb_str(b, "&#39;"); break;
            default:  sb_chr(b, s[i]);
        }
    }
}

/* HTML void elements: render as <tag> with no closing tag. */
static bool is_void_tag(const char *tag) {
    static const char *const void_tags[] = {
        "area", "base", "br", "col", "embed", "hr", "img", "input",
        "link", "meta", "param", "source", "track", "wbr", NULL
    };
    for (int i = 0; void_tags[i]; i++)
        if (strcmp(tag, void_tags[i]) == 0) return true;
    return false;
}

static void scalar_into(sbuf *b, Value v) {
    if (IS_NULL(v)) return;
    if (IS_BOOL(v)) { sb_str(b, AS_BOOL(v) ? "true" : "false"); return; }
    if (IS_NUM(v)) {
        char buf[64];
        double d = AS_NUM(v);
        if (d == (long long)d)
            snprintf(buf, sizeof(buf), "%lld", (long long)d);
        else
            snprintf(buf, sizeof(buf), "%g", d);
        sb_str(b, buf);
        return;
    }
    if (IS_OBJ(v) && AS_OBJ(v)->type == OBJ_STRING)
        html_escape(b, obj_string(AS_OBJ(v)), obj_string_len(AS_OBJ(v)));
}

static void render_node(VM *vm, sbuf *b, Value v);

static void element_to_sb(VM *vm, sbuf *b, Obj *o) {
    int found = 0;
    Value tagv = map_get(vm, o, "type", &found);
    const char *tag = (found && IS_OBJ(tagv) && AS_OBJ(tagv)->type == OBJ_STRING)
                          ? obj_string(AS_OBJ(tagv))
                          : "div";
    sb_chr(b, '<');
    sb_str(b, tag);
    int pfound = 0;
    Value pv = map_get(vm, o, "props", &pfound);
    if (pfound && IS_OBJ(pv) && AS_OBJ(pv)->type == OBJ_MAP) {
        Obj *props = AS_OBJ(pv);
        for (int i = 0; i < props->as.map.count; i++) {
            const char *key = props->as.map.keys[i];
            Value vv = props->as.map.vals[i];
            /* client-side handlers, null props and `false` values are
             * dropped server-side (`hidden: true` still renders) */
            if (strncmp(key, "on", 2) == 0 || IS_NULL(vv) ||
                (IS_BOOL(vv) && !AS_BOOL(vv)))
                continue;
            const char *attr = strcmp(key, "data_page") == 0 ? "data-page" : key;
            sb_str(b, " ");
            sb_str(b, attr);
            sb_str(b, "=\"");
            scalar_into(b, vv);
            sb_chr(b, '"');
        }
    }
    sb_str(b, ">");
    int cfound = 0;
    Value cv = map_get(vm, o, "children", &cfound);
    if (cfound && IS_OBJ(cv) && AS_OBJ(cv)->type == OBJ_LIST) {
        Obj *cs = AS_OBJ(cv);
        for (int i = 0; i < cs->as.list.count; i++)
            render_node(vm, b, cs->as.list.items[i]);
    }
    if (!is_void_tag(tag)) {
        sb_str(b, "</");
        sb_str(b, tag);
        sb_chr(b, '>');
    }
}

static void render_node(VM *vm, sbuf *b, Value v) {
    if (!IS_OBJ(v)) { scalar_into(b, v); return; }
    Obj *o = AS_OBJ(v);
    if (o->type == OBJ_STRING) { html_escape(b, obj_string(o), obj_string_len(o)); return; }
    if (o->type == OBJ_LIST) {
        for (int i = 0; i < o->as.list.count; i++)
            render_node(vm, b, o->as.list.items[i]);
        return;
    }
    if (o->type == OBJ_MAP) { element_to_sb(vm, b, o); return; }
    scalar_into(b, v);
}

/* html() slot injection: vnodes render structurally, a list flattens with
 * the same rule, and strings are escaped to HTML just like el() text
 * children — EXCEPT strings produced by html() itself (trusted_html), which
 * are already-rendered markup and compose raw into the outer shell. */
static void slot_into(VM *vm, sbuf *b, Value v) {
    if (IS_OBJ(v)) {
        Obj *o = AS_OBJ(v);
        if (o->type == OBJ_STRING) {
            if (o->as.str.trusted_html)
                sb_mem(b, obj_string(o), obj_string_len(o));
            else
                html_escape(b, obj_string(o), obj_string_len(o));
            return;
        }
        if (o->type == OBJ_LIST) {
            for (int i = 0; i < o->as.list.count; i++)
                slot_into(vm, b, o->as.list.items[i]);
            return;
        }
        if (o->type == OBJ_MAP) { element_to_sb(vm, b, o); return; }
    }
    scalar_into(b, v);
}

static void native_el(VM *vm, int argc, Value *args, Value *out) {
    if (argc < 2 || !IS_OBJ(args[0]) || AS_OBJ(args[0])->type != OBJ_STRING) {
        vm_set_error(vm, "el() needs (tag, props, ...children)");
        return;
    }
    if (!IS_OBJ(args[1]) || AS_OBJ(args[1])->type != OBJ_MAP)
        args[1] = make_map(vm);
    vm_push(vm, make_map(vm));
    Obj *m = AS_OBJ(vm->stack[vm->stack_count - 1]);
    map_set(vm, m, "type", args[0]);
    map_set(vm, m, "props", args[1]);
    vm_push(vm, make_list(vm));
    Obj *kids = AS_OBJ(vm->stack[vm->stack_count - 1]);
    for (int i = 2; i < argc; i++) {
        el_grow(kids);
        kids->as.list.items[kids->as.list.count++] = args[i];
    }
    map_set(vm, m, "children", vm_pop(vm));
    vm_pop(vm);
    *out = val_obj(m);
}

static void native_render(VM *vm, int argc, Value *args, Value *out) {
    if (argc < 1) { vm_set_error(vm, "render() needs a vnode"); return; }
    sbuf b = {0};
    render_node(vm, &b, args[0]);
    *out = make_string(vm, b.p ? b.p : "", b.len);
    free(b.p);
}

static void native_html(VM *vm, int argc, Value *args, Value *out) {
    if (argc < 1 || !IS_OBJ(args[0]) || AS_OBJ(args[0])->type != OBJ_STRING) {
        vm_set_error(vm, "html() needs (template, ...slots)");
        return;
    }
    size_t n = obj_string_len(AS_OBJ(args[0]));
    const char *t = obj_string(AS_OBJ(args[0]));
    sbuf b = {0};
    const char *p = t, *end = t + n;
    while (p < end) {
        /* `{{` and `}}` are literal braces (mustache escape) */
        if (*p == '{' && p + 1 < end && p[1] == '{') { sb_chr(&b, '{'); p += 2; continue; }
        if (*p == '}' && p + 1 < end && p[1] == '}') { sb_chr(&b, '}'); p += 2; continue; }
        if (*p == '{' && p + 1 < end && p[1] >= '0' && p[1] <= '9') {
            const char *q = p + 1;
            long idx = 0;
            while (q < end && *q >= '0' && *q <= '9') { idx = idx * 10 + (*q - '0'); q++; }
            if (q < end && *q == '}' && idx < argc - 1) {
                slot_into(vm, &b, args[idx + 1]);
                p = q + 1;
                continue;
            }
        }
        sb_chr(&b, *p);
        p++;
    }
    *out = make_string(vm, b.p ? b.p : "", b.len);
    free(b.p);
    /* html() returns already-rendered markup: mark it so a slot inside an
     * outer html() shell injects it raw instead of escaping it again. */
    if (IS_OBJ(*out) && AS_OBJ(*out)->type == OBJ_STRING)
        AS_OBJ(*out)->as.str.trusted_html = true;
}
Value b_el(VM *vm, int argc, Value *args)        { return vm_native(vm, argc, args, native_el); }
Value b_render(VM *vm, int argc, Value *args)    { return vm_native(vm, argc, args, native_render); }
Value b_html(VM *vm, int argc, Value *args)      { return vm_native(vm, argc, args, native_html); }

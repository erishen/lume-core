/* llvm_cg_lit.inc - LLVM backend lit functions.
 * Textually included by llvm_codegen.c (single translation unit).
 * Do not compile this file on its own. */

static int struct_field_idx(CG *g, const char *sname, const char *fname)
{
    SDef *sd = sdef_find(&g->structs, sname);
    if (!sd) return -1;
    for (int i = 0; i < sd->count; i++)
        if (strcmp(sd->names[i], fname) == 0) return i;
    return -1;
}

static Type *struct_field_type(CG *g, const char *sname, int idx)
{
    SDef *sd = sdef_find(&g->structs, sname);
    if (sd && idx >= 0 && idx < sd->count) return sd->types[idx];
    return NULL;
}

/* Address of field `idx` of *ptrv* (a pointer to a struct value).
 * LLVM ≥ 15 keeps pointers opaque, so the pointee cannot be recovered from
 * the pointer: on LLVM 23 LLVMTypeOf(ptrv) is a bare `ptr` and
 * LLVMGetElementType on it yields `half`, which turns the GEP into
 * `getelementptr inbounds nuw half, ptr %0, i32 0, i32 0` — the verifier
 * rejects it ("Invalid indices for GEP pointer type!"). The struct type is
 * therefore spelled in from the frontend's type node, which is authoritative
 * for both the member read (cg_member) and the member write (cg_assign_mem).
 *
 * One index, not the text backend's double index, so the offset-16
 * wrong-field trap it dances around does not exist here. */
static LLVMValueRef gep_field(CG *g, Type *sty, LLVMValueRef ptrv, int idx)
{
    LLVMTypeRef et = ty_of(g, sty);
    if (!et) return NULL;
    return LLVMBuildStructGEP2(g->ab, et, ptrv, (unsigned)idx, "f");
}

static void struct_addr(CG *g, Val *o)
{
    if (!o->ty || o->ty->kind != TY_STRUCT) return;
    if (o->v && struct_is_addr(o->v)) return;      /* already an address */
    LLVMTypeRef st = ty_of(g, o->ty);
    if (!st) return;
    LLVMValueRef slot = LLVMBuildAlloca(g->ab, st, "sa");
    LLVMBuildStore(g->ab, o->v, slot);
    o->v = slot;
}

/* Which getter reads a map value of static type `ty` -- the same choice
 * map_put_fn makes on the way in, read back for `?` unwrapping. Kept in step
 * with codegen_expr.c's copy. */
static const char *map_get_fn(Type *ty)
{
    if (!ty) return NULL;
    switch (ty->kind) {
    case TY_FLOAT:  return "lume_map_get_f";
    case TY_STRING: return "lume_map_get_s";
    case TY_INT:
    case TY_BOOL:   return "lume_map_get_i";
    case TY_LIST:
    case TY_STRUCT:
    case TY_RESULT:
    case TY_ENUM:   return "lume_map_get_obj";
    /* A null default (`get(m, k, null)`) reads through the obj getter, the
     * same choice codegen_expr.c's copy makes. */
    case TY_NULL:   return "lume_map_get_obj";
    default:        return NULL;
    }
}

/* Which push helper an element of static type `ty` needs. The checker keeps a
 * list homogeneous, so one answer serves every element. */
static const char *list_push_fn(Type *ty)
{
    if (!ty) return NULL;
    switch (ty->kind) {
    case TY_FLOAT:  return "lume_list_push_f";
    case TY_STRING: return "lume_list_push_s";
    case TY_BOOL:   return "lume_list_push_b";
    case TY_INT:    return "lume_list_push_i";
    /* Containers as elements -- the same branch, and the same reasoning, as in
     * codegen_expr.c's copy of this function. Kept in step deliberately: these
     * two are separate implementations of one table, and a type added to one
     * but not the other compiles on one backend and not the other. */
    case TY_LIST:
    case TY_STRUCT:
    case TY_RESULT:
    case TY_ENUM:   return "lume_list_push_obj";
    /* A null element rides the obj helper with a null pointer -- the slot
     * keeps its obj tag with a null payload, which every reader and printer
     * already treats as null. */
    case TY_NULL:   return "lume_list_push_obj";
    default:        return NULL;
    }
}

/* Which map helper stores a value of static type `ty`. */
static const char *map_put_fn(Type *ty)
{
    if (!ty) return NULL;
    switch (ty->kind) {
    case TY_FLOAT:  return "lume_map_put_f";
    case TY_STRING: return "lume_map_put_s";
    case TY_BOOL:   return "lume_map_put_b";
    case TY_INT:    return "lume_map_put_i";
    case TY_LIST:
    case TY_STRUCT:
    case TY_RESULT:
    case TY_ENUM:   return "lume_map_put_obj";
    /* Same null rule as list_push_fn: the obj helper stores a null payload,
     * which print/str/eq all render as null. */
    case TY_NULL:   return "lume_map_put_obj";
    default:        return NULL;
    }
}

/* `[a, b, c]` — a heap list, built by pushing every element in source order so
 * iteration and the interpreter's ordering agree. */
static Val cg_list_lit(CG *g, Node *n)
{
    AT(g);
    LLVMTypeRef newp[1] = { g->i8ptr };
    LLVMValueRef nv[1] = { NULL };
    LLVMValueRef l = LLVMBuildCall2(g->ab, LLVMFunctionType(g->i8ptr, NULL, 0, 0),
                                    rt_decl(g, "lume_list_new", g->i8ptr, newp, 0),
                                    nv, 0, "l");
    if (!l) ERRV(g, "line %zu: cannot allocate a list", n->line);

    for (int i = 0; i < n->as.list.count; i++) {
        Val e = cg_expr(g, n->as.list.items[i]);
        const char *fn = list_push_fn(e.ty);
        if (!fn) ERRV(g, "line %zu: list elements must be int, float, string or a container", n->line);
        if (e.ty && e.ty->kind == TY_BOOL) e = coerce(g, type_prim(TY_INT), e, n->line);
        LLVMTypeRef ats[2] = { g->i8ptr, ty_of(g, e.ty) };
        LLVMValueRef vs[2] = { l, e.v };
        Val r = rt_call(g, type_prim(TY_INT), fn, 2, ats, vs);
        if (!r.v) ERRV(g, "line %zu: cannot append to a list", n->line);
    }
    return val_make(list_type(), l);
}

/* A map literal that no declared type claimed is a runtime map, keyed by its
 * source keys in insertion order. */
static Val cg_map_lit(CG *g, Node *n)
{
    AT(g);
    LLVMTypeRef newp[1] = { g->i8ptr };
    LLVMValueRef nv[1] = { NULL };
    LLVMValueRef m = LLVMBuildCall2(g->ab, LLVMFunctionType(g->i8ptr, NULL, 0, 0),
                                    rt_decl(g, "lume_map_new", g->i8ptr, newp, 0),
                                    nv, 0, "m");
    if (!m) ERRV(g, "line %zu: cannot allocate a map", n->line);

    for (int i = 0; i < n->as.map.count; i++) {
        Val v = cg_expr(g, n->as.map.vals[i]);
        Val k = val_make(type_prim(TY_STRING),
                         cg_string_val(g, n->as.map.keys[i], strlen(n->as.map.keys[i])));
        const char *fn = map_put_fn(v.ty);
        if (!fn) ERRV(g, "line %zu: map values must be int, float, string or a container", n->line);
        if (v.ty && v.ty->kind == TY_BOOL) v = coerce(g, type_prim(TY_INT), v, n->line);
        /* The three flavours disagree on the *type* of the third argument as
         * well as on the helper: the string one takes a i8* key, the two
         * scalar ones an i8* key and a numeric value, so both are spelled. */
        LLVMTypeRef ats[3] = { g->i8ptr, g->i8ptr, ty_of(g, v.ty) };
        LLVMValueRef vs[3] = { m, k.v, v.v };
        Val r = rt_call(g, type_prim(TY_INT), fn, 3, ats, vs);
        if (!r.v) ERRV(g, "line %zu: cannot store into a map", n->line);
    }
    /* The map is a heap object like a list; the anonymous struct type only
     * marks the value's provenance. */
    return val_make(type_anon_struct(), m);
}

/* A map literal that no declared type claimed is a runtime map; one that did
 * is a struct value (see cg_struct_lit). */
static Val cg_map_or_struct_lit(CG *g, Node *n)
{
    if (resolve_struct_lit(g, n)) return cg_struct_lit(g, n);
    return cg_map_lit(g, n);
}

/* `{ base with k: v, ... }` — the functional update (SPEC 1.8's sibling).
 * Two shapes by the base's static type, mirroring codegen_expr.c: a named
 * struct is a typed aggregate rebuilt field by field (untouched fields load
 * from the base through gep_field, overridden ones are evaluated and
 * inserted); an anonymous map is cloned by lume_map_clone and then
 * overwritten key by key. */
static Val cg_struct_update(CG *g, Node *n)
{
    AT(g);
    Val base = cg_expr(g, n->as.supd.base);
    if (!base.v) return base;

    if (base.ty && base.ty->kind == TY_STRUCT && base.ty->name) {
        Type *ty = base.ty;
        SDef *sd = sdef_find(&g->structs, ty->name);
        if (!sd) ERRV(g, "line %zu: unknown type '%s'", n->line, ty->name);
        LLVMTypeRef sty = ty_of(g, ty);
        if (!sty) ERRV(g, "line %zu: type '%s' has no codegen type", n->line,
                       ty->name);

        /* Normalize to an address (a slot for an aggregate value, a no-op
         * for a reference). */
        struct_addr(g, &base);
        if (!base.v)
            ERRV(g, "line %zu: cannot update a value of type '%s'", n->line,
                 ty->name);

        LLVMValueRef cur = LLVMGetUndef(sty);
        for (int j = 0; j < sd->count; j++) {
            int w = -1;
            for (int k = 0; k < n->as.supd.count; k++)
                if (strcmp(n->as.supd.keys[k], sd->names[j]) == 0) { w = k; break; }

            Val fv;
            if (w >= 0) {
                Type *prev = g->expect;
                g->expect = sd->types[j];
                fv = cg_expr(g, n->as.supd.vals[w]);
                g->expect = prev;
                if (!fv.v) return fv;
            } else {
                LLVMTypeRef fty = ty_of(g, sd->types[j]);
                if (!fty)
                    ERRV(g, "line %zu: field '%s' has no codegen type",
                         n->line, sd->names[j]);
                LLVMValueRef fp = gep_field(g, ty, base.v, j);
                if (!fp)
                    ERRV(g, "line %zu: cannot read field '%s'", n->line,
                         sd->names[j]);
                fv = val_make(sd->types[j],
                              LLVMBuildLoad2(g->ab, fty, fp, "f"));
            }

            if (fv.ty && sd->types[j] && fv.ty != sd->types[j])
                fv = coerce(g, sd->types[j], fv, n->line);
            if (g->err[0]) return (Val){ NULL, NULL };
            cur = LLVMBuildInsertValue(g->ab, cur, fv.v, (unsigned)j, "s");
        }
        return val_make(ty, cur);
    }

    /* Anonymous map: clone the base at runtime, then overwrite the listed
     * keys (nested containers stay shared, like the interpreter). */
    LLVMTypeRef newp[1] = { g->i8ptr };
    LLVMValueRef nv[1] = { base.v };
    LLVMValueRef m = LLVMBuildCall2(g->ab,
                                    LLVMFunctionType(g->i8ptr, newp, 1, 0),
                                    rt_decl(g, "lume_map_clone", g->i8ptr,
                                            newp, 1),
                                    nv, 1, "mc");
    if (!m) ERRV(g, "line %zu: cannot copy a map for 'with' update", n->line);

    for (int i = 0; i < n->as.supd.count; i++) {
        Val v = cg_expr(g, n->as.supd.vals[i]);
        if (!v.v) return v;
        Val k = val_make(type_prim(TY_STRING),
                         cg_string_val(g, n->as.supd.keys[i],
                                       strlen(n->as.supd.keys[i])));
        const char *fn = map_put_fn(v.ty);
        if (!fn) ERRV(g, "line %zu: map values must be int, float, string or a container",
                      n->line);
        if (v.ty && v.ty->kind == TY_BOOL) v = coerce(g, type_prim(TY_INT), v, n->line);
        LLVMTypeRef ats[3] = { g->i8ptr, g->i8ptr, ty_of(g, v.ty) };
        LLVMValueRef vs[3] = { m, k.v, v.v };
        Val r = rt_call(g, type_prim(TY_INT), fn, 3, ats, vs);
        if (!r.v) ERRV(g, "line %zu: cannot store into a map", n->line);
    }
    return val_make(type_anon_struct(), m);
}

static Val cg_assign_expr(CG *g, Node *n)
{
    AT(g);
    Val v = cg_expr(g, n->as.assign.value);

    Asg *a = asg_find(&g->locals, n->as.assign.name, n->line);
    if (!a) ERRV(g, "line %zu: assignment to unknown variable '%s'",
                 n->line, n->as.assign.name);

    v = coerce(g, a->ty, v, n->line);
    if (g->err[0]) return (Val){ NULL, NULL };
    if (!ty_of(g, a->ty))
        ERRV(g, "line %zu: variable '%s' has no codegen type",
             n->line, n->as.assign.name);

    LLVMBuildStore(g->ab, v.v, a->slot);
    return v;
}

/* A map/struct literal is a named struct exactly when its *context* names one
 * — a let annotation, a return type, a parameter, a struct field (g->expect).
 * That is the same rule the type checker applies: typecheck_expr.c's N_MAP_LIT
 * reads `expected` and never scans declared shapes, and infer_node_type
 * (llvm_cg_scan.inc) answers "anonymous" for every un-annotated literal. The
 * emitters must give the same answer to the same question — an older version
 * of this function also promoted on a field-name scan, which disagreed with
 * infer_node_type inside one backend: `let rec = { u: 8, v: 9 }` under a
 * declared `type Rec = { u: int, v: int }` stored a %Rec aggregate into the
 * i8* slot the inference had allocated, and the next `rec.u` handed the field
 * value (8) to lume_map_get_i as a pointer and crashed. Kept in step with the
 * text emitter's copy in codegen_lit.c. Tuple literals `(a, b)` are the
 * anonymous-by-default case in its purest form (SPEC §4.13). */
static Type *resolve_struct_lit(CG *g, Node *n)
{
    (void)n;
    Type *e = g->expect;
    if (e && e->kind == TY_STRUCT && e->name && sdef_find(&g->structs, e->name))
        return e;
    return NULL;
}

static Val cg_struct_lit(CG *g, Node *n)
{
    AT(g);
    Type *ty = resolve_struct_lit(g, n);
    SDef *sd = sdef_find(&g->structs, ty->name);
    if (!sd) ERRV(g, "line %zu: unknown type '%s'", n->line, ty->name);
    if (sd->count != n->as.map.count)
        ERRV(g, "line %zu: struct literal for '%s' has %d field(s), declared %d",
             n->line, ty->name, n->as.map.count, sd->count);

    LLVMTypeRef sty = ty_of(g, ty);
    if (!sty) ERRV(g, "line %zu: type '%s' has no codegen type", n->line, ty->name);

    /* Fill the declared field order, not the source order, so a literal
     * written out of order still lands in the right slot. */
    LLVMValueRef cur = LLVMGetUndef(sty);
    for (int j = 0; j < sd->count; j++) {
        int idx = -1;
        for (int i = 0; i < n->as.map.count; i++)
            if (strcmp(n->as.map.keys[i], sd->names[j]) == 0) { idx = i; break; }
        if (idx < 0) ERRV(g, "line %zu: '%s' is missing field '%s'",
                          n->line, ty->name, sd->names[j]);

        Type *prev = g->expect;
        g->expect = sd->types[j];                 /* nested literals resolve here */
        Val f = cg_expr(g, n->as.map.vals[idx]);
        g->expect = prev;

        if (f.ty && sd->types[j] && f.ty != sd->types[j])
            f = coerce(g, sd->types[j], f, n->line);
        if (g->err[0]) return (Val){ NULL, NULL };

        /* A named-struct field may arrive as an address (a variable or member
         * read) rather than an aggregate value (a nested literal). InsertValue
         * needs the aggregate, so load it first. The text emitter keys this
         * off Val.agg; here an aggregate and an address share agg == 0, so the
         * only reliable signal is the value's LLVM type (a struct vs a ptr).
         * The serializer recursion already knows how to read a nested named
         * struct, so this is all the constructor needs. */
        LLVMValueRef fval = f.v;
        if (f.ty && f.ty->kind == TY_STRUCT && f.ty->name && struct_is_addr(f.v))
            fval = LLVMBuildLoad2(g->ab, ty_of(g, f.ty), f.v, "ld");
        cur = LLVMBuildInsertValue(g->ab, cur, fval, (unsigned)j, "s");
    }
    return val_make(ty, cur);
}

/* print() lowers to a non-variadic runtime helper, never to printf: the
 * helpers take i64 for int/bool and double for float, so a boolean has to be
 * widened before the call. */
static Val cg_print(CG *g, Node *n, Val a)
{
    AT(g);
    LLVMTypeRef ret = g->i64;

    switch (a.ty ? a.ty->kind : TY_ANY) {
    case TY_INT: {
        LLVMTypeRef p[1] = { g->i64 };
        LLVMValueRef v[1] = { a.v };
        return val_make(type_prim(TY_INT),
                        LLVMBuildCall2(g->ab, LLVMFunctionType(ret, p, 1, 0),
                                       rt_decl(g, "lume_print_i64", ret, p, 1), v, 1, "p"));
    }
    case TY_FLOAT: {
        LLVMTypeRef p[1] = { g->dbl };
        LLVMValueRef v[1] = { a.v };
        return val_make(type_prim(TY_INT),
                        LLVMBuildCall2(g->ab, LLVMFunctionType(ret, p, 1, 0),
                                       rt_decl(g, "lume_print_double", ret, p, 1), v, 1, "p"));
    }
    case TY_BOOL: {
        LLVMTypeRef p[1] = { g->i64 };
        LLVMValueRef w = LLVMBuildZExt(g->ab, a.v, g->i64, "p");
        LLVMValueRef v[1] = { w };
        return val_make(type_prim(TY_INT),
                        LLVMBuildCall2(g->ab, LLVMFunctionType(ret, p, 1, 0),
                                       rt_decl(g, "lume_print_bool", ret, p, 1), v, 1, "p"));
    }
    case TY_STRING: {
        LLVMTypeRef p[1] = { g->i8ptr };
        LLVMValueRef v[1] = { a.v };
        return val_make(type_prim(TY_INT),
                        LLVMBuildCall2(g->ab, LLVMFunctionType(ret, p, 1, 0),
                                       rt_decl(g, "lume_print_str", ret, p, 1), v, 1, "p"));
    }
    case TY_LIST: {
        LLVMTypeRef p[1] = { g->i8ptr };
        LLVMValueRef v[1] = { a.v };
        return val_make(type_prim(TY_INT),
                        LLVMBuildCall2(g->ab, LLVMFunctionType(ret, p, 1, 0),
                                       rt_decl(g, "lume_list_print", ret, p, 1), v, 1, "p"));
    }
    case TY_STRUCT:
        if (a.ty->name) {
            /* A named struct is a bare GEP value; build a map from its fields
             * (emit_struct_to_map) and print it the same way an anonymous map
             * prints — matching the interpreter's JSON output. */
            struct_addr(g, &a);
            Val m = emit_struct_to_map(g, a.v, a.ty);
            if (!m.v) return m;
            LLVMTypeRef p[1] = { g->i8ptr };
            LLVMValueRef v[1] = { m.v };
            return val_make(type_prim(TY_INT),
                            LLVMBuildCall2(g->ab, LLVMFunctionType(ret, p, 1, 0),
                                           rt_decl(g, "lume_map_print", ret, p, 1),
                                           v, 1, "p"));
        }
        {   /* an anonymous struct is a runtime map */
            LLVMTypeRef p[1] = { g->i8ptr };
            LLVMValueRef v[1] = { a.v };
            return val_make(type_prim(TY_INT),
                            LLVMBuildCall2(g->ab, LLVMFunctionType(ret, p, 1, 0),
                                           rt_decl(g, "lume_map_print", ret, p, 1),
                                           v, 1, "p"));
        }
    case TY_RESULT:
        /* A Result *is* a map ({ ok: ... } / { err: ... }), so it prints
         * through the same runtime helper. Kept in step with cg_print() in
         * codegen_expr.c, which the text emitter uses. */
        {
            LLVMTypeRef p[1] = { g->i8ptr };
            LLVMValueRef v[1] = { a.v };
            return val_make(type_prim(TY_INT),
                            LLVMBuildCall2(g->ab, LLVMFunctionType(ret, p, 1, 0),
                                           rt_decl(g, "lume_map_print", ret, p, 1),
                                           v, 1, "p"));
        }
    case TY_ENUM:
        /* An enum value is the same heap map with a "$" tag. */
        {
            LLVMTypeRef p[1] = { g->i8ptr };
            LLVMValueRef v[1] = { a.v };
            return val_make(type_prim(TY_INT),
                            LLVMBuildCall2(g->ab, LLVMFunctionType(ret, p, 1, 0),
                                           rt_decl(g, "lume_map_print", ret, p, 1),
                                           v, 1, "p"));
        }
    case TY_NULL:
        /* `null` prints the bare word "null" and takes no argument, so it
         * cannot share the typed-argument call shape the switch otherwise
         * builds. */
        return val_make(type_prim(TY_INT),
                        LLVMBuildCall2(g->ab, LLVMFunctionType(ret, NULL, 0, 0),
                                       rt_decl(g, "lume_print_null", ret, NULL, 0),
                                       NULL, 0, "p"));
    case TY_ANY: {
        /* A for-in loop variable over list<any> (e.g. [null, 2, null]) is a
         * boxed LumeSlot*; print() dispatches on its tag through
         * lume_print_slot (src/rt.c), matching the interpreter byte-for-byte. */
        LLVMTypeRef p[1] = { g->i8ptr };
        LLVMValueRef v[1] = { a.v };
        return val_make(type_prim(TY_INT),
                        LLVMBuildCall2(g->ab, LLVMFunctionType(ret, p, 1, 0),
                                       rt_decl(g, "lume_print_slot", ret, p, 1),
                                       v, 1, "p"));
    }
    default:
        ERRV(g, "line %zu: print() cannot print this value", n->line);
    }
}

/* Build a LumeMap from the fields of a named-struct value `sv` (an address
 * of the struct). See the long note on emit_struct_to_map() in
 * codegen_expr.c — same approach, LLVM C API spelling. The type `st` already
 * carries the field names and types, so no runtime reflection is needed:
 * each field is read by gep_field+load and stored under its name with the
 * same put helpers a map literal uses. A nested named struct is converted
 * recursively and stored as an obj. This is what lets print()/str()/
 * stringify() accept a named struct natively. */
static Val emit_struct_to_map(CG *g, LLVMValueRef sv, Type *st)
{
    AT(g);
    LLVMTypeRef newp[1] = { g->i8ptr };
    LLVMValueRef nv[1] = { NULL };
    LLVMValueRef m = LLVMBuildCall2(g->ab, LLVMFunctionType(g->i8ptr, NULL, 0, 0),
                                    rt_decl(g, "lume_map_new", g->i8ptr, newp, 0),
                                    nv, 0, "m");
    if (!m) ERRV(g, "cannot serialize struct '%s'", st->name);

    /* A struct type handed in may be a bare name reference (count == 0) — that
     * is what a member access returns, because struct_field_type() gives the
     * codegen struct table's stored field type, kept as a reference. Resolve
     * the declared field list through the SDef by name. */
    SDef *sd = st->name ? sdef_find(&g->structs, st->name) : NULL;

    for (int i = 0; i < (sd ? sd->count : st->count); i++) {
        Type *fty   = sd ? sd->types[i] : st->types[i];
        const char *fname = sd ? sd->names[i] : st->names[i];

        LLVMValueRef key = cg_string_val(g, fname, strlen(fname));
        if (!key) return (Val){ NULL, NULL };

        LLVMValueRef p = gep_field(g, st, sv, i);
        if (!p) return (Val){ NULL, NULL };

        if (fty->kind == TY_STRUCT && fty->name) {
            Val inner = emit_struct_to_map(g, p, fty);
            if (!inner.v) return inner;
            LLVMTypeRef ats[3] = { g->i8ptr, g->i8ptr, g->i8ptr };
            LLVMValueRef vs[3] = { m, key, inner.v };
            Val r = rt_call(g, type_prim(TY_INT), "lume_map_put_obj", 3, ats, vs);
            if (!r.v) return r;
            continue;
        }

        LLVMValueRef fv = LLVMBuildLoad2(g->ab, ty_of(g, fty), p, "f");
        Val vv = val_make(fty, fv);
        if (fty->kind == TY_BOOL) vv = coerce(g, type_prim(TY_INT), vv, 0);

        const char *fn = map_put_fn(fty);
        if (!fn) ERRV(g, "struct field '%s' has no serializable type", fname);

        LLVMTypeRef ats[3] = { g->i8ptr, g->i8ptr, ty_of(g, vv.ty) };
        LLVMValueRef vs[3] = { m, key, vv.v };
        Val r = rt_call(g, type_prim(TY_INT), fn, 3, ats, vs);
        if (!r.v) return r;
    }
    return val_make(type_anon_struct(), m);
}

static Val cg_funclit(CG *g, Node *n)
{
    AT(g);
    const char *cname = n->as.funclit.cname;
    if (!cname)
        ERRV(g, "line %zu: internal: lambda was never named by the signature "
                "pass", n->line);

    int arity = n->as.funclit.arity;
    LLVMTypeRef *pts = (LLVMTypeRef *)xmalloc(sizeof(LLVMTypeRef) * (size_t)(arity + 1));
    pts[0] = g->i8ptr;                       /* the capture context */
    for (int i = 0; i < arity; i++) {
        pts[i + 1] = n->as.funclit.param_types
            ? ptr_of(g, n->as.funclit.param_types[i]) : NULL;
        if (!pts[i + 1]) { free(pts); ERRV(g, "line %zu: this lambda's parameter "
                                           "has no codegen type", n->line); }
    }
    LLVMTypeRef rty = n->as.funclit.ret ? ty_of(g, n->as.funclit.ret) : g->void_ty;
    LLVMTypeRef fty = LLVMFunctionType(rty, pts, (unsigned)(arity + 1), 0);
    free(pts);
    if (!fty) ERRV(g, "line %zu: cannot spell this lambda's type", n->line);
    if (!LLVMGetNamedFunction(g->mod, cname) &&
        !LLVMAddFunction(g->mod, cname, fty))
        ERRV(g, "line %zu: cannot declare the closure", n->line);
    LLVMValueRef fnv = LLVMGetNamedFunction(g->mod, cname);
    if (!fnv) ERRV(g, "line %zu: internal: closure symbol vanished", n->line);

    /* Phase-2 captures (mirrors codegen_call.c's cg_funclit): build the
     * capture array — one i8* per captured name, holding the address of
     * the outer variable's slot — and hand it to lume_closure_make as the
     * record's second word. The closure's own define reads those addresses
     * back through its context parameter (cg_def), so a later write to the
     * outer variable stays visible inside (shared env, SPEC §4.7). The
     * array lives in the *creating* frame, so the closure must not
     * outlive it — the same contract the text backend has. */
    LLVMValueRef caparg = LLVMConstNull(g->i8ptr);
    int ncap = n->as.funclit.cap_count;
    if (ncap > 0) {
        LLVMValueRef *addrs = (LLVMValueRef *)xmalloc(sizeof(LLVMValueRef)
                                                      * (size_t)ncap);
        for (int i = 0; i < ncap; i++) {
            const char *nm = n->as.funclit.cap_names[i];
            Asg *a = asg_find(&g->locals, nm, n->line);
            if (!a) { free(addrs); ERRV(g, "line %zu: captured variable '%s' "
                                        "has no codegen slot", n->line, nm); }
            if (a->cap) { free(addrs); ERRV(g, "line %zu: capturing a variable "
                                            "already captured by the enclosing "
                                            "closure is not supported", n->line); }
            LLVMTypeRef vt = ty_of(g, a->ty);
            if (!vt || !a->slot) {
                free(addrs);
                ERRV(g, "line %zu: captured variable '%s' has no codegen type",
                     n->line, nm);
            }
            /* Every slot is a pointer (alloca, module global, or a struct
             * parameter's incoming pointer), so bitcast the *pointer value*
             * to i8* — that address is what the closure reads back. */
            LLVMValueRef addr = LLVMBuildBitCast(g->ab, a->slot, g->i8ptr,
                                                 "capaddr");
            if (!addr) { free(addrs); ERRV(g, "line %zu: cannot capture '%s'",
                                           n->line, nm); }
            addrs[i] = addr;
        }
        LLVMValueRef arr = LLVMBuildArrayAlloca(g->ab, g->i8ptr,
                                                LLVMConstInt(g->i64, (uint64_t)ncap, 0),
                                                "caparr");
        if (!arr) { free(addrs); ERRV(g, "line %zu: cannot allocate the "
                                       "capture array", n->line); }
        for (int i = 0; i < ncap; i++) {
            LLVMValueRef idxv = LLVMConstInt(g->i64, (uint64_t)i, 0);
            LLVMValueRef el = LLVMBuildGEP2(g->ab, g->i8ptr, arr, &idxv, 1,
                                            "capel");
            if (!el || LLVMBuildStore(g->ab, addrs[i], el) == NULL) {
                free(addrs);
                ERRV(g, "line %zu: cannot store capture '%s'", n->line,
                     n->as.funclit.cap_names[i]);
            }
        }
        free(addrs);
        caparg = LLVMBuildBitCast(g->ab, arr, g->i8ptr, "capctx");
        if (!caparg) ERRV(g, "line %zu: cannot build the capture context",
                          n->line);
    }

    LLVMTypeRef p2[2] = { g->i8ptr, g->i8ptr };
    LLVMValueRef vs[2] = { fnv, caparg };
    LLVMValueRef r = LLVMBuildCall2(g->ab, LLVMFunctionType(g->i8ptr, p2, 2, 0),
                                    rt_decl(g, "lume_closure_make", g->i8ptr, p2, 2),
                                    vs, 2, "clo");
    if (!r) ERRV(g, "line %zu: cannot build this closure", n->line);

    /* The same TY_FUNC-with-back-pointer infer_node_type builds, so a call
     * through the boxed value finds the lambda and its pinned signature. */
    Type *ft = type_func(arity, n->as.funclit.param_types, n->as.funclit.ret);
    ft->func = n;
    return val_make(ft, r);
}

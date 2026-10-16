/* llvm_cg_expr.inc - LLVM backend expr functions.
 * Textually included by llvm_codegen.c (single translation unit).
 * Do not compile this file on its own. */

/* The vendored frontend exports no node-name helper; keep a local table so
 * "not supported yet" errors point at the actual construct. */
static const char *node_type_name(NodeType t)
{
    switch (t) {
    case N_PROGRAM: return "program";           case N_BLOCK:  return "block";
    case N_LET:     return "let";               case N_IF:     return "if";
    case N_IF_LET:  return "if-let";
    case N_LET_PAT: return "let-pattern";
    case N_LET_EXPR: return "let-expression";
    case N_WHILE:   return "while";             case N_FOR:    return "for";
    case N_BREAK:   return "break";             case N_CONTINUE: return "continue";
    case N_RETURN:  return "return";            case N_EXPR_STMT: return "expression statement";
    case N_SERVER:  return "server";            case N_ROUTE:  return "route";
    case N_TOOL:    return "tool";              case N_VERBS:  return "verbs";
    case N_FUNC_DECL: return "func";            case N_TYPE_DECL: return "type";
    case N_VAR:     return "variable";          case N_ASSIGN: return "assign";
    case N_ASSIGN_MEMBER: return "member assign";              case N_LITERAL: return "literal";
    case N_MAP_LIT: return "struct literal";    case N_LIST_LIT: return "list literal";
    case N_FUNC_LIT: return "closure";          case N_CALL:   return "call";
    case N_MEMBER:  return "field access";      case N_UNARY:  return "unary";
    case N_BINARY:  return "binary";            case N_TERNARY: return "ternary";
    case N_IMPORT: return "import";
    case N_INDEX:   return "index";
    case N_ENUM_DECL: return "enum";
    case N_MATCH:   return "match";             case N_CTOR: return "constructor";
    case N_STRUCT_UPDATE: return "struct update";
    }
    return "node";
}

static const char *bad(CG *g, Node *n, const char *what)
{
    snprintf(g->err, sizeof g->err,
             "line %zu: %s (%s) is not supported by the native backend yet",
             n ? n->line : 0, node_type_name(n->type), what);
    return g->err;
}

static Val cg_literal(CG *g, Node *n)
{
    AT(g);
    LitKind k = n->as.lit.kind;

    if (k == LIT_TRUE || k == LIT_FALSE)
        return val_make(type_prim(TY_BOOL),
                        LLVMConstInt(g->i1, k == LIT_TRUE ? 1u : 0u, 0));

    if (k == LIT_STR) {
        /* The lexer hands over the token *with* its quotes. */
        const char *t = n->as.lit.text;
        size_t      len = (size_t)n->as.lit.len;
        if (len >= 2 && t[0] == '"' && t[len - 1] == '"') { t++; len -= 2; }
        /* Language-level escapes resolved through the shared table:
         * LLVMConstStringInContext takes the bytes verbatim, so without this
         * pass `"a\nb"` emitted four characters while the interpreter printed
         * a newline (mirrors codegen_expr.c's cg_literal). */
        size_t ulen = 0;
        char *u = lume_unescape(t, len, &ulen);
        LLVMValueRef s = cg_string_val(g, u, ulen);
        free(u);
        return val_make(type_prim(TY_STRING), s);
    }

    if (k == LIT_NUM) {
        if (n->as.lit.is_float)
            return val_make(type_prim(TY_FLOAT), LLVMConstReal(g->dbl, n->as.lit.num));
        /* Read the literal's i64 directly; going through lit.num (a double)
         * would round anything past 2^53 before LLVM ever sees it. */
        return val_make(type_prim(TY_INT),
                        LLVMConstInt(g->i64, (unsigned long long)n->as.lit.inum, 0));
    }

    if (k == LIT_NULL)
        /* `null` lowers to an opaque i8* null pointer (LLVMConstNull); the
         * runtime helper prints it as "null", matching the interpreter. */
        return val_make(type_prim(TY_NULL), LLVMConstNull(g->i8ptr));

    ERRV(g, "line %zu: 'null' literals are not supported by the native backend yet", n->line);
}

static Val cg_var(CG *g, Node *n)
{
    Asg *a = asg_find(&g->locals, n->as.var.name, n->line);
    /* Not a local: a top-level binding, read through its module global (the
     * slot value is the global itself — a valid pointer operand for the load,
     * same as an alloca). Mirrors codegen_expr.c's fallback. */
    if (!a) a = gvar_find(&g->gvars, n->as.var.name);
    if (!a) ERRV(g, "line %zu: unknown variable '%s'", n->line, n->as.var.name);
    if (!ty_of(g, a->ty))
        ERRV(g, "line %zu: variable '%s' has no codegen type", n->line, n->as.var.name);

    /* A *named* struct is always *referenced*: the slot holds its address.
     * A list or a runtime map is a heap object behind an opaque pointer, so
     * the slot holds the pointer itself and has to be loaded -- passing the
     * slot would make the runtime read its struct out of the stack frame. */
    if (a->ty->kind == TY_STRUCT && a->ty->name) return val_make(a->ty, a->slot);

    AT(g);
    return val_make(a->ty, LLVMBuildLoad2(g->ab, ty_of(g, a->ty), a->slot, "v"));
}

static Val cg_unary(CG *g, Node *n)
{
    AT(g);
    Val o = cg_expr(g, n->as.unary.operand);
    if (!o.v) ERRV(g, "line %zu: bad unary operand", n->line);

    if (n->as.unary.op == OP_NOT)
        return val_make(type_prim(TY_BOOL), LLVMBuildNot(g->ab, o.v, "n"));

    if (!o.ty || (o.ty->kind != TY_INT && o.ty->kind != TY_FLOAT))
        ERRV(g, "line %zu: unary '-' needs a numeric operand", n->line);

    if (o.ty->kind == TY_FLOAT) return val_make(o.ty, LLVMBuildFNeg(g->ab, o.v, "n"));
    return val_make(o.ty, LLVMBuildNeg(g->ab, o.v, "n"));
}

/* == / != ask what two values *contain*, never whether two pointers happen to
 * match — the libLLVM mirror of codegen_expr.c's cg_val_eq, kept in step with
 * it branch for branch (the long comment there carries the semantics). Both
 * operands are consumed; NULL is an error, with the reason in g->err.
 *
 * A named struct is the one shape that never reaches rt.c: it lowers to a
 * typed aggregate, not the i8* heap map the runtime walks, so a struct
 * against a struct goes to cg_struct_eq for a field-by-field compare. */
static LLVMValueRef cg_val_eq(CG *g, Val a, Val b, bool ne, size_t line)
{
    AT(g);
    TypeKind ak = a.ty ? a.ty->kind : TY_ANY;
    TypeKind bk = b.ty ? b.ty->kind : TY_ANY;
    bool a_ctr = ak == TY_LIST || ak == TY_STRUCT ||
                 ak == TY_ENUM || ak == TY_RESULT;
    bool b_ctr = bk == TY_LIST || bk == TY_STRUCT ||
                 bk == TY_ENUM || bk == TY_RESULT;
    bool a_ptr = a_ctr || ak == TY_STRING || ak == TY_FUNC ||
                 ak == TY_NULL || ak == TY_ANY;
    bool b_ptr = b_ctr || bk == TY_STRING || bk == TY_FUNC ||
                 bk == TY_NULL || bk == TY_ANY;
    bool a_named = ak == TY_STRUCT && a.ty->name &&
                   sdef_find(&g->structs, a.ty->name) != NULL;
    bool b_named = bk == TY_STRUCT && b.ty->name &&
                   sdef_find(&g->structs, b.ty->name) != NULL;

    if (a_named && b_named)
        return cg_struct_eq(g, a, b, ne, line);

    if (a_named != b_named)
        /* A typed aggregate against an i8* heap map: no runtime bridge
         * between the two representations exists, and the interpreter would
         * answer by comparing contents. */
        ERR(g, "line %zu: '%s' between %s and %s is not supported "
                "by the native backend yet",
             line, ne ? "!=" : "==", src_type_name(a.ty), src_type_name(b.ty));

    const char *rt_fn = NULL;
    if (ak == TY_STRING && bk == TY_STRING)
        rt_fn = "lume_str_eq";
    else if (a_ctr && b_ctr)
        rt_fn = "lume_value_eq";   /* structural, SPEC 1.8 */
    else if ((ak == TY_ANY && b_ptr) || (bk == TY_ANY && a_ptr))
        rt_fn = "lume_any_eq";     /* runtime probes the kind */
    if (rt_fn) {
        LLVMTypeRef at2[2] = { g->i8ptr, g->i8ptr };
        LLVMValueRef eq64 = LLVMBuildCall2(g->ab,
                LLVMFunctionType(g->i64, at2, 2, 0),
                rt_decl(g, rt_fn, g->i64, at2, 2),
                (LLVMValueRef[]){ a.v, b.v }, 2, "eq");
        LLVMValueRef b1 = LLVMBuildICmp(g->ab, LLVMIntNE, eq64,
                                        LLVMConstInt(g->i64, 0, 0), "isb");
        if (ne)
            b1 = LLVMBuildXor(g->ab, b1, LLVMConstInt(g->i1, 1, 0), "not");
        return b1;
    }
    /* bool == bool, null against another i8*-lowered value, and func identity
     * all compare as their own IR types. */
    bool self_typed =
        (ak == TY_BOOL && bk == TY_BOOL) ||
        ((ak == TY_NULL || bk == TY_NULL) && a_ptr && b_ptr) ||
        (ak == TY_FUNC && bk == TY_FUNC);
    if (self_typed || ((ak == TY_INT || ak == TY_FLOAT) &&
                       (bk == TY_INT || bk == TY_FLOAT))) {
        /* Numeric: cg_binary's promotion already widened a mixed int/float
         * pair to double, so a float here means both sides are float. */
        unsigned pred[2] = { 0, 0 };
        if (ak == TY_FLOAT) {
            pred[0] = LLVMRealOEQ; pred[1] = LLVMRealONE;
            return LLVMBuildFCmp(g->ab, (LLVMRealPredicate)pred[ne ? 1 : 0],
                                 a.v, b.v, "c");
        }
        pred[0] = LLVMIntEQ; pred[1] = LLVMIntNE;
        return LLVMBuildICmp(g->ab, (LLVMIntPredicate)pred[ne ? 1 : 0],
                             a.v, b.v, "c");
    }
    ERR(g, "line %zu: '%s' between %s and %s is not supported "
            "by the native backend yet",
         line, ne ? "!=" : "==", src_type_name(a.ty), src_type_name(b.ty));
}

/* Two named structs compare field by field (SPEC 1.8). The interpreter
 * answers the same question by comparing the key sets of the two maps, and
 * field names are the keys — so a field-set mismatch is a constant answer,
 * not an error, and the two sides need not even be the same declared type:
 * what matters is that the fields line up by name.
 *
 * Both sides get a slot (struct_addr), each field is loaded through a
 * StructGEP and compared recursively — a field that is itself a list, a map
 * or a nested struct recurses back into cg_val_eq. The result is the AND
 * over the fields, xored for `!=`; two field-less structs are equal. */
static LLVMValueRef cg_struct_eq(CG *g, Val a, Val b, bool ne, size_t line)
{
    AT(g);
    SDef *sa = sdef_find(&g->structs, a.ty->name);
    SDef *sb = sdef_find(&g->structs, b.ty->name);

    /* bj[i] is sb's index for sa's field i, or -1 when the name is absent. */
    int *bj = NULL;
    if (sa && sb && sa->count == sb->count) {
        bj = (int *)xmalloc((size_t)sa->count * sizeof *bj);
        for (int i = 0; i < sa->count; i++) {
            bj[i] = -1;
            for (int j = 0; j < sb->count; j++)
                if (strcmp(sb->names[j], sa->names[i]) == 0) { bj[i] = j; break; }
        }
        for (int i = 0; i < sa->count; i++)
            if (bj[i] < 0) { free(bj); bj = NULL; break; }
    }
    if (!bj)
        return ne ? LLVMConstInt(g->i1, 1, 0) : LLVMConstInt(g->i1, 0, 0);

    struct_addr(g, &a);
    struct_addr(g, &b);
    if (!a.v || !b.v) {
        free(bj);
        ERR(g, "line %zu: cannot compare these struct values", line);
    }

    LLVMValueRef acc = LLVMConstInt(g->i1, 1, 0);
    for (int i = 0; i < sa->count; i++) {
        int j = bj[i];
        Type *fty = sa->types[i];
        Type *gty = sb->types[j];
        if (!ty_of(g, fty) || !ty_of(g, gty)) {
            free(bj);
            ERR(g, "line %zu: field '%s' has no codegen type", line, sa->names[i]);
        }
        LLVMValueRef pa = gep_field(g, a.ty, a.v, i);
        LLVMValueRef pb = gep_field(g, b.ty, b.v, j);
        if (!pa || !pb) {
            free(bj);
            ERR(g, "line %zu: cannot compare field '%s'", line, sa->names[i]);
        }
        Val fa = val_make(fty, LLVMBuildLoad2(g->ab, ty_of(g, fty), pa, "f"));
        Val fb = val_make(gty, LLVMBuildLoad2(g->ab, ty_of(g, gty), pb, "f"));
        if (!fa.v || !fb.v) {
            free(bj);
            ERR(g, "line %zu: cannot read field '%s'", line, sa->names[i]);
        }
        LLVMValueRef fe = cg_val_eq(g, fa, fb, false, line);
        if (!fe) { free(bj); return NULL; }
        acc = LLVMBuildAnd(g->ab, acc, fe, "and");
        if (!acc) {
            free(bj);
            ERR(g, "line %zu: cannot compare these struct values", line);
        }
    }
    free(bj);
    if (ne)
        acc = LLVMBuildXor(g->ab, acc, LLVMConstInt(g->i1, 1, 0), "not");
    return acc;
}

static Val cg_binary(CG *g, Node *n)
{
    AT(g);
    /* == / != against a named struct: when the *other* operand is statically a
     * named struct, pin it as the expect type while both operands evaluate, so
     * `p == { x: 1, y: 2 }` rebuilds the literal as a %P aggregate and
     * cg_val_eq sees named-vs-named — the content comparison the interpreter
     * answers. Only a literal consults expect; a variable or member read
     * ignores it. Mirrors codegen_expr.c's cg_binary. */
    Type *named_side = NULL;
    if (n->as.binary.op == OP_EQ || n->as.binary.op == OP_NE) {
        Type *lt = infer_node_type(g, n->as.binary.left);
        Type *rt = infer_node_type(g, n->as.binary.right);
        if (lt && lt->kind == TY_STRUCT && lt->name &&
            sdef_find(&g->structs, lt->name))
            named_side = lt;
        else if (rt && rt->kind == TY_STRUCT && rt->name &&
                 sdef_find(&g->structs, rt->name))
            named_side = rt;
    }
    Type *saved_exp = g->expect;
    if (named_side) g->expect = named_side;
    Val a = cg_expr(g, n->as.binary.left);
    Val b = cg_expr(g, n->as.binary.right);
    if (named_side) g->expect = saved_exp;

    /* Promote before emitting anything: `x / 2` with x: double would otherwise
     * hand LLVM an i64 constant where a double operand is required. */
    if (a.ty && b.ty && (a.ty->kind == TY_FLOAT || b.ty->kind == TY_FLOAT)) {
        a = coerce(g, type_prim(TY_FLOAT), a, n->line);
        b = coerce(g, type_prim(TY_FLOAT), b, n->line);
    }
    if (!a.v || !b.v) ERRV(g, "line %zu: bad operands", n->line);

    if (n->as.binary.op == OP_AND || n->as.binary.op == OP_OR) {
        /* Short-circuit, so the right operand is *not* emitted yet: the subset
         * is used for `x != 0 and 10 / x > 1` and an eager lowering would
         * divide by zero. */
        bool is_and = n->as.binary.op == OP_AND;

        /* The result is parked in an alloca rather than in a phi, which is
         * what the rest of this emitter does for every other local. */
        LLVMValueRef a_slot = LLVMBuildAlloca(g->ab, g->i1, "lb");

        /* Three blocks, each writing the result slot exactly once. AND can
         * only answer `true` from the right operand, OR only from the left, so
         * the short path stores a constant: `true or 1/0` has to read `true`,
         * never the value of an expression it never evaluated. */
        LLVMBasicBlockRef ls = LLVMAppendBasicBlockInContext(g->ctx, g->fn, "short");
        LLVMBasicBlockRef ll = LLVMAppendBasicBlockInContext(g->ctx, g->fn, "long");
        LLVMBasicBlockRef lj = LLVMAppendBasicBlockInContext(g->ctx, g->fn, "join");

        if (!DONE(g->cur)) LLVMBuildCondBr(g->ab, a.v, is_and ? ll : ls, is_and ? ls : ll);

        g->cur = ls; AT(g);
        LLVMBuildStore(g->ab, LLVMConstInt(g->i1, is_and ? 0u : 1u, 0), a_slot);
        if (!DONE(g->cur)) LLVMBuildBr(g->ab, lj);

        g->cur = ll; AT(g);
        Val b2 = cg_expr(g, n->as.binary.right);
        if (!b2.v) ERRV(g, "line %zu: bad logical operands", n->line);
        LLVMBuildStore(g->ab, b2.v, a_slot);
        if (!DONE(g->cur)) LLVMBuildBr(g->ab, lj);

        g->cur = lj; AT(g);
        return val_make(type_prim(TY_BOOL), LLVMBuildLoad2(g->ab, g->i1, a_slot, "l"));
    }

    /* string + string is the only operator strings get, and it has to be a
     * runtime call — IR has no aggregate instruction to concatenate with. */
    if (n->as.binary.op == OP_ADD &&
        a.ty && b.ty && a.ty->kind == TY_STRING && b.ty->kind == TY_STRING) {
        LLVMTypeRef at[2] = { g->i8ptr, g->i8ptr };
        LLVMTypeRef ft = LLVMFunctionType(g->i8ptr, at, 2, 0);
        LLVMValueRef fn = LLVMGetNamedFunction(g->mod, "lume_bi_cat");
        if (!fn) fn = LLVMAddFunction(g->mod, "lume_bi_cat", ft);
        LLVMValueRef args[2] = { a.v, b.v };
        return val_make(type_prim(TY_STRING),
                        LLVMBuildCall2(g->ab, ft, fn, args, 2, "cat"));
    }

    if (n->as.binary.op >= OP_EQ && n->as.binary.op <= OP_GE) {
        size_t k = (size_t)(n->as.binary.op - OP_EQ);
        bool is_eq = n->as.binary.op == OP_EQ || n->as.binary.op == OP_NE;
        static const unsigned iop[] = { LLVMIntEQ, LLVMIntNE, LLVMIntSLT,
                                        LLVMIntSLE, LLVMIntSGT, LLVMIntSGE };
        if (is_eq) {
            /* == / != need content or structure — never pointer luck: the
             * dispatch lives in cg_val_eq (mirrors codegen_expr.c's
             * restructured block; see the comment there for why strings stay
             * a known-broken corner on the relational spellings, which never
             * reach it). */
            LLVMValueRef r = cg_val_eq(g, a, b,
                                       n->as.binary.op == OP_NE, n->line);
            if (!r) ERRV(g, "line %zu: bad comparison", n->line);
            return val_make(type_prim(TY_BOOL), r);
        }
        if (a.ty && a.ty->kind == TY_FLOAT) {
            static const unsigned op[] = { LLVMRealOEQ, LLVMRealONE, LLVMRealOLT,
                                           LLVMRealOLE, LLVMRealOGT, LLVMRealOGE };
            return val_make(type_prim(TY_BOOL),
                            LLVMBuildFCmp(g->ab, (LLVMRealPredicate)op[k], a.v, b.v, "c"));
        }
        return val_make(type_prim(TY_BOOL),
                        LLVMBuildICmp(g->ab, (LLVMIntPredicate)iop[k], a.v, b.v, "c"));
    }

    /* Same guard the IR-text emitter puts up (see cg_binary there): a string
     * against a number is rejected with a line number in the source instead of
     * letting a libLLVM assertion with no file name surface from the builder. */
    bool astr    = a.ty && a.ty->kind == TY_STRING;
    bool bstr    = b.ty && b.ty->kind == TY_STRING;
    bool numeric = (a.ty && (a.ty->kind == TY_INT || a.ty->kind == TY_FLOAT)) ||
                   (b.ty && (b.ty->kind == TY_INT || b.ty->kind == TY_FLOAT));
    if ((astr || bstr) && numeric)
        ERRV(g, "line %zu: mixing a string with a number is not supported by the "
                "native backend yet", n->line);

    bool is_float = a.ty && a.ty->kind == TY_FLOAT;

    /* `/` is floating-point division even on two integers: the interpreter
     * answers `7 / 2` with 3.5, and an sdiv answers 3. Both native backends
     * used to emit the integer division — a wrong answer, not an error — so
     * the operands are widened here instead. */
    bool div_int = !is_float && n->as.binary.op == OP_DIV;
    Type *rty = type_prim((is_float || div_int) ? TY_FLOAT : TY_INT);
    LLVMValueRef r;
    if (is_float) {
        switch (n->as.binary.op) {
        case OP_ADD: r = LLVMBuildFAdd(g->ab, a.v, b.v, "a"); break;
        case OP_SUB: r = LLVMBuildFSub(g->ab, a.v, b.v, "a"); break;
        case OP_MUL: r = LLVMBuildFMul(g->ab, a.v, b.v, "a"); break;
        case OP_DIV: r = LLVMBuildFDiv(g->ab, a.v, b.v, "a"); break;
        case OP_MOD: ERRV(g, "line %zu: '%%' does not apply to floats", n->line);
        default:     ERRV(g, "line %zu: bad operator", n->line);
        }
    } else {
        switch (n->as.binary.op) {
        case OP_ADD: r = LLVMBuildAdd(g->ab, a.v, b.v, "a"); break;
        case OP_SUB: r = LLVMBuildSub(g->ab, a.v, b.v, "a"); break;
        case OP_MUL: r = LLVMBuildMul(g->ab, a.v, b.v, "a"); break;
        case OP_DIV: {
            LLVMValueRef fa = LLVMBuildSIToFP(g->ab, a.v, g->dbl, "a");
            LLVMValueRef fb = LLVMBuildSIToFP(g->ab, b.v, g->dbl, "b");
            if (!fa || !fb) ERRV(g, "line %zu: bad division operands", n->line);
            r = LLVMBuildFDiv(g->ab, fa, fb, "a");
            break;
        }
        case OP_MOD: r = LLVMBuildSRem(g->ab, a.v, b.v, "a"); break;
        default:     ERRV(g, "line %zu: bad operator", n->line);
        }
    }
    return val_make(rty, r);
}

static Val cg_len_of(CG *g, const char *fn, LLVMValueRef obj)
{
    AT(g);
    LLVMTypeRef p[1] = { g->i8ptr };
    LLVMValueRef v[1] = { obj };
    LLVMValueRef r = LLVMBuildCall2(g->ab, LLVMFunctionType(g->i64, p, 1, 0),
                                    rt_decl(g, fn, g->i64, p, 1), v, 1, "len");
    return r ? val_make(type_prim(TY_INT), r) : (Val){ NULL, NULL };
}

static Val cg_member_load(CG *g, Val obj, const char *name, Type *vt,
                          size_t line);

/* `let { a, b } = e;` — SPEC §4.12. One evaluation of the init, then every
 * name stored from a field load of that single value: the emitter's
 * spelling of `let a = e.a; let b = e.b;` with one e. The checker stamped
 * the field types, so each load dispatches exactly like the `m.x` it
 * mirrors. */
static void cg_let_pat(CG *g, Node *n)
{
    AT(g);
    Val base = cg_expr(g, n->as.letpat.init);
    if (!base.v) ERRX(g, "line %zu: bad destructuring value", n->line);
    for (int i = 0; i < n->as.letpat.count && !g->err[0]; i++) {
        Type *ty = n->as.letpat.types[i];
        const char *nm = n->as.letpat.names[i];
        if (!ty)
            ERRX(g, "line %zu: cannot store '%s' (the destructured map has "
                    "no known shape)", n->line, nm);
        Val v = cg_member_load(g, base, n->as.letpat.fields[i], ty, n->line);
        if (!v.v) break;
        v = coerce(g, ty, v, n->line);
        if (g->err[0]) break;   /* coerce refused (e.g. null into a scalar) */
        if (!ty_of(g, ty))
            ERRX(g, "line %zu: cannot store this into a typed local", n->line);
        /* A named-struct field arrives as an address; the slot holds the
         * aggregate, so load it first — the same conversion a struct
         * literal's field makes (see cg_stmt's N_LET). */
        if (ty->kind == TY_STRUCT && ty->name && struct_is_addr(v.v))
            v.v = LLVMBuildLoad2(g->ab, ty_of(g, ty), v.v, "ld");
        Asg *a = asg_find(&g->locals, nm, n->line);
        if (!a) ERRX(g, "line %zu: internal: '%s' was not allocated by the "
                        "scan pass", n->line, nm);
        if (!a->slot) a->slot = LLVMBuildAlloca(g->ab, ty_of(g, ty), "lv");
        LLVMBuildStore(g->ab, v.v, a->slot);
    }
}

static Val cg_member(CG *g, Node *n)
{
    AT(g);
    Val obj = cg_expr(g, n->as.member.obj);

    /* `s.len` / `s.length` / `xs.len` / `m.len`, before the struct path: there
     * is no declared field behind any of them. It has to come before struct_addr
     * too — struct_addr turns a map's *heap pointer* into the address of a
     * stack slot holding that pointer, and passing the slot to lume_map_len
     * would read the map out of the caller's frame. */
    if (strcmp(n->as.member.name, "len") == 0 ||
        strcmp(n->as.member.name, "length") == 0) {
        if (obj.ty && obj.ty->kind == TY_LIST)
            return cg_len_of(g, "lume_list_len", obj.v);
        if (obj.ty && obj.ty->kind == TY_STRUCT && !obj.ty->name)
            return cg_len_of(g, "lume_map_len", obj.v);
        /* A string is the runtime's own i8* in a slot, not an object with a
         * header, so its length lives in the runtime's helper. Skipping this
         * branch used to fall through to print() as an unprintable value. */
        if (obj.ty && obj.ty->kind == TY_STRING)
            return cg_len_of(g, "lume_bi_len_s", obj.v);
    }

    return cg_member_load(g, obj, n->as.member.name, n->as.member.type,
                          n->line);
}

/* Load one field out of an already-evaluated base: the runtime-map path
 * (`m.x` on an anonymous struct — lume_map_get_*, dispatched by the
 * field's value type) or the named-struct path (gep + load). obj.v is an
 * LLVMValueRef, so there is nothing to free and a caller may reuse one
 * base for several fields (a destructuring `let { a, b } = e`). */
static Val cg_member_load(CG *g, Val obj, const char *name, Type *vt,
                          size_t line)
{
    if (obj.ty && obj.ty->kind == TY_STRUCT && !obj.ty->name) {
        const char *gfn = map_get_fn(vt);
        if (!gfn)
            ERRV(g, "line %zu: map field '%s' has no codegen type",
                 line, name);

        LLVMTypeRef ret_ty, dflt_ty;
        LLVMValueRef dflt;
        if (vt->kind == TY_FLOAT) {
            ret_ty = g->dbl;  dflt_ty = g->dbl;
            dflt = LLVMConstReal(g->dbl, 0.0);
        } else if (vt->kind == TY_STRING || vt->kind == TY_LIST ||
                   vt->kind == TY_STRUCT || vt->kind == TY_RESULT ||
                   vt->kind == TY_ENUM  || vt->kind == TY_NULL) {
            ret_ty = g->i8ptr; dflt_ty = g->i8ptr;
            dflt = LLVMConstNull(g->i8ptr);
        } else {
            ret_ty = g->i64;  dflt_ty = g->i64;
            dflt = LLVMConstInt(g->i64, 0, 0);   /* int / bool */
        }

        LLVMValueRef key = cg_string_val(g, name, (int)strlen(name));
        LLVMTypeRef params[3] = { g->i8ptr, g->i8ptr, dflt_ty };
        LLVMValueRef args[3] = { obj.v, key, dflt };
        LLVMValueRef call = LLVMBuildCall2(g->ab,
                LLVMFunctionType(ret_ty, params, 3, 0),
                rt_decl(g, gfn, ret_ty, params, 3),
                args, 3, "mf");
        /* A bool comes back from lume_map_get_i as an i64 (0/1); narrow it to
         * the backend's i1 bool representation. */
        if (vt->kind == TY_BOOL)
            call = LLVMBuildTrunc(g->ab, call, g->i1, "mf");
        return val_make(vt, call);
    }

    struct_addr(g, &obj);

    /* Look the field up through the *object's* type, not the member node's
     * own type: the member node carries the field's *result* type (so
     * `o1.inner` stamps `Inner`), which would send the lookup to the wrong
     * struct. The object is always the owner of the field. */
    Type *st = obj.ty;
    if (!st || st->kind != TY_STRUCT || !st->name)
        ERRV(g, "line %zu: field access on a non-struct value", line);

    int idx = struct_field_idx(g, st->name, name);
    if (idx < 0) ERRV(g, "line %zu: unknown field '%s' on '%s'",
                      line, name, st->name);
    if (!ty_of(g, st)) ERRV(g, "line %zu: cannot access '%s'", line, name);

    Type *fty = struct_field_type(g, st->name, idx);
    if (!fty || !ty_of(g, fty))
        ERRV(g, "line %zu: field '%s' has no codegen type", line, name);

    /* StructGEP walks `%Rect* -> field` in one index, so the text backend's
     * `getelementptr inbounds %Rect, %Rect* %r, i32 0, i32 <field>` double
     * index (and the silent wrong-field bug it avoids) is gone. A struct value
     * was never loaded, so obj.v is its address already. */
    LLVMValueRef p = gep_field(g, st, obj.v, idx);
    if (!p) ERRV(g, "line %zu: cannot take the address of '%s'", line, name);

    /* A struct field is returned by address, so a chain like `o1.inner.x`
     * keeps working; scalar fields are loaded into a value as before. The
     * struct travels as a pointer through the rest of the pipeline, matching
     * how a struct variable is already represented. */
    if (fty->kind == TY_STRUCT && fty->name)
        return val_make(fty, p);
    return val_make(fty, LLVMBuildLoad2(g->ab, ty_of(g, fty), p, "f"));
}

static Type *list_type(void) { return type_list(infer_list_elem(NULL, NULL)); }

static Val cg_let_expr(CG *g, Node *n)
{
    /* `let x = e1 in e2` — e1 evaluates into a fresh alloca that only e2
     * sees. The binding registers HERE (the scan never walks expressions):
     * pushed just before the body and truncated right after — the same
     * emit-time registration match arms use, which also keeps two same-name
     * nested let-expressions from aliasing one slot and makes the binding
     * invisible to reads after the expression. The alloca is dominated by
     * every block the body later branches to, so loads inside the body are
     * legal IR. Mirrors codegen_expr.c's cg_let_expr. */
    Type *prev = g->expect;
    g->expect = n->as.letx.annot;
    Val v = cg_expr(g, n->as.letx.init);
    g->expect = prev;

    /* Prefer the annotation so the stored value matches the slot. */
    Type *ty = n->as.letx.annot ? n->as.letx.annot : v.ty;
    v = coerce(g, ty, v, n->line);
    if (g->err[0]) return (Val){ NULL, NULL };
    if (!ty_of(g, ty))
        ERRV(g, "line %zu: cannot store '%s' into a typed local",
             n->line, n->as.letx.name);

    /* A named struct may arrive as an address rather than an aggregate —
     * load it first, exactly as a statement let's store does. */
    if (ty->kind == TY_STRUCT && ty->name && struct_is_addr(v.v))
        v.v = LLVMBuildLoad2(g->ab, ty_of(g, ty), v.v, "ld");

    LLVMValueRef slot = LLVMBuildAlloca(g->ab, ty_of(g, ty), "lv");
    int saved_n = g->locals.n;
    asg_push(&g->locals, n->as.letx.name, ty, slot, n->line);
    LLVMBuildStore(g->ab, v.v, slot);

    Val bv = cg_expr(g, n->as.letx.body);
    asg_truncate(&g->locals, saved_n);   /* the binding dies with the body */
    return bv;
}

static Val cg_expr(CG *g, Node *n)
{
    if (!n) ERRV(g, "internal: null expression");

    switch (n->type) {
    case N_LITERAL:  return cg_literal(g, n);
    case N_LIST_LIT: return cg_list_lit(g, n);
    case N_MAP_LIT:  return cg_map_or_struct_lit(g, n);
    case N_STRUCT_UPDATE: return cg_struct_update(g, n);
    case N_ASSIGN:   return cg_assign_expr(g, n);
    case N_ASSIGN_MEMBER: return cg_assign_mem(g, n);
    case N_EXPR_STMT: return cg_expr(g, n->as.expr_stmt.expr);
    case N_VAR:      return cg_var(g, n);
    case N_FUNC_LIT: return cg_funclit(g, n);
    case N_UNARY:    return cg_unary(g, n);
    case N_BINARY:   return cg_binary(g, n);
    case N_TERNARY:  return cg_ternary(g, n);
    case N_MATCH:    return cg_match(g, n);
    case N_LET_EXPR: return cg_let_expr(g, n);
    case N_CTOR:     return cg_ctor(g, n);
    case N_MEMBER:   return cg_member(g, n);
    case N_INDEX:    return cg_index(g, n);
    case N_CALL:     return cg_call(g, n);
    default:         ERRV(g, "%s", bad(g, n, "this expression"));
    }
}

/* `m["k"]` / `l[0]`.
 *
 * Lowered the same way as the text backend: index first, then container
 * (matching the interpreter's evaluation order so a key with a side effect runs
 * first), and the read goes through the unified lume_index_* entry points
 * rather than a per-shape accessor chosen here. The emitter often cannot tell a
 * list from a map -- `m["p"][1]` hands over an opaque i8* and a runtime map's
 * value type is written down nowhere -- so the container's own kind does the
 * dispatch, in rt.c, once.
 *
 * Strictness (a missing key or out-of-range index stopping the program) has to
 * be a call the emitted program makes, exactly as in the text backend: the
 * read sets g_index_failed and lume_index_error() reports and exits, like every
 * other fatal native condition. The three-block shape (bad / ok, the join is
 * implicit -- the read's result is used directly from the pre-branch block)
 * follows the `?` propagation lowering in cg_call. */
static Val cg_index(CG *g, Node *n)
{
    Val ix  = cg_expr(g, n->as.index.index);
    if (!ix.v) ERRV(g, "line %zu: bad index expression", n->line);
    Val obj = cg_expr(g, n->as.index.obj);
    if (!obj.v) ERRV(g, "line %zu: bad index operand", n->line);

    int known_map  = obj.ty && obj.ty->kind == TY_STRUCT && !obj.ty->name;
    int known_list = obj.ty && obj.ty->kind == TY_LIST;
    if (!known_map && !known_list) {
        Type *k = obj.ty;
        int opaque = k && (k->kind == TY_STRUCT || k->kind == TY_LIST ||
                           k->kind == TY_ANY);
        if (!opaque)
            ERRV(g, "line %zu: cannot index into a value of type '%s'", n->line,
                 k ? src_type_name(k) : "unknown");
    }

    Type *rty = known_list ? (obj.ty->elem ? obj.ty->elem : any_type())
                           : type_anon_struct();
    LLVMTypeRef ret_ty;
    const char *fn;
    switch (rty->kind) {
    case TY_FLOAT:
        ret_ty = g->dbl;  fn = "lume_index_float";  break;
    case TY_STRING: case TY_STRUCT: case TY_LIST: case TY_NULL: case TY_ANY:
    case TY_ENUM:   /* an enum value is the same opaque i8* a container is */
        ret_ty = g->i8ptr; fn = "lume_index_ptr";  break;
    default:
        ret_ty = g->i64;  fn = "lume_index_int";   break;  /* int / bool */
    }

    /* The index is an i64 for a list and an i8* for a map, passed in that order
     * so the runtime uses whichever its container needs. Which slot the index
     * travels in is the one thing knowable here, and it is the same rule the
     * text backend's rt_arg_type() applies: a string / list / struct / result
     * index is a pointer (the key), everything else an integer (the index). An
     * `any` index lowers to i64 on both backends, so a backend that refused it
     * for the wrong reason refuses it the same way as its sibling. */
    int ix_is_str;
    if (ix.ty) {
        switch (ix.ty->kind) {
        case TY_STRING: case TY_LIST: case TY_STRUCT: case TY_RESULT:
            ix_is_str = 1; break;
        default:
            ix_is_str = 0;
        }
    } else ix_is_str = 0;

    if (obj.ty && obj.ty->kind == TY_ANY) {
        /* A tagged-any subscript (j["k"] / j[i]): the container is a
         * LumeSlot*, so the read goes through the slot helpers (rt.c
         * lume_slot_at_key for a string key / lume_slot_at for an int
         * index), which hand back a LumeSlot*. A NULL is a failed read
         * and goes through lume_index_error (SPEC 6.2); the failure
         * branch is unreachable because, unlike the text backend, the
         * LLVM backend has no try() yet — outside try() index_error
         * prints and exits, so falling off the branch cannot happen. */
        const char *sfn = ix_is_str ? "lume_slot_at_key" : "lume_slot_at";
        LLVMTypeRef sp[2];
        LLVMValueRef sv[2];
        if (ix_is_str) {
            sp[0] = g->i8ptr; sp[1] = g->i8ptr;
            sv[0] = obj.v;    sv[1] = ix.v;
        } else {
            sp[0] = g->i8ptr; sp[1] = g->i64;
            sv[0] = obj.v;    sv[1] = ix.v;
        }
        LLVMValueRef sc = LLVMBuildCall2(g->ab,
                LLVMFunctionType(g->i8ptr, sp, 2, 0),
                rt_decl(g, sfn, g->i8ptr, sp, 2),
                sv, 2, "jsl");
        LLVMBasicBlockRef sb_bad = LLVMAppendBasicBlockInContext(g->ctx, g->fn, "jsl_bad");
        LLVMBasicBlockRef sb_ok  = LLVMAppendBasicBlockInContext(g->ctx, g->fn, "jsl_ok");
        LLVMValueRef snull = LLVMBuildIsNull(g->ab, sc, "jslnull");
        if (!DONE(g->cur)) LLVMBuildCondBr(g->ab, snull, sb_bad, sb_ok);
        g->cur = sb_bad; AT(g);
        LLVMValueRef swhat = cg_string_val(g, "index read failed", 17);
        LLVMBuildCall2(g->ab,
                LLVMFunctionType(g->void_ty, (LLVMTypeRef[]){ g->i8ptr }, 1, 0),
                rt_decl(g, "lume_index_error", g->void_ty, (LLVMTypeRef[]){ g->i8ptr }, 1),
                (LLVMValueRef[]){ swhat }, 1, "");
        if (!DONE(g->cur)) LLVMBuildUnreachable(g->ab);
        g->cur = sb_ok; AT(g);
        return val_make(any_type(), sc);
    }

    LLVMTypeRef iparams[3] = { g->i8ptr, g->i64, g->i8ptr };
    LLVMValueRef iargs[3];
    iargs[0] = obj.v;
    if (ix_is_str) {
        iargs[1] = LLVMConstInt(g->i64, 0, 0);
        iargs[2] = ix.v;
    } else {
        iargs[1] = ix.v;
        iargs[2] = LLVMConstNull(g->i8ptr);
    }

    LLVMValueRef call = LLVMBuildCall2(g->ab,
            LLVMFunctionType(ret_ty, iparams, 3, 0),
            rt_decl(g, fn, ret_ty, iparams, 3),
            iargs, 3, "idx");

    /* Strictness, at run time. lume_index_failed() is non-zero when the read
     * did not happen, and lume_index_error() reports and exits. */
    LLVMBasicBlockRef bb_bad = LLVMAppendBasicBlockInContext(g->ctx, g->fn, "idx_bad");
    LLVMBasicBlockRef bb_ok  = LLVMAppendBasicBlockInContext(g->ctx, g->fn, "idx_ok");

    LLVMValueRef failed = LLVMBuildCall2(g->ab,
            LLVMFunctionType(g->i64, NULL, 0, 0),
            rt_decl(g, "lume_index_failed", g->i64, NULL, 0),
            NULL, 0, "idxfail");
    LLVMValueRef cond = LLVMBuildICmp(g->ab, LLVMIntNE, failed,
                                      LLVMConstInt(g->i64, 0, 0), "idxbad");
    if (!DONE(g->cur)) LLVMBuildCondBr(g->ab, cond, bb_bad, bb_ok);

    g->cur = bb_bad; AT(g);
    LLVMValueRef what = cg_string_val(g,
            known_map ? "no such key in this map" : "index out of range",
            known_map ? 23 : 19);
    LLVMBuildCall2(g->ab,
            LLVMFunctionType(g->void_ty, (LLVMTypeRef[]){ g->i8ptr }, 1, 0),
            rt_decl(g, "lume_index_error", g->void_ty, (LLVMTypeRef[]){ g->i8ptr }, 1),
            (LLVMValueRef[]){ what }, 1, "");
    if (!DONE(g->cur)) LLVMBuildUnreachable(g->ab);

    g->cur = bb_ok; AT(g);
    return val_make(rty, call);
}

/* `cond ? then : els`。形状与 cg_binary 的 and/or 短路一致：结果停在
 * alloca 而不是 phi，三个块各写一次。槽类型取 infer_node_type 的统一
 * 结果（typecheck 已保证可统一），分支值 store 前 coerce（int→float）。
 * 只求值选中分支。 */
static Val cg_ternary(CG *g, Node *n)
{
    Type *rty = infer_node_type(g, n);
    if (!rty || rty->kind == TY_ANY)
        ERRV(g, "line %zu: cannot infer ternary result type", n->line);
    LLVMTypeRef slot;
    switch (rty->kind) {
    case TY_FLOAT:             slot = g->dbl;   break;
    case TY_BOOL:              slot = g->i1;    break; /* and/or 同款 i1 槽 */
    case TY_INT:               slot = g->i64;   break;
    default:                   slot = g->i8ptr; break; /* string/list/map/result/closure */
    }

    Val c = cg_expr(g, n->as.ternary.cond);
    if (!c.v) ERRV(g, "line %zu: bad ternary condition", n->line);

    LLVMValueRef res = LLVMBuildAlloca(g->ab, slot, "tv");
    LLVMBasicBlockRef bt = LLVMAppendBasicBlockInContext(g->ctx, g->fn, "then");
    LLVMBasicBlockRef be = LLVMAppendBasicBlockInContext(g->ctx, g->fn, "else");
    LLVMBasicBlockRef lj = LLVMAppendBasicBlockInContext(g->ctx, g->fn, "join");

    if (!DONE(g->cur)) LLVMBuildCondBr(g->ab, c.v, bt, be);

    g->cur = bt; AT(g);
    Val tv = cg_expr(g, n->as.ternary.then);
    if (!tv.v) ERRV(g, "line %zu: bad ternary branch", n->line);
    tv = coerce(g, rty, tv, n->line);
    LLVMBuildStore(g->ab, tv.v, res);
    if (!DONE(g->cur)) LLVMBuildBr(g->ab, lj);

    g->cur = be; AT(g);
    Val ev = cg_expr(g, n->as.ternary.els);
    if (!ev.v) ERRV(g, "line %zu: bad ternary branch", n->line);
    ev = coerce(g, rty, ev, n->line);
    LLVMBuildStore(g->ab, ev.v, res);
    if (!DONE(g->cur)) LLVMBuildBr(g->ab, lj);

    g->cur = lj; AT(g);
    return val_make(rty, LLVMBuildLoad2(g->ab, slot, res, "t"));
}

/* An enum constructor: the interpreter's `{ "$": V, ...payload }` map, built
 * with the same runtime helpers a map literal uses. Kept in step with
 * cg_ctor() in codegen_expr.c (docs/SPEC.md §enum). */
static Val cg_ctor(CG *g, Node *n)
{
    AT(g);
    LLVMTypeRef newp[1] = { g->i8ptr };
    LLVMValueRef nv[1] = { NULL };
    LLVMValueRef m = LLVMBuildCall2(g->ab, LLVMFunctionType(g->i8ptr, NULL, 0, 0),
                                    rt_decl(g, "lume_map_new", g->i8ptr, newp, 0),
                                    nv, 0, "m");
    if (!m) ERRV(g, "line %zu: cannot allocate an enum value", n->line);

    Val k = val_make(type_prim(TY_STRING), cg_string_val(g, "$", 1));
    Val tv = val_make(type_prim(TY_STRING),
                      cg_string_val(g, n->as.ctor.name, strlen(n->as.ctor.name)));
    LLVMTypeRef ats[3] = { g->i8ptr, g->i8ptr, g->i8ptr };
    LLVMValueRef vs[3] = { m, k.v, tv.v };
    Val r = rt_call(g, type_prim(TY_INT), "lume_map_put_s", 3, ats, vs);
    if (!r.v) ERRV(g, "line %zu: cannot store the variant tag", n->line);

    for (int i = 0; i < n->as.ctor.argc; i++) {
        Val v = cg_expr(g, n->as.ctor.args[i]);
        Val fk = val_make(type_prim(TY_STRING),
                          cg_string_val(g, n->as.ctor.fields[i],
                                        strlen(n->as.ctor.fields[i])));
        const char *fn = map_put_fn(v.ty);
        if (!fn)
            ERRV(g, "line %zu: enum payload must be int, float, string or bool",
                 n->line);
        if (v.ty && v.ty->kind == TY_BOOL) v = coerce(g, type_prim(TY_INT), v, n->line);
        LLVMTypeRef pats[3] = { g->i8ptr, g->i8ptr, ty_of(g, v.ty) };
        LLVMValueRef pvs[3] = { m, fk.v, v.v };
        Val rr = rt_call(g, type_prim(TY_INT), fn, 3, pats, pvs);
        if (!rr.v) ERRV(g, "line %zu: cannot store the payload", n->line);
    }
    return val_make(n->as.ctor.ename ? type_enum(n->as.ctor.ename)
                                     : type_anon_struct(), m);
}

static Val cg_match(CG *g, Node *n)
{
    Type *rty = infer_node_type(g, n);
    if (!rty || rty->kind == TY_ANY)
        ERRV(g, "line %zu: cannot infer the match result type", n->line);
    LLVMTypeRef slot;
    switch (rty->kind) {
    case TY_FLOAT:             slot = g->dbl;   break;
    case TY_BOOL:              slot = g->i1;    break;
    case TY_INT:               slot = g->i64;   break;
    default:                   slot = g->i8ptr; break;
    }

    AT(g);
    Val s = cg_expr(g, n->as.matchn.scrutinee);
    if (!s.v) ERRV(g, "line %zu: bad match scrutinee", n->line);

    LLVMValueRef dollark = cg_string_val(g, "$", 1);
    LLVMTypeRef gats[3] = { g->i8ptr, g->i8ptr, g->i8ptr };
    LLVMValueRef tag = LLVMBuildCall2(g->ab,
            LLVMFunctionType(g->i8ptr, gats, 3, 0),
            rt_decl(g, "lume_map_get_s", g->i8ptr, gats, 3),
            (LLVMValueRef[]){ s.v, dollark, LLVMConstNull(g->i8ptr) }, 3, "tag");

    LLVMValueRef res = LLVMBuildAlloca(g->ab, slot, "mv");
    LLVMBasicBlockRef lj = LLVMAppendBasicBlockInContext(g->ctx, g->fn, "mjoin");

    for (int i = 0; i < n->as.matchn.armc; i++) {
        MatchArm *arm = &n->as.matchn.arms[i];
        bool last_arm = i == n->as.matchn.armc - 1;
        MatchPat *p0 = &arm->pats[0];
        if (p0->bindc > MATCH_BIND_MAX)
            ERRV(g, "line %zu: too many match bindings (max %d)",
                 arm->line, MATCH_BIND_MAX);

        /* the arm's bind slots, allocated in the block every pattern block
         * of this arm is entered from (so they dominate the arm block's
         * loads); one set serves all alternatives */
        LLVMValueRef bslots[MATCH_BIND_MAX];
        for (int kb = 0; kb < p0->bindc; kb++) {
            Type *bt = p0->vfield_types ? p0->vfield_types[kb] : NULL;
            LLVMTypeRef blt = bt ? ty_of(g, bt) : NULL;
            if (!blt)
                ERRV(g, "line %zu: match binding '%s' has no codegen type",
                     arm->line, p0->binds[kb]);
            bslots[kb] = LLVMBuildAlloca(g->ab, blt, p0->binds[kb]);
        }

        LLVMBasicBlockRef larm = LLVMAppendBasicBlockInContext(g->ctx, g->fn, "marm");
        LLVMBasicBlockRef lnext =
            (last_arm && !arm->guard) ? NULL
            : LLVMAppendBasicBlockInContext(g->ctx, g->fn, "mnext");
        LLVMBasicBlockRef lbody = arm->guard
            ? LLVMAppendBasicBlockInContext(g->ctx, g->fn, "mguard") : NULL;

        for (int pi = 0; pi < arm->patc; pi++) {
            MatchPat *pat = &arm->pats[pi];
            bool last_pat = pi == arm->patc - 1;
            bool catch_all = last_arm && last_pat;
            LLVMBasicBlockRef lpb =
                LLVMAppendBasicBlockInContext(g->ctx, g->fn, "mpat");
            LLVMBasicBlockRef lmiss = catch_all ? NULL
                                   : last_pat ? lnext
                                   : LLVMAppendBasicBlockInContext(g->ctx, g->fn, "mmiss");
            if (catch_all || strcmp(pat->vname, "_") == 0) {
                if (!DONE(g->cur)) LLVMBuildBr(g->ab, lpb);
            } else {
                LLVMValueRef patk = cg_string_val(g, pat->vname,
                                                  strlen(pat->vname));
                LLVMValueRef eq64 = LLVMBuildCall2(g->ab,
                        LLVMFunctionType(g->i64, gats, 2, 0),
                        rt_decl(g, "lume_str_eq", g->i64,
                                (LLVMTypeRef[]){ g->i8ptr, g->i8ptr }, 2),
                        (LLVMValueRef[]){ tag, patk }, 2, "eq");
                LLVMValueRef eq = LLVMBuildICmp(g->ab, LLVMIntNE, eq64,
                                                LLVMConstInt(g->i64, 0, 0), "isv");
                if (!DONE(g->cur)) LLVMBuildCondBr(g->ab, eq, lpb, lmiss);
            }

            g->cur = lpb; AT(g);
            for (int kb = 0; kb < pat->bindc; kb++) {
                Type *bt = pat->vfield_types ? pat->vfield_types[kb] : NULL;
                const char *gfn = map_get_fn(bt);
                if (!gfn)
                    ERRV(g, "line %zu: match binding '%s' payload must be int, "
                            "float, string or bool", arm->line, pat->binds[kb]);
                /* The rt helper's own spelling rules the call:
                 * lume_map_get_i (the bool reader) returns and takes i64,
                 * while the slot spells i1. Read as i64, narrow after. */
                LLVMTypeRef call_ty = ty_of(g, bt);
                if (bt->kind == TY_BOOL) call_ty = g->i64;
                LLVMValueRef fk = cg_string_val(g, pat->vfields[kb],
                                                strlen(pat->vfields[kb]));
                LLVMTypeRef pf[3] = { g->i8ptr, g->i8ptr, call_ty };
                LLVMValueRef dflt = bt->kind == TY_FLOAT
                                      ? LLVMConstReal(ty_of(g, bt), 0.0)
                                    : call_ty == g->i8ptr
                                      ? LLVMConstNull(g->i8ptr)
                                      : LLVMConstInt(call_ty, 0, 0);
                LLVMValueRef pv = LLVMBuildCall2(g->ab,
                        LLVMFunctionType(call_ty, pf, 3, 0),
                        rt_decl(g, gfn, call_ty, pf, 3),
                        (LLVMValueRef[]){ s.v, fk, dflt }, 3, "pv");
                if (bt->kind == TY_BOOL)
                    pv = LLVMBuildICmp(g->ab, LLVMIntNE, pv,
                                       LLVMConstInt(g->i64, 0, 0), "pb");
                LLVMBuildStore(g->ab, pv, bslots[kb]);
            }
            if (!DONE(g->cur)) LLVMBuildBr(g->ab, larm);
            if (lmiss) { g->cur = lmiss; AT(g); }
        }

        g->cur = larm; AT(g);
        int saved_n = g->locals.n;
        for (int kb = 0; kb < p0->bindc; kb++)
            asg_push(&g->locals, p0->binds[kb],
                     p0->vfield_types ? p0->vfield_types[kb] : NULL,
                     bslots[kb], arm->line);
        if (arm->guard) {
            Val gv = cg_expr(g, arm->guard);
            if (!gv.v) ERRV(g, "line %zu: bad match guard", arm->line);
            if (!DONE(g->cur)) LLVMBuildCondBr(g->ab, gv.v, lbody, lnext);
            g->cur = lbody; AT(g);
        }
        Val bv = cg_expr(g, arm->body);
        if (!bv.v) ERRV(g, "line %zu: bad match arm body", arm->line);
        bv = coerce(g, rty, bv, arm->line);
        LLVMBuildStore(g->ab, bv.v, res);
        asg_truncate(&g->locals, saved_n); /* truncate this arm's binds */
        if (!DONE(g->cur)) LLVMBuildBr(g->ab, lj);
        if (last_arm && arm->guard) {
            /* a guarded last arm falling through is a forged map */
            g->cur = lnext; AT(g);
            LLVMBuildUnreachable(g->ab);
        } else if (lnext) {
            g->cur = lnext; AT(g);
        }
    }

    g->cur = lj; AT(g);
    return val_make(rty, LLVMBuildLoad2(g->ab, slot, res, "mv"));
}

/* `if let P [| P] = value { then } [else ...] }` — the libLLVM mirror of
 * cg_if_let() in codegen_expr.c: one arm's worth of pattern blocks, the
 * first hit binds and runs the then-branch, a miss runs the else-branch.
 * Binds are block-scoped (registered around the then-branch, truncated
 * after). No guards in this form. */
static void cg_if_let(CG *g, Node *n)
{
    AT(g);
    Val v = cg_expr(g, n->as.iflet.value);
    if (!v.v) ERRX(g, "line %zu: bad if-let value", n->line);

    LLVMValueRef dollark = cg_string_val(g, "$", 1);
    LLVMTypeRef gats[3] = { g->i8ptr, g->i8ptr, g->i8ptr };
    LLVMValueRef tag = LLVMBuildCall2(g->ab,
            LLVMFunctionType(g->i8ptr, gats, 3, 0),
            rt_decl(g, "lume_map_get_s", g->i8ptr, gats, 3),
            (LLVMValueRef[]){ v.v, dollark, LLVMConstNull(g->i8ptr) }, 3, "tag");

    MatchPat *p0 = &n->as.iflet.pats[0];
    if (p0->bindc > MATCH_BIND_MAX)
        ERRX(g, "line %zu: too many if-let bindings (max %d)",
             n->line, MATCH_BIND_MAX);
    LLVMValueRef bslots[MATCH_BIND_MAX];
    for (int kb = 0; kb < p0->bindc; kb++) {
        Type *bt = p0->vfield_types ? p0->vfield_types[kb] : NULL;
        LLVMTypeRef blt = bt ? ty_of(g, bt) : NULL;
        if (!blt)
            ERRX(g, "line %zu: if-let binding '%s' has no codegen type",
                 n->line, p0->binds[kb]);
        bslots[kb] = LLVMBuildAlloca(g->ab, blt, p0->binds[kb]);
    }

    LLVMBasicBlockRef lt = LLVMAppendBasicBlockInContext(g->ctx, g->fn, "ilt");
    LLVMBasicBlockRef lel = LLVMAppendBasicBlockInContext(g->ctx, g->fn, "ielse");
    LLVMBasicBlockRef lend = LLVMAppendBasicBlockInContext(g->ctx, g->fn, "iend");

    for (int pi = 0; pi < n->as.iflet.patc; pi++) {
        MatchPat *pat = &n->as.iflet.pats[pi];
        bool last_pat = pi == n->as.iflet.patc - 1;
        LLVMBasicBlockRef lpb =
            LLVMAppendBasicBlockInContext(g->ctx, g->fn, "ipat");
        LLVMBasicBlockRef lmiss = last_pat ? lel
            : LLVMAppendBasicBlockInContext(g->ctx, g->fn, "imiss");
        if (strcmp(pat->vname, "_") == 0) {
            if (!DONE(g->cur)) LLVMBuildBr(g->ab, lpb);
        } else {
            LLVMValueRef patk = cg_string_val(g, pat->vname,
                                              strlen(pat->vname));
            LLVMValueRef eq64 = LLVMBuildCall2(g->ab,
                    LLVMFunctionType(g->i64, gats, 2, 0),
                    rt_decl(g, "lume_str_eq", g->i64,
                            (LLVMTypeRef[]){ g->i8ptr, g->i8ptr }, 2),
                    (LLVMValueRef[]){ tag, patk }, 2, "eq");
            LLVMValueRef eq = LLVMBuildICmp(g->ab, LLVMIntNE, eq64,
                                            LLVMConstInt(g->i64, 0, 0), "isv");
            if (!DONE(g->cur)) LLVMBuildCondBr(g->ab, eq, lpb, lmiss);
        }

        g->cur = lpb; AT(g);
        for (int kb = 0; kb < pat->bindc; kb++) {
            Type *bt = pat->vfield_types ? pat->vfield_types[kb] : NULL;
            const char *gfn = map_get_fn(bt);
            if (!gfn)
                ERRX(g, "line %zu: if-let binding '%s' payload must be int, "
                        "float, string or bool", n->line, pat->binds[kb]);
            LLVMTypeRef call_ty = ty_of(g, bt);
            if (bt->kind == TY_BOOL) call_ty = g->i64;
            LLVMValueRef fk = cg_string_val(g, pat->vfields[kb],
                                            strlen(pat->vfields[kb]));
            LLVMTypeRef pf[3] = { g->i8ptr, g->i8ptr, call_ty };
            LLVMValueRef dflt = bt->kind == TY_FLOAT
                                  ? LLVMConstReal(ty_of(g, bt), 0.0)
                                : call_ty == g->i8ptr
                                  ? LLVMConstNull(g->i8ptr)
                                  : LLVMConstInt(call_ty, 0, 0);
            LLVMValueRef pv = LLVMBuildCall2(g->ab,
                    LLVMFunctionType(call_ty, pf, 3, 0),
                    rt_decl(g, gfn, call_ty, pf, 3),
                    (LLVMValueRef[]){ v.v, fk, dflt }, 3, "pv");
            if (bt->kind == TY_BOOL)
                pv = LLVMBuildICmp(g->ab, LLVMIntNE, pv,
                                   LLVMConstInt(g->i64, 0, 0), "pb");
            LLVMBuildStore(g->ab, pv, bslots[kb]);
        }
        if (!DONE(g->cur)) LLVMBuildBr(g->ab, lt);
        if (lmiss != lel) { g->cur = lmiss; AT(g); }
    }

    g->cur = lt; AT(g);
    int saved_n = g->locals.n;
    for (int kb = 0; kb < p0->bindc; kb++)
        asg_push(&g->locals, p0->binds[kb],
                 p0->vfield_types ? p0->vfield_types[kb] : NULL,
                 bslots[kb], n->line);
    cg_stmt(g, n->as.iflet.then);
    asg_truncate(&g->locals, saved_n);
    if (!DONE(g->cur)) LLVMBuildBr(g->ab, lend);

    g->cur = lel; AT(g);
    if (n->as.iflet.els) cg_stmt(g, n->as.iflet.els);
    if (!DONE(g->cur)) LLVMBuildBr(g->ab, lend);

    g->cur = lend; AT(g);
}

/* The static type of `expr.field` without emitting anything. The entry-block
 * alloca pass runs before any statement is emitted, so a `for (v in bag.xs)`
 * has to be able to answer "what type is v" from the field's declared type —
 * otherwise the loop variable is registered untyped and LLVMBuildAlloca is
 * handed a NULL type. */
static Type *member_field_type(CG *g, Node *n)
{
    Type *t = infer_node_type(g, n->as.member.obj);
    if (t && t->kind == TY_STRUCT) {
        if (t->name) {
            int idx = struct_field_idx(g, t->name, n->as.member.name);
            if (idx >= 0) return struct_field_type(g, t->name, idx);
        } else {
            /* A runtime map: the type checker required the key to be a
             * declared field, so the result type is exactly the field's
             * value type it recorded on the member node. */
            return n->as.member.type;
        }
    }
    return NULL;
}

static Val coerce(CG *g, Type *to, Val v, size_t line)
{
    if (!to || !v.ty || to == v.ty) return v;

    /* `null` is representable natively as an opaque i8* null pointer, and the
     * type checker lets it stand in for any type (`type_compat`: "all types
     * nullable"). But the pointer has no scalar meaning: without this guard
     * the ptrtoint/intcast edges below turned a null into 0 (or false), so
     * `let x: int = null; print(x)` printed a confident, wrong 0 where the
     * interpreter printed `null`. Reject it instead -- silently substituting a
     * value is worse than refusing to compile. Pointer-shaped targets
     * (string/list/struct) stay legal; lume_print_str and friends absorb the
     * null. */
    if (v.ty->kind == TY_NULL && to->kind != TY_STRING && to->kind != TY_LIST &&
        to->kind != TY_STRUCT && to->kind != TY_ANY && to->kind != TY_ENUM)
        ERRV(g, "line %zu: cannot use 'null' as a %s value in the native backend",
             line, src_type_name(to));

    LLVMTypeRef tt = ty_of(g, to);
    LLVMTypeRef vt = ty_of(g, v.ty);
    if (!tt || !vt || tt == vt) return v;

    AT(g);
    LLVMValueRef r;
    if (vt == g->i1 && tt == g->i64)       r = LLVMBuildZExt(g->ab, v.v, tt, "co");
    else if (vt == g->i64 && tt == g->i1)  r = LLVMBuildICmp(g->ab, LLVMIntNE, v.v,
                                                              LLVMConstInt(g->i64, 0, 0), "co");
    else if (vt == g->i64 && tt == g->dbl) r = LLVMBuildSIToFP(g->ab, v.v, tt, "co");
    else if (vt == g->dbl && tt == g->i64) r = LLVMBuildFPToSI(g->ab, v.v, tt, "co");
    else if (vt == g->i8ptr && tt == g->i64) r = LLVMBuildPtrToInt(g->ab, v.v, tt, "co");
    else if (vt == g->i64 && tt == g->i8ptr) r = LLVMBuildIntToPtr(g->ab, v.v, tt, "co");
    else r = LLVMBuildIntCast2(g->ab, v.v, tt, 1, "co");
    if (!r) ERRV(g, "line %zu: cannot convert this value", line);
    return val_make(to, r);
}

/* Which element accessor reads a list whose element type is `ty`. */
static const char *list_at_fn(Type *ty)
{
    if (!ty) return NULL;
    switch (ty->kind) {
    case TY_FLOAT:  return "lume_list_at_f";
    case TY_STRING: return "lume_list_at_s";
    case TY_INT:
    case TY_BOOL:   return "lume_list_at_i";
    /* Same branch as codegen_stmt.c's shared copy: an enum value is the
     * opaque i8* at_obj hands back, and the loop variable keeps its enum
     * type so print/match dispatch stay correct. A nested list element is
     * the same opaque i8* — `map((xs) => ..., [[1,2],[3,4]])` reads each
     * inner list as an object. */
    case TY_ENUM:
    case TY_LIST:   return "lume_list_at_obj";
    default:        return NULL;
    }
}

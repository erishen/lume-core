/* ---- Gradient Boosting Regressor (2026-11-26) ----------------------------
 * gbdt_fit(X, y, rounds, depth, lr, min_leaf) -> model (map)
 * gbdt_predict(model, X) -> float list
 *
 * Squared-error gradient boosting over regression trees. This covers the
 * reference implementation's HistGradientBoostingRegressor use case with a
 * dependency-free C builtin, so a .lume pipeline can train / register / score
 * a signal model without sklearn. Pure interpreter path; no LLVM backend
 * parity claim (like server{}/route, this is an interpreter-only builtin).
 *
 * Model is a lume map with GC-owned lists:
 *   { "base": float, "lr": float,
 *     "trees": [ [f, thr, l, r, leaf, is_leaf, ...], ... ] }
 * Each tree is a pre-order flat node array, 6 doubles per node:
 *   [feature_index, threshold, left_child_idx, right_child_idx, value, is_leaf]
 * Child indices are node indices within the same tree (-1 for leaves).
 * list-of-lists keeps the model inside lume's GC, so no manual lifetime.
 */

/* ---------- self-contained helpers (both trees: core + host) ----------
 * Named with gb_ prefix so this file can be #included by either tree's
 * builtins_math.c without depending on that file's local helper names
 * (core uses gb_arg_list/gb_cmp_d, host uses m54_arg_list/...). */
static bool gb_arg_list(VM *vm, Value v, ObjList **out) {
    if (!IS_OBJ(v) || AS_OBJ(v)->type != OBJ_LIST) {
        vm_set_error(vm, "gbdt: expected list argument");
        return false;
    }
    *out = &AS_OBJ(v)->as.list;
    return true;
}
static bool gb_list_num(VM *vm, ObjList *L, int i, double *out) {
    Value v = L->items[i];
    if (IS_INT(v))   { *out = (double)AS_INT(v); return true; }
    if (IS_FLOAT(v)) { *out = v.as.n;            return true; }
    vm_set_error(vm, "gbdt: expected numeric matrix");
    return false;
}
static int gb_cmp_d(const void *a, const void *b) {
    double x = *(const double *)a, y = *(const double *)b;
    return x < y ? -1 : (x > y ? 1 : 0);
}
static double *gb_list_to_doubles(VM *vm, ObjList *L, int *n) {
    *n = L->count;
    double *xs = (double *)malloc((size_t)(*n ? *n : 1) * sizeof(double));
    if (!xs) { vm_set_error(vm, "gbdt: out of memory"); return NULL; }
    for (int i = 0; i < *n; i++)
        if (!gb_list_num(vm, L, i, &xs[i])) { free(xs); return NULL; }
    return xs;
}

/* ---------- private C-side tree representation ---------- */
typedef struct {
    int   feature;      /* split feature, -1 for leaf */
    double threshold;
    int   left, right;  /* child node indices, -1 for leaf */
    double value;       /* leaf prediction */
    int   is_leaf;
} GbNode;

typedef struct {
    GbNode *nodes;
    int n, cap;
} GbTree;

static void gbdt_node_push(GbTree *t, GbNode nd) {
    if (t->n == t->cap) {
        int nc = t->cap ? t->cap * 2 : 16;
        GbNode *p = (GbNode *)realloc(t->nodes, (size_t)nc * sizeof(GbNode));
        if (!p) return; /* caller checks t->n growth; OOM -> truncated tree */
        t->nodes = p; t->cap = nc;
    }
    t->nodes[t->n++] = nd;
}

/* Fit one regression tree on residuals with greedy CART (squared error). */
static int gbdt_grow(GbTree *t, const double *X, int n, int p,
                     const int *rows, int m, const double *res,
                     int depth, int max_depth, int min_leaf) {
    GbNode nd;
    nd.feature = -1; nd.threshold = 0.0; nd.left = -1; nd.right = -1;
    nd.value = 0.0; nd.is_leaf = 1;

    double sum = 0.0;
    for (int i = 0; i < m; i++) sum += res[rows[i]];
    nd.value = sum / (double)m;

    if (depth >= max_depth || m < 2 * min_leaf) {
        gbdt_node_push(t, nd);
        return t->n - 1;
    }
    /* best split by total within-node SSE reduction */
    double best_gain = 0.0;
    int best_f = -1; double best_thr = 0.0;
    double *vals = (double *)malloc((size_t)m * sizeof(double));
    if (!vals) { gbdt_node_push(t, nd); return t->n - 1; }
    for (int f = 0; f < p; f++) {
        for (int i = 0; i < m; i++) vals[i] = X[rows[i] * p + f];
        qsort(vals, (size_t)m, sizeof(double), gb_cmp_d);
        for (int s = min_leaf; s <= m - min_leaf; s++) {
            double thr = (s < m) ? 0.5 * (vals[s - 1] + vals[s]) : vals[s - 1];
            if (s < m && vals[s - 1] == vals[s]) continue;
            double sl = 0.0, sr = 0.0;
            for (int i = 0; i < m; i++) {
                if (X[rows[i] * p + f] <= thr) sl += res[rows[i]];
                else sr += res[rows[i]];
            }
            double gain = sl * sl / (double)s + sr * sr / (double)(m - s)
                        - sum * sum / (double)m;
            if (gain > best_gain) { best_gain = gain; best_f = f; best_thr = thr; }
        }
    }
    free(vals);
    if (best_f < 0) { gbdt_node_push(t, nd); return t->n - 1; }

    int *left = (int *)malloc((size_t)m * sizeof(int));
    int *right = (int *)malloc((size_t)m * sizeof(int));
    if (!left || !right) {
        free(left); free(right); gbdt_node_push(t, nd); return t->n - 1;
    }
    int nl = 0, nr = 0;
    for (int i = 0; i < m; i++) {
        if (X[rows[i] * p + best_f] <= best_thr) left[nl++] = rows[i];
        else right[nr++] = rows[i];
    }
    nd.is_leaf = 0; nd.feature = best_f; nd.threshold = best_thr;
    int self = t->n;
    gbdt_node_push(t, nd);
    /* re-fetch: realloc may have moved nodes */
    int li = gbdt_grow(t, X, n, p, left,  nl, res, depth + 1, max_depth, min_leaf);
    int ri = gbdt_grow(t, X, n, p, right, nr, res, depth + 1, max_depth, min_leaf);
    t->nodes[self].left = li;
    t->nodes[self].right = ri;
    free(left); free(right);
    return self;
}

/* Score one C-side tree (used by fit to update residuals in place). */
static double gbdt_nodes_value(const GbTree *t, const double *x, int p) {
    int idx = 0;
    while (idx >= 0 && idx < t->n) {
        GbNode *nd = &t->nodes[idx];
        if (nd->is_leaf) return nd->value;
        if (nd->feature < 0 || nd->feature >= p) return 0.0;
        idx = (x[nd->feature] <= nd->threshold) ? nd->left : nd->right;
    }
    return 0.0;
}

static double gbdt_tree_value(const double *flat, int cnt, const double *x, int p) {
    int idx = 0;
    while (idx >= 0 && idx < cnt) {
        int base = idx * 6;
        if (flat[base + 5] > 0.5) return flat[base + 4];
        int f = (int)flat[base];
        if (f < 0 || f >= p) return 0.0;
        idx = (x[f] <= flat[base + 1]) ? (int)flat[base + 2] : (int)flat[base + 3];
    }
    return 0.0;
}

/* Parse X as row-major n x p double matrix. Caller frees *out. */
static double *gbdt_matrix(VM *vm, ObjList *X, int *n, int *p) {
    *n = X->count;
    if (*n <= 0) { vm_set_error(vm, "gbdt: X must be a non-empty list of rows"); return NULL; }
    ObjList *r0;
    if (!gb_arg_list(vm, X->items[0], &r0)) return NULL;
    *p = r0->count;
    if (*p <= 0) { vm_set_error(vm, "gbdt: rows must be non-empty"); return NULL; }
    double *m = (double *)malloc((size_t)(*n) * (size_t)(*p) * sizeof(double));
    if (!m) { vm_set_error(vm, "gbdt: out of memory"); return NULL; }
    for (int i = 0; i < *n; i++) {
        ObjList *row;
        if (!gb_arg_list(vm, X->items[i], &row) || row->count != *p) {
            free(m); vm_set_error(vm, "gbdt: ragged X matrix"); return NULL;
        }
        for (int j = 0; j < *p; j++)
            if (!gb_list_num(vm, row, j, &m[i * (*p) + j])) { free(m); return NULL; }
    }
    return m;
}

static void native_gbdt_fit(VM *vm, int argc, Value *args, Value *out) {
    ObjList *X, *y;
    if (argc < 2 || !gb_arg_list(vm, args[0], &X) || !gb_arg_list(vm, args[1], &y)) return;
    double d_rounds = 40, d_depth = 3, d_min_leaf = 5, lr = 0.1;
    if (argc > 2 && !arg_num(vm, args[2], &d_rounds)) return;
    if (argc > 3 && !arg_num(vm, args[3], &d_depth)) return;
    if (argc > 4 && !arg_num(vm, args[4], &lr)) return;
    if (argc > 5 && !arg_num(vm, args[5], &d_min_leaf)) return;
    int rounds = (int)d_rounds, depth = (int)d_depth, min_leaf = (int)d_min_leaf;
    if (rounds < 1 || rounds > 500 || depth < 1 || depth > 12 ||
        lr <= 0.0 || lr > 10.0 || min_leaf < 1) {
        vm_set_error(vm, "gbdt_fit: expect rounds in [1,500], depth in [1,12], lr>0, min_leaf>=1");
        return;
    }
    int n, p;
    double *mat = gbdt_matrix(vm, X, &n, &p);
    if (!mat) return;
    if (y->count != n) { free(mat); vm_set_error(vm, "gbdt_fit: len(y) must equal rows(X)"); return; }
    double *ys = gb_list_to_doubles(vm, y, &n);
    if (!ys) { free(mat); return; }

    double ysum = 0.0;
    for (int i = 0; i < n; i++) ysum += ys[i];
    double base = ysum / (double)n;
    double *res = (double *)malloc((size_t)n * sizeof(double));
    int *rows = (int *)malloc((size_t)n * sizeof(int));
    if (!res || !rows) { free(mat); free(ys); free(res); free(rows);
        vm_set_error(vm, "gbdt_fit: out of memory"); return; }
    for (int i = 0; i < n; i++) { res[i] = ys[i] - base; rows[i] = i; }

    Obj *trees = AS_OBJ(make_list(vm));
    vm_push(vm, val_obj(trees));
    for (int r = 0; r < rounds; r++) {
        GbTree t = {0};
        gbdt_grow(&t, mat, n, p, rows, n, res, 0, depth, min_leaf);
        Obj *flat = AS_OBJ(make_list(vm));
        vm_push(vm, val_obj(flat));
        for (int i = 0; i < t.n; i++) {
            GbNode *nd = &t.nodes[i];
            double v[6] = { (double)nd->feature, nd->threshold,
                            (double)nd->left, (double)nd->right,
                            nd->value, (double)nd->is_leaf };
            for (int k = 0; k < 6; k++) list_push(vm, flat, val_num(v[k]));
        }
        vm_pop(vm);
        list_push(vm, trees, val_obj(flat));
        /* update residuals: res -= lr * h(x) */
        for (int i = 0; i < n; i++) {
            double *xi = mat + (size_t)i * p;
            res[i] -= lr * gbdt_nodes_value(&t, xi, p);
        }
        free(t.nodes);
    }

    Obj *model = AS_OBJ(make_map(vm));
    vm_push(vm, val_obj(model));
    map_set(vm, model, "base", val_num(base));
    map_set(vm, model, "lr", val_num(lr));
    map_set(vm, model, "trees", val_obj(trees));
    map_set(vm, model, "n_features", val_num((double)p));
    vm_pop(vm); /* model */
    vm_pop(vm); /* trees */

    free(mat); free(ys); free(res); free(rows);
    *out = val_obj(model);
}

static void native_gbdt_predict(VM *vm, int argc, Value *args, Value *out) {
    if (argc < 2) { vm_set_error(vm, "gbdt_predict(model, X) needs 2 args"); return; }
    if (!IS_OBJ(args[0]) || AS_OBJ(args[0])->type != OBJ_MAP) {
        vm_set_error(vm, "gbdt_predict: first arg must be a model map"); return;
    }
    Obj *model = AS_OBJ(args[0]);
    int found = 0;
    Value vb = map_get(vm, model, "base", &found);
    if (!found) { vm_set_error(vm, "gbdt_predict: model lacks 'base'"); return; }
    double base = (IS_INT(vb)) ? (double)AS_INT(vb) :
                  (IS_FLOAT(vb)) ? vb.as.n : NAN;
    Value vlr = map_get(vm, model, "lr", &found);
    double lr = (IS_FLOAT(vlr)) ? vlr.as.n : (IS_INT(vlr)) ? (double)AS_INT(vlr) : 1.0;
    Value vt = map_get(vm, model, "trees", &found);
    if (!found || !IS_OBJ(vt) || AS_OBJ(vt)->type != OBJ_LIST) {
        vm_set_error(vm, "gbdt_predict: model lacks 'trees' list"); return;
    }
    ObjList *trees = &AS_OBJ(vt)->as.list;
    ObjList *X;
    if (!gb_arg_list(vm, args[1], &X)) return;
    int n, p;
    double *mat = gbdt_matrix(vm, X, &n, &p);
    if (!mat) return;

    /* materialize flat tree arrays from lume lists */
    double **flats = (double **)malloc((size_t)(trees->count ? trees->count : 1) * sizeof(double *));
    int *cnts = (int *)malloc((size_t)(trees->count ? trees->count : 1) * sizeof(int));
    if (!flats || !cnts) { free(flats); free(cnts); free(mat);
        vm_set_error(vm, "gbdt_predict: out of memory"); return; }
    for (int t = 0; t < trees->count; t++) {
        ObjList *tl;
        if (!gb_arg_list(vm, trees->items[t], &tl) || tl->count % 6 != 0) {
            free(flats); free(cnts); free(mat);
            vm_set_error(vm, "gbdt_predict: malformed tree in model"); return;
        }
        cnts[t] = tl->count / 6;
        flats[t] = (double *)malloc((size_t)(tl->count ? tl->count : 1) * sizeof(double));
        for (int k = 0; k < tl->count; k++) {
            Value v = tl->items[k];
            flats[t][k] = IS_FLOAT(v) ? v.as.n : (IS_INT(v) ? (double)AS_INT(v) : 0.0);
        }
    }

    Obj *res = AS_OBJ(make_list(vm));
    vm_push(vm, val_obj(res));
    for (int i = 0; i < n; i++) {
        double s = base;
        double *xi = mat + (size_t)i * p;
        for (int t = 0; t < trees->count; t++)
            s += lr * gbdt_tree_value(flats[t], cnts[t], xi, p);
        list_push(vm, res, val_num(s));
    }
    vm_pop(vm);
    for (int t = 0; t < trees->count; t++) free(flats[t]);
    free(flats); free(cnts); free(mat);
    *out = val_obj(res);
}

Value b_gbdt_fit(VM *vm, int argc, Value *args)     { return vm_native(vm, argc, args, native_gbdt_fit); }
Value b_gbdt_predict(VM *vm, int argc, Value *args) { return vm_native(vm, argc, args, native_gbdt_predict); }

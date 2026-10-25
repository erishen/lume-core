/* ---- 量化 builtins (2026-10-24): 描述统计 / 相关回归 / 时间序列 ----
 * 与 rt.c 的 lume_bi_* 同一套公式(interp / 原生后端一致)。列表元素
 * int/float 都接受(list_num 宽化),退化输入(空 / 过短 / 非数字 / 长度
 * 不等)一律 vm_set_error。 */

static bool arg_list(VM *vm, Value v, ObjList **out) {
    if (!IS_OBJ(v) || AS_OBJ(v)->type != OBJ_LIST) {
        vm_set_error(vm, "expected list argument");
        return false;
    }
    *out = &AS_OBJ(v)->as.list;
    return true;
}

static bool list_num(VM *vm, ObjList *L, int i, double *out) {
    Value v = L->items[i];
    if (IS_INT(v))   { *out = (double)AS_INT(v); return true; }
    if (IS_FLOAT(v)) { *out = v.as.n;            return true; }
    vm_set_error(vm, "quant builtin needs a list of numbers");
    return false;
}

static double *list_to_doubles(VM *vm, ObjList *L, int *n) {
    *n = L->count;
    double *xs = (double *)malloc((size_t)(*n ? *n : 1) * sizeof(double));
    if (!xs) { vm_set_error(vm, "out of memory"); return NULL; }
    for (int i = 0; i < *n; i++)
        if (!list_num(vm, L, i, &xs[i])) { free(xs); return NULL; }
    return xs;
}

/* Fisher-Yates on a fresh list (input untouched): the j draw inlines
 * bi_rng_next's two-output 64-bit reduction exactly like native_rand_int,
 * so interpreter and native backends consume the same stream. */
static void native_shuffle(VM *vm, int argc, Value *args, Value *out) {
    ObjList *L;
    if (argc < 1 || !arg_list(vm, args[0], &L)) return;
    Obj *res = AS_OBJ(make_list(vm));
    vm_push(vm, val_obj(res));
    int n; double *xs = list_to_doubles(vm, L, &n);
    if (xs) {
        for (int i = n - 1; i > 0; i--) {
            uint64_t span = (uint64_t)i + 1ULL;
            uint64_t r = (uint64_t)bi_rng_next() | ((uint64_t)bi_rng_next() << 32u);
            int64_t j = (int64_t)(r % span);
            double t = xs[i]; xs[i] = xs[j]; xs[j] = t;
        }
        for (int i = 0; i < n; i++) list_push(vm, res, val_num(xs[i]));
        free(xs);
    }
    vm_pop(vm);
    *out = val_obj(res);
}

static int cmp_d(const void *a, const void *b) {
    double x = *(const double *)a, y = *(const double *)b;
    return x < y ? -1 : (x > y ? 1 : 0);
}

static void native_mean(VM *vm, int argc, Value *args, Value *out) {
    ObjList *L;
    if (argc < 1 || !arg_list(vm, args[0], &L)) return;
    if (L->count == 0) { vm_set_error(vm, "mean() needs a non-empty list"); return; }
    int n; double *xs = list_to_doubles(vm, L, &n);
    if (!xs) return;
    double s = 0.0;
    for (int i = 0; i < n; i++) s += xs[i];
    free(xs);
    *out = val_num(s / (double)n);
}

static void native_variance(VM *vm, int argc, Value *args, Value *out) {
    ObjList *L;
    if (argc < 1 || !arg_list(vm, args[0], &L)) return;
    if (L->count < 2) { vm_set_error(vm, "variance() needs >= 2 elements"); return; }
    int n; double *xs = list_to_doubles(vm, L, &n);
    if (!xs) return;
    /* Welford's single-pass algorithm (mirrors rt.c lume_bi_variance):
     * stable under large offsets, identical on ordinary data. */
    double m = 0.0, m2 = 0.0;
    for (int i = 0; i < n; i++) {
        double d = xs[i] - m;
        m += d / (double)(i + 1);
        m2 += d * (xs[i] - m);
    }
    free(xs);
    *out = val_num(m2 / (double)(n - 1));   /* sample variance, n-1 */
}

static void native_stddev(VM *vm, int argc, Value *args, Value *out) {
    ObjList *L;
    if (argc < 1 || !arg_list(vm, args[0], &L)) return;
    if (L->count < 2) { vm_set_error(vm, "stddev() needs >= 2 elements"); return; }
    int n; double *xs = list_to_doubles(vm, L, &n);
    if (!xs) return;
    double m = 0.0, m2 = 0.0;
    for (int i = 0; i < n; i++) {
        double d = xs[i] - m;
        m += d / (double)(i + 1);
        m2 += d * (xs[i] - m);
    }
    free(xs);
    *out = val_num(sqrt(m2 / (double)(n - 1)));
}

static void native_median(VM *vm, int argc, Value *args, Value *out) {
    ObjList *L;
    if (argc < 1 || !arg_list(vm, args[0], &L)) return;
    if (L->count == 0) { vm_set_error(vm, "median() needs a non-empty list"); return; }
    int n; double *xs = list_to_doubles(vm, L, &n);
    if (!xs) return;
    qsort(xs, (size_t)n, sizeof(double), cmp_d);
    double med = (n % 2) ? xs[n / 2] : 0.5 * (xs[n / 2 - 1] + xs[n / 2]);
    free(xs);
    *out = val_num(med);
}

static void native_percentile(VM *vm, int argc, Value *args, Value *out) {
    ObjList *L; double p;
    if (argc < 2 || !arg_list(vm, args[0], &L) || !arg_num(vm, args[1], &p)) return;
    if (p < 0.0 || p > 100.0) { vm_set_error(vm, "percentile() needs p in [0,100]"); return; }
    if (L->count == 0) { vm_set_error(vm, "percentile() needs a non-empty list"); return; }
    int n; double *xs = list_to_doubles(vm, L, &n);
    if (!xs) return;
    qsort(xs, (size_t)n, sizeof(double), cmp_d);
    double r = p / 100.0 * (double)(n - 1);
    int lo = (int)r;
    double frac = r - (double)lo;
    double v = lo + 1 < n ? xs[lo] + frac * (xs[lo + 1] - xs[lo]) : xs[lo];
    free(xs);
    *out = val_num(v);
}

/* ---- bootstrap / resampling family (2026-10-30) ----
 * Mirrors rt.c bi_bootstrap_* step for step: each replicate picks n iid
 * uniform indices over [0,n) (one bi_rng_next per position — the
 * rand_multinomial convention), the statistic is evaluated on the
 * resampled series (mean / median / sample stddev), the sorted
 * replicates answer the alpha/2 and 1-alpha/2 quantiles by the pandas
 * linear interpolation (the percentile rule above); se_mean answers the
 * sample stddev (n-1) of the replicate means (the Efron convention).
 * Invalid parameters error here, empty list / NAN natively. */

static double boot_mean(const double *xs, int64_t n)
{
    double s = 0.0;
    for (int64_t i = 0; i < n; i++) s += xs[i];
    return s / (double)n;
}

static double boot_stddev(const double *xs, int64_t n)
{
    double m = 0.0, m2 = 0.0;
    for (int64_t i = 0; i < n; i++) {
        double d = xs[i] - m;
        m += d / (double)(i + 1);
        m2 += d * (xs[i] - m);
    }
    return sqrt(m2 / (double)(n - 1));
}

static double boot_median(double *xs, int64_t n)
{
    /* sorts in place: xs is a freshly-resampled temporary array */
    qsort(xs, (size_t)n, sizeof(double), cmp_d);
    return (n % 2) ? xs[n / 2] : 0.5 * (xs[n / 2 - 1] + xs[n / 2]);
}

static double *boot_resample(const double *xs, int64_t n)
{
    double *r = (double *)malloc((size_t)n * sizeof(double));
    if (!r) return NULL;
    for (int64_t j = 0; j < n; j++) {
        int64_t idx = (int64_t)((double)(bi_rng_next() >> 8) *
                                0.000000059604644775390625 * (double)n);
        r[j] = xs[idx];
    }
    return r;
}

static double boot_pctl_sorted(const double *v, int64_t m, double p100)
{
    double r = p100 / 100.0 * (double)(m - 1);
    int64_t lo = (int64_t)r;
    double frac = r - (double)lo;
    return lo + 1 < m ? v[lo] + frac * (v[lo + 1] - v[lo]) : v[lo];
}

static void native_bootstrap_ci(VM *vm, ObjList *L, double n_iter, double alpha,
                                int which, const char *name, Value *out) {
    int n; double *xs = list_to_doubles(vm, L, &n);
    if (!xs) return;
    if (n < 2 || !isfinite(n_iter) || n_iter < 1.0 || !isfinite(alpha) ||
        alpha <= 0.0 || alpha >= 1.0) {
        free(xs); vm_set_error(vm, "%s(): invalid parameters (n >= 2, n_iter >= 1, alpha in (0,1))", name);
        return;
    }
    int64_t it = (int64_t)n_iter;
    double *vals = (double *)malloc((size_t)it * sizeof(double));
    if (!vals) { free(xs); vm_set_error(vm, "out of memory"); return; }
    for (int64_t r = 0; r < it; r++) {
        double *bs = boot_resample(xs, n);
        if (!bs) { free(vals); free(xs); vm_set_error(vm, "out of memory"); return; }
        vals[r] = (which == 1) ? boot_median(bs, n)
                 : (which == 2) ? boot_stddev(bs, n)
                                : boot_mean(bs, n);
        free(bs);
    }
    free(xs);
    qsort(vals, (size_t)it, sizeof(double), cmp_d);
    double lo = boot_pctl_sorted(vals, it, 50.0 * alpha);
    double hi = boot_pctl_sorted(vals, it, 100.0 - 50.0 * alpha);
    free(vals);
    Obj *res = AS_OBJ(make_list(vm));
    vm_push(vm, val_obj(res));
    list_push(vm, res, val_num(lo));
    list_push(vm, res, val_num(hi));
    vm_pop(vm);
    *out = val_obj(res);
}

static void native_bootstrap_mean_ci(VM *vm, int argc, Value *args, Value *out) {
    ObjList *L; double n_iter, alpha;
    if (argc < 3 || !arg_list(vm, args[0], &L) || !arg_num(vm, args[1], &n_iter) ||
        !arg_num(vm, args[2], &alpha)) return;
    native_bootstrap_ci(vm, L, n_iter, alpha, 0, "bootstrap_mean_ci", out);
}

static void native_bootstrap_median_ci(VM *vm, int argc, Value *args, Value *out) {
    ObjList *L; double n_iter, alpha;
    if (argc < 3 || !arg_list(vm, args[0], &L) || !arg_num(vm, args[1], &n_iter) ||
        !arg_num(vm, args[2], &alpha)) return;
    native_bootstrap_ci(vm, L, n_iter, alpha, 1, "bootstrap_median_ci", out);
}

static void native_bootstrap_stddev_ci(VM *vm, int argc, Value *args, Value *out) {
    ObjList *L; double n_iter, alpha;
    if (argc < 3 || !arg_list(vm, args[0], &L) || !arg_num(vm, args[1], &n_iter) ||
        !arg_num(vm, args[2], &alpha)) return;
    native_bootstrap_ci(vm, L, n_iter, alpha, 2, "bootstrap_stddev_ci", out);
}

static void native_bootstrap_se_mean(VM *vm, int argc, Value *args, Value *out) {
    ObjList *L; double n_iter;
    if (argc < 2 || !arg_list(vm, args[0], &L) || !arg_num(vm, args[1], &n_iter)) return;
    int n; double *xs = list_to_doubles(vm, L, &n);
    if (!xs) return;
    if (n < 2 || !isfinite(n_iter) || n_iter < 1.0) {
        free(xs); vm_set_error(vm, "bootstrap_se_mean(): invalid parameters (n >= 2, n_iter >= 1)");
        return;
    }
    int64_t it = (int64_t)n_iter;
    double *vals = (double *)malloc((size_t)it * sizeof(double));
    if (!vals) { free(xs); vm_set_error(vm, "out of memory"); return; }
    for (int64_t r = 0; r < it; r++) {
        double *bs = boot_resample(xs, n);
        if (!bs) { free(vals); free(xs); vm_set_error(vm, "out of memory"); return; }
        vals[r] = boot_mean(bs, n);
        free(bs);
    }
    free(xs);
    double m = 0.0, m2 = 0.0;
    for (int64_t i = 0; i < it; i++) {
        double d = vals[i] - m;
        m += d / (double)(i + 1);
        m2 += d * (vals[i] - m);
    }
    free(vals);
    if (it < 2) { vm_set_error(vm, "bootstrap_se_mean(): need n_iter >= 2"); return; }
    *out = val_num(sqrt(m2 / (double)(it - 1)));
}

/* ---- portfolio / risk metrics (2026-10-31) ----
 * Mirrors rt.c lume_bi_* step for step: value_at_risk answers the
 * negative of the level-quantile (pandas linear interpolation, the
 * percentile rule above) — a positive loss figure; expected_shortfall
 * the negative mean of every observation at or below that quantile;
 * sharpe_ratio (mean - rf) / sample stddev (n-1); sortino_ratio
 * (mean - rf) / downside deviation (n denominator). Deterministic, no
 * RNG. Invalid parameters error here, NAN natively. */

static void native_value_at_risk(VM *vm, int argc, Value *args, Value *out) {
    ObjList *L; double level;
    if (argc < 2 || !arg_list(vm, args[0], &L) || !arg_num(vm, args[1], &level)) return;
    if (!isfinite(level) || level <= 0.0 || level >= 1.0) {
        vm_set_error(vm, "value_at_risk() needs level in (0,1)"); return;
    }
    int n; double *xs = list_to_doubles(vm, L, &n);
    if (!xs) return;
    if (n < 1) { free(xs); vm_set_error(vm, "value_at_risk() needs a non-empty list"); return; }
    qsort(xs, (size_t)n, sizeof(double), cmp_d);
    double q = boot_pctl_sorted(xs, n, level * 100.0);
    free(xs);
    *out = val_num(-q);
}

static void native_expected_shortfall(VM *vm, int argc, Value *args, Value *out) {
    ObjList *L; double level;
    if (argc < 2 || !arg_list(vm, args[0], &L) || !arg_num(vm, args[1], &level)) return;
    if (!isfinite(level) || level <= 0.0 || level >= 1.0) {
        vm_set_error(vm, "expected_shortfall() needs level in (0,1)"); return;
    }
    int n; double *xs = list_to_doubles(vm, L, &n);
    if (!xs) return;
    if (n < 1) { free(xs); vm_set_error(vm, "expected_shortfall() needs a non-empty list"); return; }
    qsort(xs, (size_t)n, sizeof(double), cmp_d);
    double q = boot_pctl_sorted(xs, n, level * 100.0);
    double s = 0.0; int64_t k = 0;
    while (k < n && xs[k] <= q) { s += xs[k]; k++; }
    free(xs);
    if (k < 1) { vm_set_error(vm, "expected_shortfall() has no tail observations"); return; }
    *out = val_num(-(s / (double)k));
}

static void native_sharpe_ratio(VM *vm, int argc, Value *args, Value *out) {
    ObjList *L; double rf;
    if (argc < 2 || !arg_list(vm, args[0], &L) || !arg_num(vm, args[1], &rf)) return;
    if (!isfinite(rf)) { vm_set_error(vm, "sharpe_ratio() needs a finite risk-free rate"); return; }
    int n; double *xs = list_to_doubles(vm, L, &n);
    if (!xs) return;
    if (n < 2) { free(xs); vm_set_error(vm, "sharpe_ratio() needs at least two observations"); return; }
    double m = 0.0, m2 = 0.0;
    for (int i = 0; i < n; i++) {
        double d = xs[i] - m;
        m += d / (double)(i + 1);
        m2 += d * (xs[i] - m);
    }
    free(xs);
    double sd = sqrt(m2 / (double)(n - 1));
    /* zero or non-finite volatility is a data property, not a parameter
     * error: answer NAN like the native backends (fuzz series with no
     * spread) instead of aborting the run. */
    if (!(sd > 0.0) || !isfinite(m)) { *out = val_num(NAN); return; }
    *out = val_num((m - rf) / sd);
}

static void native_sortino_ratio(VM *vm, int argc, Value *args, Value *out) {
    ObjList *L; double rf;
    if (argc < 2 || !arg_list(vm, args[0], &L) || !arg_num(vm, args[1], &rf)) return;
    if (!isfinite(rf)) { vm_set_error(vm, "sortino_ratio() needs a finite risk-free rate"); return; }
    int n; double *xs = list_to_doubles(vm, L, &n);
    if (!xs) return;
    if (n < 2) { free(xs); vm_set_error(vm, "sortino_ratio() needs at least two observations"); return; }
    double m = 0.0, dd = 0.0;
    for (int i = 0; i < n; i++) {
        m += xs[i];
        double d = xs[i] - rf;
        if (d < 0.0) dd += d * d;
    }
    free(xs);
    m /= (double)n;
    double ds = sqrt(dd / (double)n);
    /* zero downside deviation (a series with no observations below rf) is
     * a data property, not a parameter error: answer NAN like the native
     * backends instead of aborting the run. */
    if (!(ds > 0.0) || !isfinite(m)) { *out = val_num(NAN); return; }
    *out = val_num((m - rf) / ds);
}

/* ---- portfolio layer (2026-11-01) ----
 * Mirrors rt.c lume_bi_sim_portfolio step for step: paths is a list of
 * equal-length strictly-positive finite price paths (each a list, length
 * >= 2), weights is a list of finite numbers, one per path (the sum is
 * not normalised); the answer is the portfolio-return series
 * r_t = sum_i w_i * (p_{i,t} / p_{i,t-1} - 1) as a fresh float list,
 * ready to feed the #48 risk metrics. Invalid shapes error here, empty
 * list natively. */
static ObjList *interp_list_at_list(Value v)
{
    if (!IS_OBJ(v)) return NULL;
    Obj *o = AS_OBJ(v);
    if (o->type != OBJ_LIST) return NULL;
    return &o->as.list;
}

static void native_sim_portfolio(VM *vm, int argc, Value *args, Value *out) {
    ObjList *P, *W;
    if (argc < 2 || !arg_list(vm, args[0], &P) || !arg_list(vm, args[1], &W)) return;
    if (P->count < 1 || W->count != P->count) {
        vm_set_error(vm, "sim_portfolio() needs equal-length paths and weights");
        return;
    }
    int m = P->count, n = -1;
    for (int i = 0; i < m; i++) {
        ObjList *p = interp_list_at_list(P->items[i]);
        if (!p || p->count < 2) {
            vm_set_error(vm, "sim_portfolio() paths must be lists of at least two prices");
            return;
        }
        if (n < 0) n = p->count;
        else if (p->count != n) {
            vm_set_error(vm, "sim_portfolio() paths must be equal length");
            return;
        }
        for (int j = 0; j < n; j++) {
            double v = AS_NUM(p->items[j]);
            if (!isfinite(v) || v <= 0.0) {
                vm_set_error(vm, "sim_portfolio() prices must be finite and positive");
                return;
            }
        }
    }
    for (int i = 0; i < m; i++) {
        double w = AS_NUM(W->items[i]);
        if (!isfinite(w)) {
            vm_set_error(vm, "sim_portfolio() weights must be finite");
            return;
        }
    }
    Obj *res = AS_OBJ(make_list(vm));
    vm_push(vm, val_obj(res));
    for (int t = 1; t < n; t++) {
        double r = 0.0;
        for (int i = 0; i < m; i++) {
            ObjList *p = interp_list_at_list(P->items[i]);
            double prev = AS_NUM(p->items[t - 1]);
            double cur = AS_NUM(p->items[t]);
            r += AS_NUM(W->items[i]) * (cur / prev - 1.0);
        }
        list_push(vm, res, val_num(r));
    }
    vm_pop(vm);
    *out = val_obj(res);
}

/* ---- rolling window portfolio evaluation (2026-11-02, #51) ----
 * Mirrors rt.c lume_bi_rolling_* step for step: every window of the
 * return series gets the #48 metric, the first window-1 slots answer
 * NAN, and zero-spread windows answer NAN (a data property — same as
 * the whole-series metrics). Invalid window / non-finite scalar errors
 * here, empty list natively. */
static void native_rolling_sharpe(VM *vm, int argc, Value *args, Value *out) {
    ObjList *L; double rf, wd;
    if (argc < 3 || !arg_list(vm, args[0], &L) ||
        !arg_num(vm, args[1], &wd) || !arg_num(vm, args[2], &rf)) return;
    int64_t window = (int64_t)wd;
    if (!isfinite(rf)) { vm_set_error(vm, "rolling_sharpe() needs a finite risk-free rate"); return; }
    int n; double *xs = list_to_doubles(vm, L, &n);
    if (!xs) return;
    if (n < 2 || window < 2 || window > n) {
        free(xs);
        vm_set_error(vm, "rolling_sharpe() needs window in [2, len]");
        return;
    }
    Obj *res = AS_OBJ(make_list(vm));
    vm_push(vm, val_obj(res));
    for (int i = 0; i < n; i++) {
        if (i < window - 1) { list_push(vm, res, val_num(NAN)); continue; }
        double m = 0.0, m2 = 0.0;
        int k = 0;
        for (int64_t j = i - window + 1; j <= i; j++, k++) {
            double d = xs[j] - m;
            m += d / (double)(k + 1);
            m2 += d * (xs[j] - m);
        }
        double sd = sqrt(m2 / (double)(window - 1));
        if (!(sd > 0.0) || !isfinite(m)) list_push(vm, res, val_num(NAN));
        else list_push(vm, res, val_num((m - rf) / sd));
    }
    free(xs);
    vm_pop(vm);
    *out = val_obj(res);
}

static void native_rolling_sortino(VM *vm, int argc, Value *args, Value *out) {
    ObjList *L; double rf, wd;
    if (argc < 3 || !arg_list(vm, args[0], &L) ||
        !arg_num(vm, args[1], &wd) || !arg_num(vm, args[2], &rf)) return;
    int64_t window = (int64_t)wd;
    if (!isfinite(rf)) { vm_set_error(vm, "rolling_sortino() needs a finite risk-free rate"); return; }
    int n; double *xs = list_to_doubles(vm, L, &n);
    if (!xs) return;
    if (n < 2 || window < 2 || window > n) {
        free(xs);
        vm_set_error(vm, "rolling_sortino() needs window in [2, len]");
        return;
    }
    Obj *res = AS_OBJ(make_list(vm));
    vm_push(vm, val_obj(res));
    for (int i = 0; i < n; i++) {
        if (i < window - 1) { list_push(vm, res, val_num(NAN)); continue; }
        double m = 0.0, dd = 0.0;
        for (int64_t j = i - window + 1; j <= i; j++) {
            m += xs[j];
            double d = xs[j] - rf;
            if (d < 0.0) dd += d * d;
        }
        m /= (double)window;
        double ds = sqrt(dd / (double)window);
        if (!(ds > 0.0) || !isfinite(m)) list_push(vm, res, val_num(NAN));
        else list_push(vm, res, val_num((m - rf) / ds));
    }
    free(xs);
    vm_pop(vm);
    *out = val_obj(res);
}

static void native_rolling_var(VM *vm, int argc, Value *args, Value *out) {
    ObjList *L; double level, wd;
    if (argc < 3 || !arg_list(vm, args[0], &L) ||
        !arg_num(vm, args[1], &wd) || !arg_num(vm, args[2], &level)) return;
    int64_t window = (int64_t)wd;
    if (!isfinite(level) || level <= 0.0 || level >= 1.0) {
        vm_set_error(vm, "rolling_var() needs level in (0, 1)");
        return;
    }
    int n; double *xs = list_to_doubles(vm, L, &n);
    if (!xs) return;
    if (n < 2 || window < 2 || window > n) {
        free(xs);
        vm_set_error(vm, "rolling_var() needs window in [2, len]");
        return;
    }
    Obj *res = AS_OBJ(make_list(vm));
    vm_push(vm, val_obj(res));
    double *win = (double *)malloc((size_t)window * sizeof(double));
    if (!win) { free(xs); vm_set_error(vm, "rolling_var(): out of memory"); return; }
    for (int i = 0; i < n; i++) {
        if (i < window - 1) { list_push(vm, res, val_num(NAN)); continue; }
        for (int64_t j = 0; j < window; j++) win[j] = xs[i - window + 1 + j];
        qsort(win, (size_t)window, sizeof(double), cmp_d);
        double q = boot_pctl_sorted(win, window, level * 100.0);
        list_push(vm, res, val_num(-q));
    }
    free(win);
    free(xs);
    vm_pop(vm);
    *out = val_obj(res);
}

static void native_rolling_cvar(VM *vm, int argc, Value *args, Value *out) {
    ObjList *L; double level, wd;
    if (argc < 3 || !arg_list(vm, args[0], &L) ||
        !arg_num(vm, args[1], &wd) || !arg_num(vm, args[2], &level)) return;
    int64_t window = (int64_t)wd;
    if (!isfinite(level) || level <= 0.0 || level >= 1.0) {
        vm_set_error(vm, "rolling_cvar() needs level in (0, 1)");
        return;
    }
    int n; double *xs = list_to_doubles(vm, L, &n);
    if (!xs) return;
    if (n < 2 || window < 2 || window > n) {
        free(xs);
        vm_set_error(vm, "rolling_cvar() needs window in [2, len]");
        return;
    }
    Obj *res = AS_OBJ(make_list(vm));
    vm_push(vm, val_obj(res));
    double *win = (double *)malloc((size_t)window * sizeof(double));
    if (!win) { free(xs); vm_set_error(vm, "rolling_cvar(): out of memory"); return; }
    for (int i = 0; i < n; i++) {
        if (i < window - 1) { list_push(vm, res, val_num(NAN)); continue; }
        for (int64_t j = 0; j < window; j++) win[j] = xs[i - window + 1 + j];
        qsort(win, (size_t)window, sizeof(double), cmp_d);
        double q = boot_pctl_sorted(win, window, level * 100.0);
        double s = 0.0; int64_t k = 0;
        while (k < window && win[k] <= q) { s += win[k]; k++; }
        if (k < 1) list_push(vm, res, val_num(NAN));
        else list_push(vm, res, val_num(-(s / (double)k)));
    }
    free(win);
    free(xs);
    vm_pop(vm);
    *out = val_obj(res);
}

/* ---- technical indicators (2026-11-02, #52) — interpreter twins of the
 * rt.c lume_bi_* signal layer. Deterministic (no RNG). */

/* Shared EMA pass: alpha = 2/(n+1), seeded at row 0 (no leading NANs —
 * the same recurrence as ewma). Returns NULL on an invalid list/period. */
static Obj *interp_ema(VM *vm, ObjList *L, int64_t period) {
    if (L->count < 1 || period < 1) return NULL;
    int n; double *xs = list_to_doubles(vm, L, &n);
    if (!xs) return NULL;
    double a = 2.0 / (double)(period + 1);
    Obj *out = AS_OBJ(make_list(vm));
    double e = xs[0];
    list_push(vm, out, val_num(e));
    for (int i = 1; i < n; i++) {
        e = a * xs[i] + (1.0 - a) * e;
        list_push(vm, out, val_num(e));
    }
    free(xs);
    return out;
}

static void native_rsi(VM *vm, int argc, Value *args, Value *out) {
    ObjList *L; double wnd;
    if (argc < 2 || !arg_list(vm, args[0], &L) || !arg_num(vm, args[1], &wnd)) return;
    int64_t window = (int64_t)wnd;
    if (L->count < 2 || window < 1 || window > (int64_t)L->count - 1) {
        vm_set_error(vm, "rsi() needs (prices, window in [1, len-1])");
        return;
    }
    int n; double *xs = list_to_doubles(vm, L, &n);
    if (!xs) return;
    Obj *res = AS_OBJ(make_list(vm));
    double avg_gain = 0.0, avg_loss = 0.0;
    for (int64_t i = 0; i < n; i++) {
        if (i < window) { list_push(vm, res, val_num(NAN)); continue; }
        double ch = xs[i] - xs[i - 1];
        double gain = ch > 0.0 ? ch : 0.0;
        double loss = ch < 0.0 ? -ch : 0.0;
        if (i == window) {
            double sg = 0.0, sl = 0.0;
            for (int64_t j = 1; j <= window; j++) {
                double d = xs[j] - xs[j - 1];
                if (d > 0.0) sg += d; else sl += -d;
            }
            avg_gain = sg / (double)window;
            avg_loss = sl / (double)window;
        } else {
            avg_gain = (avg_gain * (window - 1) + gain) / (double)window;
            avg_loss = (avg_loss * (window - 1) + loss) / (double)window;
        }
        if (!(avg_loss > 0.0)) {
            list_push(vm, res, val_num(avg_gain > 0.0 ? 100.0 : 50.0));
        } else {
            list_push(vm, res, val_num(100.0 - 100.0 / (1.0 + avg_gain / avg_loss)));
        }
    }
    free(xs);
    *out = val_obj(res);
}

static void native_macd(VM *vm, int argc, Value *args, Value *out) {
    ObjList *L; double fd, sd, gd;
    if (argc < 4 || !arg_list(vm, args[0], &L) || !arg_num(vm, args[1], &fd) ||
        !arg_num(vm, args[2], &sd) || !arg_num(vm, args[3], &gd)) return;
    int64_t fast = (int64_t)fd, slow = (int64_t)sd, signal = (int64_t)gd;
    if (L->count < 1 || fast < 1 || slow < 1 || signal < 1) {
        vm_set_error(vm, "macd() needs (prices, fast, slow, signal) with periods >= 1");
        return;
    }
    Obj *ef = interp_ema(vm, L, fast);
    Obj *es = interp_ema(vm, L, slow);
    if (!ef || !es) return;
    int n; double *fx = list_to_doubles(vm, &ef->as.list, &n);
    double *sx = fx ? list_to_doubles(vm, &es->as.list, &n) : NULL;
    if (!sx) { free(fx); return; }
    Obj *macd = AS_OBJ(make_list(vm));
    for (int i = 0; i < n; i++) list_push(vm, macd, val_num(fx[i] - sx[i]));
    free(fx); free(sx);
    Obj *sig = interp_ema(vm, &macd->as.list, signal);
    if (!sig) return;
    int m; double *mx = list_to_doubles(vm, &macd->as.list, &m);
    double *gx = mx ? list_to_doubles(vm, &sig->as.list, &m) : NULL;
    if (!gx) { free(mx); return; }
    Obj *hist = AS_OBJ(make_list(vm));
    for (int i = 0; i < m; i++) list_push(vm, hist, val_num(mx[i] - gx[i]));
    free(mx); free(gx);
    Obj *map = AS_OBJ(make_map(vm));
    vm_push(vm, val_obj(map));
    map_set(vm, map, "macd", val_obj(macd));
    map_set(vm, map, "signal", val_obj(sig));
    map_set(vm, map, "hist", val_obj(hist));
    vm_pop(vm);
    *out = val_obj(map);
}

static void native_bollinger(VM *vm, int argc, Value *args, Value *out) {
    ObjList *L; double wnd, k;
    if (argc < 3 || !arg_list(vm, args[0], &L) || !arg_num(vm, args[1], &wnd) ||
        !arg_num(vm, args[2], &k)) return;
    int64_t window = (int64_t)wnd;
    if (L->count < 2 || window < 2 || window > (int64_t)L->count || !isfinite(k)) {
        vm_set_error(vm, "bollinger() needs (prices, window in [2, len], finite k)");
        return;
    }
    int n; double *xs = list_to_doubles(vm, L, &n);
    if (!xs) return;
    Obj *mid = AS_OBJ(make_list(vm));
    Obj *up = AS_OBJ(make_list(vm));
    Obj *lo = AS_OBJ(make_list(vm));
    for (int64_t i = 0; i < n; i++) {
        if (i < window - 1) {
            list_push(vm, mid, val_num(NAN));
            list_push(vm, up, val_num(NAN));
            list_push(vm, lo, val_num(NAN));
            continue;
        }
        double m = 0.0, m2 = 0.0;
        int64_t kk = 0;
        for (int64_t j = i - window + 1; j <= i; j++, kk++) {
            double d = xs[j] - m;
            m += d / (double)(kk + 1);
            m2 += d * (xs[j] - m);
        }
        double sd = sqrt(m2 / (double)(window - 1));
        list_push(vm, mid, val_num(m));
        list_push(vm, up, val_num(m + k * sd));
        list_push(vm, lo, val_num(m - k * sd));
    }
    free(xs);
    Obj *map = AS_OBJ(make_map(vm));
    vm_push(vm, val_obj(map));
    map_set(vm, map, "mid", val_obj(mid));
    map_set(vm, map, "upper", val_obj(up));
    map_set(vm, map, "lower", val_obj(lo));
    vm_pop(vm);
    *out = val_obj(map);
}

static void native_covariance(VM *vm, int argc, Value *args, Value *out) {
    ObjList *X, *Y;
    if (argc < 2 || !arg_list(vm, args[0], &X) || !arg_list(vm, args[1], &Y)) return;
    if (X->count != Y->count || X->count < 2) {
        vm_set_error(vm, "covariance() needs two equal-length lists (n >= 2)");
        return;
    }
    int n; double *xs = list_to_doubles(vm, X, &n);
    double *ys = xs ? list_to_doubles(vm, Y, &n) : NULL;
    if (!ys) { free(xs); return; }
    /* Joint Welford (West form) — identical recurrence to rt.c
     * lume_bi_covariance so the three legs stay byte-identical. */
    double mx = 0.0, my = 0.0, c = 0.0;
    for (int i = 0; i < n; i++) {
        double dx = xs[i] - mx, dy = ys[i] - my;
        mx += dx / (double)(i + 1);
        my += dy / (double)(i + 1);
        c += dx * (ys[i] - my);
    }
    free(xs); free(ys);
    *out = val_num(c / (double)(n - 1));
}

static void native_correlation(VM *vm, int argc, Value *args, Value *out) {
    ObjList *X, *Y;
    if (argc < 2 || !arg_list(vm, args[0], &X) || !arg_list(vm, args[1], &Y)) return;
    if (X->count != Y->count || X->count < 2) {
        vm_set_error(vm, "correlation() needs two equal-length lists (n >= 2)");
        return;
    }
    int n; double *xs = list_to_doubles(vm, X, &n);
    double *ys = xs ? list_to_doubles(vm, Y, &n) : NULL;
    if (!ys) { free(xs); return; }
    double mx = 0.0, my = 0.0, sxx = 0.0, sxy = 0.0, syy = 0.0;
    for (int i = 0; i < n; i++) {
        double dx = xs[i] - mx, dy = ys[i] - my;
        mx += dx / (double)(i + 1);
        my += dy / (double)(i + 1);
        sxx += dx * (xs[i] - mx);
        sxy += dx * (ys[i] - my);
        syy += dy * (ys[i] - my);
    }
    free(xs); free(ys);
    if (sxx == 0.0 || syy == 0.0) {
        vm_set_error(vm, "correlation(): one series is constant");
        return;
    }
    *out = val_num(sxy / sqrt(sxx * syy));
}

static void native_linreg(VM *vm, int argc, Value *args, Value *out) {
    ObjList *X, *Y;
    if (argc < 2 || !arg_list(vm, args[0], &X) || !arg_list(vm, args[1], &Y)) return;
    if (X->count != Y->count || X->count < 2) {
        vm_set_error(vm, "linreg() needs two equal-length lists (n >= 2)");
        return;
    }
    int n; double *xs = list_to_doubles(vm, X, &n);
    double *ys = xs ? list_to_doubles(vm, Y, &n) : NULL;
    if (!ys) { free(xs); return; }
    /* Joint Welford for sxx/sxy/syy — identical recurrence to rt.c
     * lume_bi_linreg. */
    double mx = 0.0, my = 0.0, sxx = 0.0, sxy = 0.0, syy = 0.0;
    for (int i = 0; i < n; i++) {
        double dx = xs[i] - mx, dy = ys[i] - my;
        mx += dx / (double)(i + 1);
        my += dy / (double)(i + 1);
        sxx += dx * (xs[i] - mx);
        sxy += dx * (ys[i] - my);
        syy += dy * (ys[i] - my);
    }
    free(xs); free(ys);
    if (sxx == 0.0) { vm_set_error(vm, "linreg(): x is constant"); return; }
    double slope = sxy / sxx;
    double intercept = my - slope * mx;
    double r2 = (syy == 0.0) ? (sxy == 0.0 ? 1.0 : NAN) : sxy * sxy / (sxx * syy);
    Obj *m = AS_OBJ(make_map(vm));
    vm_push(vm, val_obj(m));
    map_set(vm, m, "slope", val_num(slope));
    map_set(vm, m, "intercept", val_num(intercept));
    map_set(vm, m, "r2", val_num(r2));
    vm_pop(vm);
    *out = val_obj(m);
}

static void native_pct_change(VM *vm, int argc, Value *args, Value *out) {
    ObjList *L;
    if (argc < 1 || !arg_list(vm, args[0], &L)) return;
    Obj *res = AS_OBJ(make_list(vm));
    vm_push(vm, val_obj(res));
    if (L->count >= 2) {
        int n; double *xs = list_to_doubles(vm, L, &n);
        if (xs) {
            for (int i = 1; i < n; i++) {
                double prev = xs[i - 1];
                double pc = prev == 0.0 ? NAN : 100.0 * (xs[i] - prev) / prev;
                list_push(vm, res, val_num(pc));
            }
            free(xs);
        }
    }
    vm_pop(vm);
    *out = val_obj(res);
}

static void native_max_drawdown(VM *vm, int argc, Value *args, Value *out) {
    ObjList *L;
    if (argc < 1 || !arg_list(vm, args[0], &L)) return;
    if (L->count < 2) { vm_set_error(vm, "max_drawdown() needs >= 2 elements"); return; }
    int n; double *xs = list_to_doubles(vm, L, &n);
    if (!xs) return;
    double peak = xs[0], mdd = 0.0;
    for (int i = 1; i < n; i++) {
        if (xs[i] > peak) peak = xs[i];
        double dd = peak > 0.0 ? 100.0 * (peak - xs[i]) / peak : 0.0;
        if (dd > mdd) mdd = dd;
    }
    free(xs);
    *out = val_num(mdd);
}

/* Performance attribution (#53): alpha_beta answers {alpha, beta} via the
 * same joint-Welford recurrence as rt.c; a zero-variance benchmark answers
 * NAN in both fields (data property, #48 rule) instead of erroring. */
static void native_alpha_beta(VM *vm, int argc, Value *args, Value *out) {
    ObjList *R, *B; double rf;
    if (argc < 3 || !arg_list(vm, args[0], &R) || !arg_list(vm, args[1], &B) ||
        !arg_num(vm, args[2], &rf)) return;
    if (R->count != B->count || R->count < 2) {
        vm_set_error(vm, "alpha_beta() needs two equal-length lists (n >= 2)");
        return;
    }
    if (!isfinite(rf)) { vm_set_error(vm, "alpha_beta(): rf must be finite"); return; }
    int n; double *rs = list_to_doubles(vm, R, &n);
    double *bs = rs ? list_to_doubles(vm, B, &n) : NULL;
    if (!bs) { free(rs); return; }
    double mr = 0.0, mb = 0.0, sxx = 0.0, sxy = 0.0;
    for (int i = 0; i < n; i++) {
        double dr = rs[i] - mr, db = bs[i] - mb;
        mr += dr / (double)(i + 1);
        mb += db / (double)(i + 1);
        sxx += db * (bs[i] - mb);
        sxy += dr * (bs[i] - mb);
    }
    free(rs); free(bs);
    double beta = NAN, alpha = NAN;
    if (sxx != 0.0) {
        beta = sxy / sxx;
        alpha = (mr - rf) - beta * (mb - rf);
    }
    Obj *m = AS_OBJ(make_map(vm));
    vm_push(vm, val_obj(m));
    map_set(vm, m, "alpha", val_num(alpha));
    map_set(vm, m, "beta", val_num(beta));
    vm_pop(vm);
    *out = val_obj(m);
}

static void native_information_ratio(VM *vm, int argc, Value *args, Value *out) {
    ObjList *R, *B;
    if (argc < 2 || !arg_list(vm, args[0], &R) || !arg_list(vm, args[1], &B)) return;
    if (R->count != B->count || R->count < 2) {
        vm_set_error(vm, "information_ratio() needs two equal-length lists (n >= 2)");
        return;
    }
    int n; double *rs = list_to_doubles(vm, R, &n);
    double *bs = rs ? list_to_doubles(vm, B, &n) : NULL;
    if (!bs) { free(rs); return; }
    double md = 0.0, sd = 0.0;
    for (int i = 0; i < n; i++) {
        double d = (rs[i] - bs[i]) - md;
        md += d / (double)(i + 1);
        sd += d * ((rs[i] - bs[i]) - md);
    }
    free(rs); free(bs);
    double std_diff = sqrt(sd / (double)(n - 1));
    *out = val_num(std_diff == 0.0 ? NAN : md / std_diff);
}

static void native_calmar_ratio(VM *vm, int argc, Value *args, Value *out) {
    ObjList *P;
    if (argc < 1 || !arg_list(vm, args[0], &P)) return;
    if (P->count < 2) { vm_set_error(vm, "calmar_ratio() needs >= 2 elements"); return; }
    int n; double *xs = list_to_doubles(vm, P, &n);
    if (!xs) return;
    double m = 0.0;
    for (int i = 1; i < n; i++) {
        if (xs[i - 1] == 0.0) { free(xs); *out = val_num(NAN); return; }
        m += 100.0 * (xs[i] - xs[i - 1]) / xs[i - 1];
    }
    m /= (double)(n - 1);
    double mdd = 0.0, peak = xs[0];
    for (int i = 1; i < n; i++) {
        if (xs[i] > peak) peak = xs[i];
        double dd = peak > 0.0 ? 100.0 * (peak - xs[i]) / peak : 0.0;
        if (dd > mdd) mdd = dd;
    }
    free(xs);
    *out = val_num(mdd == 0.0 ? NAN : m / mdd);
}

/* Time series & ratios (#54): ACF(lag) over the aligned segment, the
 * Treynor ratio (reuses the alpha_beta joint-Welford recurrence) and
 * the full-distribution Omega ratio — identical contracts to rt.c. */
static void native_autocorrelation(VM *vm, int argc, Value *args, Value *out) {
    ObjList *L; double lagd;
    if (argc < 2 || !arg_list(vm, args[0], &L) || !arg_num(vm, args[1], &lagd)) return;
    int64_t lag = (int64_t)lagd;
    if (L->count < 3 || lag < 1 || lag > L->count - 2) {
        vm_set_error(vm, "autocorrelation() needs lag in [1, n-2] with n >= 3");
        return;
    }
    int n; double *xs = list_to_doubles(vm, L, &n);
    if (!xs) return;
    int64_t m = n - lag;
    double m1 = 0.0, m2 = 0.0, sxx = 0.0, sxy = 0.0;
    for (int64_t i = 0; i < m; i++) {
        double a = xs[i], b = xs[i + lag];
        double da = a - m1, db = b - m2;
        m1 += da / (double)(i + 1);
        m2 += db / (double)(i + 1);
        sxx += da * (a - m1);
        sxy += da * (b - m2);
    }
    free(xs);
    *out = val_num(sxx == 0.0 ? NAN : sxy / sxx);
}

static void native_treynor_ratio(VM *vm, int argc, Value *args, Value *out) {
    ObjList *R, *B; double rf;
    if (argc < 3 || !arg_list(vm, args[0], &R) || !arg_list(vm, args[1], &B) ||
        !arg_num(vm, args[2], &rf)) return;
    if (R->count != B->count || R->count < 2) {
        vm_set_error(vm, "treynor_ratio() needs two equal-length lists (n >= 2)");
        return;
    }
    if (!isfinite(rf)) { vm_set_error(vm, "treynor_ratio(): rf must be finite"); return; }
    int n; double *rs = list_to_doubles(vm, R, &n);
    double *bs = rs ? list_to_doubles(vm, B, &n) : NULL;
    if (!bs) { free(rs); return; }
    double mr = 0.0, mb = 0.0, sxx = 0.0, sxy = 0.0;
    for (int i = 0; i < n; i++) {
        double dr = rs[i] - mr, db = bs[i] - mb;
        mr += dr / (double)(i + 1);
        mb += db / (double)(i + 1);
        sxx += db * (bs[i] - mb);
        sxy += dr * (bs[i] - mb);
    }
    free(rs); free(bs);
    if (sxx == 0.0) { *out = val_num(NAN); return; }
    double beta = sxy / sxx;
    *out = val_num(beta == 0.0 ? NAN : (mr - rf) / beta);
}

static void native_omega_ratio(VM *vm, int argc, Value *args, Value *out) {
    ObjList *L; double t;
    if (argc < 2 || !arg_list(vm, args[0], &L) || !arg_num(vm, args[1], &t)) return;
    if (L->count < 2) { vm_set_error(vm, "omega_ratio() needs >= 2 elements"); return; }
    if (!isfinite(t)) { vm_set_error(vm, "omega_ratio(): threshold must be finite"); return; }
    int n; double *xs = list_to_doubles(vm, L, &n);
    if (!xs) return;
    double g = 0.0, lo = 0.0;
    for (int i = 0; i < n; i++) {
        double d = xs[i] - t;
        if (d > 0.0) g += d; else lo += -d;
    }
    free(xs);
    *out = val_num(lo == 0.0 ? NAN : g / lo);
}

/* Rank statistics & signal evaluation (#55): average ranks (1-based,
 * ties share the mean), Spearman on the rank pairs (joint-Welford
 * Pearson, mirroring rt.c) and the information coefficient over the
 * aligned signal/forward-return segment. */
static void native_rank(VM *vm, int argc, Value *args, Value *out) {
    ObjList *L;
    if (argc < 1 || !arg_list(vm, args[0], &L)) return;
    if (L->count < 1) { vm_set_error(vm, "rank() needs >= 1 element"); return; }
    int n; double *xs = list_to_doubles(vm, L, &n);
    if (!xs) return;
    double *sorted = malloc((size_t)n * sizeof(double));
    if (!sorted) { free(xs); return; }
    memcpy(sorted, xs, (size_t)n * sizeof(double));
    qsort(sorted, (size_t)n, sizeof(double), cmp_d);
    Obj *res = AS_OBJ(make_list(vm));
    vm_push(vm, val_obj(res));
    for (int i = 0; i < n; i++) {
        int lo = 0, hi = n - 1, first = -1, last = -1;
        while (lo <= hi) {
            int mid = lo + (hi - lo) / 2;
            if (sorted[mid] < xs[i]) lo = mid + 1;
            else { first = mid; hi = mid - 1; }
        }
        lo = 0; hi = n - 1;
        while (lo <= hi) {
            int mid = lo + (hi - lo) / 2;
            if (sorted[mid] > xs[i]) hi = mid - 1;
            else { last = mid; lo = mid + 1; }
        }
        list_push(vm, res, val_num(((double)(first + 1) + (double)(last + 1)) / 2.0));
    }
    free(sorted); free(xs);
    vm_pop(vm);
    *out = val_obj(res);
}

static double interp_spearman_pairs(const double *xs, const double *ys, int n)
{
    double *sx = malloc((size_t)n * sizeof(double));
    double *sy = malloc((size_t)n * sizeof(double));
    double *ox = malloc((size_t)n * sizeof(double));
    double *oy = malloc((size_t)n * sizeof(double));
    if (!sx || !sy || !ox || !oy) {
        free(sx); free(sy); free(ox); free(oy); return NAN;
    }
    for (int i = 0; i < n; i++) { sx[i] = xs[i]; sy[i] = ys[i]; }
    qsort(sx, (size_t)n, sizeof(double), cmp_d);
    qsort(sy, (size_t)n, sizeof(double), cmp_d);
    for (int i = 0; i < n; i++) {
        int lo = 0, hi = n - 1, first = -1, last = -1;
        while (lo <= hi) {
            int mid = lo + (hi - lo) / 2;
            if (sx[mid] < xs[i]) lo = mid + 1;
            else { first = mid; hi = mid - 1; }
        }
        lo = 0; hi = n - 1;
        while (lo <= hi) {
            int mid = lo + (hi - lo) / 2;
            if (sx[mid] > xs[i]) hi = mid - 1;
            else { last = mid; lo = mid + 1; }
        }
        ox[i] = ((double)(first + 1) + (double)(last + 1)) / 2.0;
        lo = 0; hi = n - 1; first = -1; last = -1;
        while (lo <= hi) {
            int mid = lo + (hi - lo) / 2;
            if (sy[mid] < ys[i]) lo = mid + 1;
            else { first = mid; hi = mid - 1; }
        }
        lo = 0; hi = n - 1;
        while (lo <= hi) {
            int mid = lo + (hi - lo) / 2;
            if (sy[mid] > ys[i]) hi = mid - 1;
            else { last = mid; lo = mid + 1; }
        }
        oy[i] = ((double)(first + 1) + (double)(last + 1)) / 2.0;
    }
    free(sx); free(sy);
    double mx = 0.0, my = 0.0, sxx = 0.0, sxy = 0.0, syy = 0.0;
    for (int i = 0; i < n; i++) {
        double dx = ox[i] - mx, dy = oy[i] - my;
        mx += dx / (double)(i + 1);
        my += dy / (double)(i + 1);
        sxx += dx * (ox[i] - mx);
        sxy += dx * (oy[i] - my);
        syy += dy * (oy[i] - my);
    }
    free(ox); free(oy);
    if (sxx == 0.0 || syy == 0.0) return NAN;
    return sxy / sqrt(sxx * syy);
}

static void native_spearman(VM *vm, int argc, Value *args, Value *out) {
    ObjList *X, *Y;
    if (argc < 2 || !arg_list(vm, args[0], &X) || !arg_list(vm, args[1], &Y)) return;
    if (X->count != Y->count || X->count < 2) {
        vm_set_error(vm, "spearman() needs two equal-length lists (n >= 2)");
        return;
    }
    int n; double *xs = list_to_doubles(vm, X, &n);
    double *ys = xs ? list_to_doubles(vm, Y, &n) : NULL;
    if (!ys) { free(xs); return; }
    double r = interp_spearman_pairs(xs, ys, n);
    free(xs); free(ys);
    *out = val_num(r);
}

static void native_ic(VM *vm, int argc, Value *args, Value *out) {
    ObjList *S, *R; double lagd;
    if (argc < 3 || !arg_list(vm, args[0], &S) || !arg_list(vm, args[1], &R) ||
        !arg_num(vm, args[2], &lagd)) return;
    int64_t lag = (int64_t)lagd;
    if (S->count != R->count || S->count < 3 || lag < 1 || lag > S->count - 2) {
        vm_set_error(vm, "ic() needs lag in [1, n-2] with n >= 3");
        return;
    }
    int n; double *ss = list_to_doubles(vm, S, &n);
    double *rs = ss ? list_to_doubles(vm, R, &n) : NULL;
    if (!rs) { free(ss); return; }
    int64_t m = n - lag;
    double *sa = malloc((size_t)m * sizeof(double));
    double *ra = malloc((size_t)m * sizeof(double));
    if (!sa || !ra) { free(ss); free(rs); free(sa); free(ra); return; }
    for (int64_t i = 0; i < m; i++) { sa[i] = ss[i]; ra[i] = rs[i + lag]; }
    free(ss); free(rs);
    double r = interp_spearman_pairs(sa, ra, (int)m);
    free(sa); free(ra);
    *out = val_num(r);
}

/* Annualization & returns (#56): the daily->yearly sqrt(252) scale and
 * the geometric/compound price forms, mirroring rt.c. */
#define BI_ANNUAL_SCALE_I 15.874507866387544

static void native_annualized_return(VM *vm, int argc, Value *args, Value *out) {
    ObjList *L;
    if (argc < 1 || !arg_list(vm, args[0], &L)) return;
    if (L->count < 2) { vm_set_error(vm, "annualized_return() needs >= 2 prices"); return; }
    int n; double *xs = list_to_doubles(vm, L, &n);
    if (!xs) return;
    for (int i = 0; i < n; i++) {
        if (!(xs[i] > 0.0)) { free(xs); *out = val_num(NAN); return; }
    }
    double r = pow(xs[n - 1] / xs[0], 1.0 / (double)(n - 1)) - 1.0;
    free(xs);
    *out = val_num(r);
}

static void native_annualized_volatility(VM *vm, int argc, Value *args, Value *out) {
    ObjList *L;
    if (argc < 1 || !arg_list(vm, args[0], &L)) return;
    if (L->count < 2) { vm_set_error(vm, "annualized_volatility() needs >= 2 returns"); return; }
    int n; double *xs = list_to_doubles(vm, L, &n);
    if (!xs) return;
    double m = 0.0, m2 = 0.0;
    for (int i = 0; i < n; i++) {
        double d = xs[i] - m;
        m += d / (double)(i + 1);
        m2 += d * (xs[i] - m);
    }
    free(xs);
    *out = val_num(sqrt(m2 / (double)(n - 1)) * BI_ANNUAL_SCALE_I);
}

static void native_sharpe_annualized(VM *vm, int argc, Value *args, Value *out) {
    ObjList *L; double rf;
    if (argc < 2 || !arg_list(vm, args[0], &L) || !arg_num(vm, args[1], &rf)) return;
    if (L->count < 2) { vm_set_error(vm, "sharpe_annualized() needs >= 2 returns"); return; }
    if (!isfinite(rf)) { vm_set_error(vm, "sharpe_annualized(): rf must be finite"); return; }
    int n; double *xs = list_to_doubles(vm, L, &n);
    if (!xs) return;
    double m = 0.0, m2 = 0.0;
    for (int i = 0; i < n; i++) {
        double d = xs[i] - m;
        m += d / (double)(i + 1);
        m2 += d * (xs[i] - m);
    }
    free(xs);
    double sd = sqrt(m2 / (double)(n - 1));
    if (!(sd > 0.0) || !isfinite(m)) { *out = val_num(NAN); return; }
    *out = val_num(((m - rf) / sd) * BI_ANNUAL_SCALE_I);
}

static void native_cumulative_return(VM *vm, int argc, Value *args, Value *out) {
    ObjList *L;
    if (argc < 1 || !arg_list(vm, args[0], &L)) return;
    if (L->count < 2) { vm_set_error(vm, "cumulative_return() needs >= 2 prices"); return; }
    int n; double *xs = list_to_doubles(vm, L, &n);
    if (!xs) return;
    if (!(xs[0] > 0.0) || !(xs[n - 1] > 0.0)) {
        free(xs); *out = val_num(NAN); return;
    }
    double r = xs[n - 1] / xs[0] - 1.0;
    free(xs);
    *out = val_num(r);
}

/* Volatility & risk derivatives (#57): mirror rt.c — mean-based
 * lower-part moments, the normal-model VaR and the Kelly fraction. */
static void native_semivariance(VM *vm, int argc, Value *args, Value *out) {
    ObjList *L;
    if (argc < 1 || !arg_list(vm, args[0], &L)) return;
    if (L->count < 2) { vm_set_error(vm, "semivariance() needs >= 2 returns"); return; }
    int n; double *xs = list_to_doubles(vm, L, &n);
    if (!xs) return;
    double m = 0.0;
    for (int i = 0; i < n; i++) {
        double d = xs[i] - m;
        m += d / (double)(i + 1);
    }
    double sv = 0.0;
    for (int i = 0; i < n; i++) {
        double d = xs[i] - m;
        if (d < 0.0) sv += d * d;
    }
    free(xs);
    *out = val_num(sv / (double)n);
}

static void native_downside_deviation(VM *vm, int argc, Value *args, Value *out) {
    ObjList *L; double t;
    if (argc < 2 || !arg_list(vm, args[0], &L) || !arg_num(vm, args[1], &t)) return;
    if (L->count < 2) { vm_set_error(vm, "downside_deviation() needs >= 2 returns"); return; }
    if (!isfinite(t)) { vm_set_error(vm, "downside_deviation(): threshold must be finite"); return; }
    int n; double *xs = list_to_doubles(vm, L, &n);
    if (!xs) return;
    double dd = 0.0;
    for (int i = 0; i < n; i++) {
        double d = xs[i] - t;
        if (d < 0.0) dd += d * d;
    }
    free(xs);
    *out = val_num(sqrt(dd / (double)n));
}

static void native_param_var(VM *vm, int argc, Value *args, Value *out) {
    ObjList *L; double level;
    if (argc < 2 || !arg_list(vm, args[0], &L) || !arg_num(vm, args[1], &level)) return;
    if (L->count < 2) { vm_set_error(vm, "param_var() needs >= 2 returns"); return; }
    if (!(level > 0.0) || !(level < 1.0)) {
        vm_set_error(vm, "param_var(): level must be in (0, 1)"); return;
    }
    int n; double *xs = list_to_doubles(vm, L, &n);
    if (!xs) return;
    double m = 0.0, m2 = 0.0;
    for (int i = 0; i < n; i++) {
        double d = xs[i] - m;
        m += d / (double)(i + 1);
        m2 += d * (xs[i] - m);
    }
    free(xs);
    double sd = sqrt(m2 / (double)(n - 1));
    double z = bi_norminv(level);
    *out = val_num(-(m + z * sd));
}

static void native_kelly_fraction(VM *vm, int argc, Value *args, Value *out) {
    ObjList *L;
    if (argc < 1 || !arg_list(vm, args[0], &L)) return;
    if (L->count < 2) { vm_set_error(vm, "kelly_fraction() needs >= 2 returns"); return; }
    int n; double *xs = list_to_doubles(vm, L, &n);
    if (!xs) return;
    double m = 0.0, m2 = 0.0;
    for (int i = 0; i < n; i++) {
        double d = xs[i] - m;
        m += d / (double)(i + 1);
        m2 += d * (xs[i] - m);
    }
    free(xs);
    double var = m2 / (double)(n - 1);
    if (!(var > 0.0)) { *out = val_num(NAN); return; }
    *out = val_num(m / var);
}

/* Trading statistics (#58): mirror rt.c — backtest-quality layer with
 * the same zero-loss-side NAN / zero-streak 0 data properties. */
static void native_profit_factor(VM *vm, int argc, Value *args, Value *out) {
    ObjList *L;
    if (argc < 1 || !arg_list(vm, args[0], &L)) return;
    if (L->count < 2) { vm_set_error(vm, "profit_factor() needs >= 2 returns"); return; }
    int n; double *xs = list_to_doubles(vm, L, &n);
    if (!xs) return;
    double w = 0.0, lo = 0.0;
    for (int i = 0; i < n; i++) {
        if (xs[i] > 0.0) w += xs[i]; else lo += -xs[i];
    }
    free(xs);
    *out = val_num(lo == 0.0 ? NAN : w / lo);
}

static void native_win_rate(VM *vm, int argc, Value *args, Value *out) {
    ObjList *L;
    if (argc < 1 || !arg_list(vm, args[0], &L)) return;
    if (L->count < 2) { vm_set_error(vm, "win_rate() needs >= 2 returns"); return; }
    int n; double *xs = list_to_doubles(vm, L, &n);
    if (!xs) return;
    int64_t w = 0;
    for (int i = 0; i < n; i++) {
        if (xs[i] > 0.0) w++;
    }
    free(xs);
    *out = val_num((double)w / (double)n);
}

static void native_avg_win(VM *vm, int argc, Value *args, Value *out) {
    ObjList *L;
    if (argc < 1 || !arg_list(vm, args[0], &L)) return;
    if (L->count < 2) { vm_set_error(vm, "avg_win() needs >= 2 returns"); return; }
    int n; double *xs = list_to_doubles(vm, L, &n);
    if (!xs) return;
    double w = 0.0; int64_t c = 0;
    for (int i = 0; i < n; i++) {
        if (xs[i] > 0.0) { w += xs[i]; c++; }
    }
    free(xs);
    *out = val_num(c == 0 ? NAN : w / (double)c);
}

static void native_avg_loss(VM *vm, int argc, Value *args, Value *out) {
    ObjList *L;
    if (argc < 1 || !arg_list(vm, args[0], &L)) return;
    if (L->count < 2) { vm_set_error(vm, "avg_loss() needs >= 2 returns"); return; }
    int n; double *xs = list_to_doubles(vm, L, &n);
    if (!xs) return;
    double lo = 0.0; int64_t c = 0;
    for (int i = 0; i < n; i++) {
        if (xs[i] < 0.0) { lo += xs[i]; c++; }
    }
    free(xs);
    *out = val_num(c == 0 ? NAN : lo / (double)c);
}

static void native_max_consecutive_losses(VM *vm, int argc, Value *args, Value *out) {
    ObjList *L;
    if (argc < 1 || !arg_list(vm, args[0], &L)) return;
    if (L->count < 2) { vm_set_error(vm, "max_consecutive_losses() needs >= 2 returns"); return; }
    int n; double *xs = list_to_doubles(vm, L, &n);
    if (!xs) return;
    int64_t run = 0, best = 0;
    for (int i = 0; i < n; i++) {
        if (xs[i] < 0.0) { run++; if (run > best) best = run; }
        else run = 0;
    }
    free(xs);
    *out = val_num((double)best);
}

static void native_skew(VM *vm, int argc, Value *args, Value *out) {
    ObjList *L;
    if (argc < 1 || !arg_list(vm, args[0], &L)) return;
    if (L->count < 3) { vm_set_error(vm, "skew() needs >= 3 elements"); return; }
    int n; double *xs = list_to_doubles(vm, L, &n);
    if (!xs) return;
    double m = 0.0;
    for (int i = 0; i < n; i++) m += xs[i];
    m /= (double)n;
    double t1 = 0.0, t2 = 0.0;
    for (int i = 0; i < n; i++) {
        double d = xs[i] - m;
        t1 += d * d * d;
        t2 += d * d;
    }
    free(xs);
    if (t2 == 0.0) { vm_set_error(vm, "skew(): series is constant"); return; }
    double g = t1 / (t2 * sqrt(t2));
    *out = val_num(g * (double)n / ((double)(n - 1) * (double)(n - 2)));
}

static void native_kurtosis(VM *vm, int argc, Value *args, Value *out) {
    ObjList *L;
    if (argc < 1 || !arg_list(vm, args[0], &L)) return;
    if (L->count < 4) { vm_set_error(vm, "kurtosis() needs >= 4 elements"); return; }
    int n; double *xs = list_to_doubles(vm, L, &n);
    if (!xs) return;
    double m = 0.0;
    for (int i = 0; i < n; i++) m += xs[i];
    m /= (double)n;
    double t1 = 0.0, t2 = 0.0;
    for (int i = 0; i < n; i++) {
        double d = xs[i] - m;
        double d2 = d * d;
        t1 += d2 * d2;
        t2 += d2;
    }
    free(xs);
    if (t2 == 0.0) { vm_set_error(vm, "kurtosis(): series is constant"); return; }
    double k = (double)n * ((double)n + 1.0) * t1
               / ((double)(n - 1) * (double)(n - 2) * (double)(n - 3) * t2 * t2)
               - 3.0 * (double)(n - 1) * (double)(n - 1)
                   / ((double)(n - 2) * (double)(n - 3));
    *out = val_num(k);
}

static void native_rolling_mean(VM *vm, int argc, Value *args, Value *out) {
    ObjList *L; double wd;
    if (argc < 2 || !arg_list(vm, args[0], &L) || !arg_num(vm, args[1], &wd)) return;
    int64_t w = (int64_t)wd;
    if (w < 1) { vm_set_error(vm, "rolling_mean() needs a positive window"); return; }
    Obj *res = AS_OBJ(make_list(vm));
    vm_push(vm, val_obj(res));
    int n; double *xs = list_to_doubles(vm, L, &n);
    if (xs) {
        double s = 0.0;
        for (int i = 0; i < n; i++) {
            s += xs[i];
            if (i >= w) s -= xs[i - w];
            list_push(vm, res, val_num(i + 1 >= w ? s / (double)w : NAN));
        }
        free(xs);
    }
    vm_pop(vm);
    *out = val_obj(res);
}

static void native_rolling_stddev(VM *vm, int argc, Value *args, Value *out) {
    ObjList *L; double wd;
    if (argc < 2 || !arg_list(vm, args[0], &L) || !arg_num(vm, args[1], &wd)) return;
    int64_t w = (int64_t)wd;
    if (w < 1) { vm_set_error(vm, "rolling_stddev() needs a positive window"); return; }
    Obj *res = AS_OBJ(make_list(vm));
    vm_push(vm, val_obj(res));
    int n; double *xs = list_to_doubles(vm, L, &n);
    if (xs) {
        for (int i = 0; i < n; i++) {
            if (i + 1 < w || w < 2) { list_push(vm, res, val_num(NAN)); continue; }
            int64_t lo = (int64_t)i + 1 - w;
            double m = 0.0;
            for (int64_t j = lo; j <= i; j++) m += xs[j];
            m /= (double)w;
            double s = 0.0;
            for (int64_t j = lo; j <= i; j++) { double d = xs[j] - m; s += d * d; }
            list_push(vm, res, val_num(sqrt(s / (double)(w - 1))));
        }
        free(xs);
    }
    vm_pop(vm);
    *out = val_obj(res);
}

static void native_ewma(VM *vm, int argc, Value *args, Value *out) {
    ObjList *L; double alpha;
    if (argc < 2 || !arg_list(vm, args[0], &L) || !arg_num(vm, args[1], &alpha)) return;
    if (alpha <= 0.0 || alpha > 1.0) {
        vm_set_error(vm, "ewma() needs alpha in (0,1]");
        return;
    }
    Obj *res = AS_OBJ(make_list(vm));
    vm_push(vm, val_obj(res));
    int n; double *xs = list_to_doubles(vm, L, &n);
    if (xs) {
        if (n > 0) {
            double e = xs[0];
            list_push(vm, res, val_num(e));
            for (int i = 1; i < n; i++) {
                e = alpha * xs[i] + (1.0 - alpha) * e;
                list_push(vm, res, val_num(e));
            }
        }
        free(xs);
    }
    vm_pop(vm);
    *out = val_obj(res);
}

Value b_mean(VM *vm, int argc, Value *args)         { return vm_native(vm, argc, args, native_mean); }
Value b_variance(VM *vm, int argc, Value *args)     { return vm_native(vm, argc, args, native_variance); }
Value b_stddev(VM *vm, int argc, Value *args)       { return vm_native(vm, argc, args, native_stddev); }
Value b_median(VM *vm, int argc, Value *args)       { return vm_native(vm, argc, args, native_median); }
Value b_percentile(VM *vm, int argc, Value *args)   { return vm_native(vm, argc, args, native_percentile); }
Value b_covariance(VM *vm, int argc, Value *args)   { return vm_native(vm, argc, args, native_covariance); }
Value b_correlation(VM *vm, int argc, Value *args)  { return vm_native(vm, argc, args, native_correlation); }
Value b_linreg(VM *vm, int argc, Value *args)       { return vm_native(vm, argc, args, native_linreg); }
Value b_pct_change(VM *vm, int argc, Value *args)   { return vm_native(vm, argc, args, native_pct_change); }
Value b_max_drawdown(VM *vm, int argc, Value *args) { return vm_native(vm, argc, args, native_max_drawdown); }
Value b_skew(VM *vm, int argc, Value *args)         { return vm_native(vm, argc, args, native_skew); }
Value b_kurtosis(VM *vm, int argc, Value *args)     { return vm_native(vm, argc, args, native_kurtosis); }
Value b_rolling_mean(VM *vm, int argc, Value *args) { return vm_native(vm, argc, args, native_rolling_mean); }
Value b_rolling_stddev(VM *vm, int argc, Value *args) { return vm_native(vm, argc, args, native_rolling_stddev); }
Value b_ewma(VM *vm, int argc, Value *args)         { return vm_native(vm, argc, args, native_ewma); }
Value b_bootstrap_mean_ci(VM *vm, int argc, Value *args)   { return vm_native(vm, argc, args, native_bootstrap_mean_ci); }
Value b_bootstrap_median_ci(VM *vm, int argc, Value *args) { return vm_native(vm, argc, args, native_bootstrap_median_ci); }
Value b_bootstrap_stddev_ci(VM *vm, int argc, Value *args) { return vm_native(vm, argc, args, native_bootstrap_stddev_ci); }
Value b_bootstrap_se_mean(VM *vm, int argc, Value *args)   { return vm_native(vm, argc, args, native_bootstrap_se_mean); }
Value b_value_at_risk(VM *vm, int argc, Value *args)       { return vm_native(vm, argc, args, native_value_at_risk); }
Value b_expected_shortfall(VM *vm, int argc, Value *args)  { return vm_native(vm, argc, args, native_expected_shortfall); }
Value b_sharpe_ratio(VM *vm, int argc, Value *args)        { return vm_native(vm, argc, args, native_sharpe_ratio); }
Value b_sortino_ratio(VM *vm, int argc, Value *args)       { return vm_native(vm, argc, args, native_sortino_ratio); }
Value b_rolling_sharpe(VM *vm, int argc, Value *args)     { return vm_native(vm, argc, args, native_rolling_sharpe); }
Value b_rolling_sortino(VM *vm, int argc, Value *args)    { return vm_native(vm, argc, args, native_rolling_sortino); }
Value b_rolling_var(VM *vm, int argc, Value *args)        { return vm_native(vm, argc, args, native_rolling_var); }
Value b_rolling_cvar(VM *vm, int argc, Value *args)       { return vm_native(vm, argc, args, native_rolling_cvar); }
Value b_sim_portfolio(VM *vm, int argc, Value *args)       { return vm_native(vm, argc, args, native_sim_portfolio); }
Value b_rsi(VM *vm, int argc, Value *args)                { return vm_native(vm, argc, args, native_rsi); }
Value b_macd(VM *vm, int argc, Value *args)               { return vm_native(vm, argc, args, native_macd); }
Value b_bollinger(VM *vm, int argc, Value *args)          { return vm_native(vm, argc, args, native_bollinger); }
Value b_alpha_beta(VM *vm, int argc, Value *args)         { return vm_native(vm, argc, args, native_alpha_beta); }
Value b_information_ratio(VM *vm, int argc, Value *args)  { return vm_native(vm, argc, args, native_information_ratio); }
Value b_calmar_ratio(VM *vm, int argc, Value *args)       { return vm_native(vm, argc, args, native_calmar_ratio); }
Value b_autocorrelation(VM *vm, int argc, Value *args)    { return vm_native(vm, argc, args, native_autocorrelation); }
Value b_treynor_ratio(VM *vm, int argc, Value *args)      { return vm_native(vm, argc, args, native_treynor_ratio); }
Value b_omega_ratio(VM *vm, int argc, Value *args)        { return vm_native(vm, argc, args, native_omega_ratio); }
Value b_rank(VM *vm, int argc, Value *args)               { return vm_native(vm, argc, args, native_rank); }
Value b_spearman(VM *vm, int argc, Value *args)           { return vm_native(vm, argc, args, native_spearman); }
Value b_ic(VM *vm, int argc, Value *args)                 { return vm_native(vm, argc, args, native_ic); }
Value b_annualized_return(VM *vm, int argc, Value *args)  { return vm_native(vm, argc, args, native_annualized_return); }
Value b_annualized_volatility(VM *vm, int argc, Value *args) { return vm_native(vm, argc, args, native_annualized_volatility); }
Value b_sharpe_annualized(VM *vm, int argc, Value *args) { return vm_native(vm, argc, args, native_sharpe_annualized); }
Value b_cumulative_return(VM *vm, int argc, Value *args) { return vm_native(vm, argc, args, native_cumulative_return); }
Value b_semivariance(VM *vm, int argc, Value *args)       { return vm_native(vm, argc, args, native_semivariance); }
Value b_downside_deviation(VM *vm, int argc, Value *args) { return vm_native(vm, argc, args, native_downside_deviation); }
Value b_param_var(VM *vm, int argc, Value *args)          { return vm_native(vm, argc, args, native_param_var); }
Value b_kelly_fraction(VM *vm, int argc, Value *args)     { return vm_native(vm, argc, args, native_kelly_fraction); }
Value b_profit_factor(VM *vm, int argc, Value *args)      { return vm_native(vm, argc, args, native_profit_factor); }
Value b_win_rate(VM *vm, int argc, Value *args)           { return vm_native(vm, argc, args, native_win_rate); }
Value b_avg_win(VM *vm, int argc, Value *args)            { return vm_native(vm, argc, args, native_avg_win); }
Value b_avg_loss(VM *vm, int argc, Value *args)           { return vm_native(vm, argc, args, native_avg_loss); }
Value b_max_consecutive_losses(VM *vm, int argc, Value *args) { return vm_native(vm, argc, args, native_max_consecutive_losses); }

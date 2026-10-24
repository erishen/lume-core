/* Native math builtins for Lume — 数值内建片 (2026-09-28).
 *
 * 给 DSL 层补基础数学能力：abs/sqrt/exp/log/ln/pow/floor/ceil/round/
 * min/max；常量 e、pi 由 interp.c bridge_seed_builtins() 直接 env_set。
 * 此前语言层只有四则运算，算法演示(lib/algo.lume)无法计算 sigmoid /
 * 距离 / softmax；本文件补齐后用标准 math.h 实现，精度交给 C double。
 *
 * 约定（与其他 builtins_*.c 一致）：
 *   - native_* 为本片实现（static，仅 b_* 包装使用）；
 *   - b_* 包装注册进 interp.c 的注册表；
 *   - 参数错误 / 域错误一律 vm_set_error（不写 out，返回 null）。
 */

#include "builtins_internal.h"
#include <math.h>

/* 取一个数值参数；非数字 → error。 */
static bool arg_num(VM *vm, Value v, double *out) {
    if (!IS_NUM(v)) {
        vm_set_error(vm, "expected number argument");
        return false;
    }
    *out = AS_NUM(v);
    return true;
}

static void native_abs(VM *vm, int argc, Value *args, Value *out) {
    double x;
    if (argc < 1 || !arg_num(vm, args[0], &x)) return;
    *out = val_num(fabs(x));
}

static void native_sqrt(VM *vm, int argc, Value *args, Value *out) {
    double x;
    if (argc < 1 || !arg_num(vm, args[0], &x)) return;
    if (x < 0.0) {
        vm_set_error(vm, "sqrt(): negative argument");
        return;
    }
    *out = val_num(sqrt(x));
}

static void native_exp(VM *vm, int argc, Value *args, Value *out) {
    double x;
    if (argc < 1 || !arg_num(vm, args[0], &x)) return;
    *out = val_num(exp(x));
}

static void native_log(VM *vm, int argc, Value *args, Value *out) {
    double x;
    if (argc < 1 || !arg_num(vm, args[0], &x)) return;
    if (x <= 0.0) {
        vm_set_error(vm, "log(): argument must be positive");
        return;
    }
    *out = val_num(log(x));
}

static void native_pow(VM *vm, int argc, Value *args, Value *out) {
    double x, y;
    if (argc < 2 || !arg_num(vm, args[0], &x) || !arg_num(vm, args[1], &y)) return;
    double r = pow(x, y);
    if (!isfinite(r)) {
        vm_set_error(vm, "pow(): result not finite (domain error)");
        return;
    }
    *out = val_num(r);
}

static void native_floor(VM *vm, int argc, Value *args, Value *out) {
    double x;
    if (argc < 1 || !arg_num(vm, args[0], &x)) return;
    *out = val_num(floor(x));
}

static void native_ceil(VM *vm, int argc, Value *args, Value *out) {
    double x;
    if (argc < 1 || !arg_num(vm, args[0], &x)) return;
    *out = val_num(ceil(x));
}

static void native_round(VM *vm, int argc, Value *args, Value *out) {
    double x;
    if (argc < 1 || !arg_num(vm, args[0], &x)) return;
    *out = val_num(round(x));
}

/* min/max 支持可变参数（≥1 个数值），逐个比较。 */
static void native_min(VM *vm, int argc, Value *args, Value *out) {
    if (argc < 1) {
        vm_set_error(vm, "min() expects at least one number");
        return;
    }
    double best = 0.0;
    for (int i = 0; i < argc; i++) {
        double x;
        if (!arg_num(vm, args[i], &x)) return;
        if (i == 0 || x < best) best = x;
    }
    *out = val_num(best);
}

static void native_max(VM *vm, int argc, Value *args, Value *out) {
    if (argc < 1) {
        vm_set_error(vm, "max() expects at least one number");
        return;
    }
    double best = 0.0;
    for (int i = 0; i < argc; i++) {
        double x;
        if (!arg_num(vm, args[i], &x)) return;
        if (i == 0 || x > best) best = x;
    }
    *out = val_num(best);
}

Value b_abs(VM *vm, int argc, Value *args)   { return vm_native(vm, argc, args, native_abs); }
Value b_sqrt(VM *vm, int argc, Value *args)  { return vm_native(vm, argc, args, native_sqrt); }
Value b_exp(VM *vm, int argc, Value *args)   { return vm_native(vm, argc, args, native_exp); }
Value b_log(VM *vm, int argc, Value *args)   { return vm_native(vm, argc, args, native_log); }
Value b_ln(VM *vm, int argc, Value *args)    { return vm_native(vm, argc, args, native_log); }
Value b_pow(VM *vm, int argc, Value *args)   { return vm_native(vm, argc, args, native_pow); }
Value b_floor(VM *vm, int argc, Value *args) { return vm_native(vm, argc, args, native_floor); }
Value b_ceil(VM *vm, int argc, Value *args)  { return vm_native(vm, argc, args, native_ceil); }
Value b_round(VM *vm, int argc, Value *args) { return vm_native(vm, argc, args, native_round); }
Value b_min(VM *vm, int argc, Value *args)   { return vm_native(vm, argc, args, native_min); }
Value b_max(VM *vm, int argc, Value *args)   { return vm_native(vm, argc, args, native_max); }

/* ------------------------------------------------------------------ *
 * Statistical distribution builtins (P4, 2026-10-20).
 *
 * The numerical core is duplicated from rt.c (the native backends' twin,
 * which --compile links separately) exactly like abs/sqrt/... already are:
 * this file must link into the interpreter binary, which does not carry
 * rt.c. Keep the series / continued fractions byte-for-byte in step with
 * rt.c or the three backends drift; the wrappers here add argument
 * validation and turn the NaN domain signal into a vm_set_error.
 * ------------------------------------------------------------------ */

static double igamma_series(double a, double x)
{
    double lnf = -x + a * log(x) - lgamma(a);
    if (lnf < -745.0) return 0.0;
    double term = 1.0 / a, sum = term;
    for (int n = 1; n < 2000; n++) {
        term *= x / (a + n);
        sum += term;
        if (term < sum * 1e-15) break;
    }
    return exp(lnf) * sum;
}

static double igamma_cf(double a, double x)
{
    double lnf = -x + a * log(x) - lgamma(a);
    if (lnf < -745.0) return 0.0;
    double b = x + 1.0 - a;
    double c = 1.0e30, d = 1.0 / b, h = d;   /* c ~ 1/FPMIN (Lentz) */
    for (int i = 1; i < 2000; i++) {
        double an = -(double)i * (i - a);
        b += 2.0;
        d = an * d + b;
        if (fabs(d) < 1e-30) d = 1e-30;
        c = b + an / c;
        if (fabs(c) < 1e-30) c = 1e-30;
        d = 1.0 / d;
        double del = d * c;
        h *= del;
        if (fabs(del - 1.0) < 1e-15) break;
    }
    return exp(lnf) * h;
}

static double ibeta_cf(double a, double b, double x)
{
    double qab = a + b, qap = a + 1.0, qam = a - 1.0;
    double c = 1.0, d = 1.0 - qab * x / qap;
    if (fabs(d) < 1e-30) d = 1e-30;
    d = 1.0 / d;
    double h = d;
    for (int m = 1; m <= 2000; m++) {
        int m2 = 2 * m;
        double aa = (double)m * (b - m) * x / ((qam + m2) * (a + m2));
        d = 1.0 + aa * d;
        if (fabs(d) < 1e-30) d = 1e-30;
        c = 1.0 + aa / c;
        if (fabs(c) < 1e-30) c = 1e-30;
        d = 1.0 / d;
        h *= d * c;
        aa = -(a + m) * (qab + m) * x / ((a + m2) * (qap + m2));
        d = 1.0 + aa * d;
        if (fabs(d) < 1e-30) d = 1e-30;
        c = 1.0 + aa / c;
        if (fabs(c) < 1e-30) c = 1e-30;
        d = 1.0 / d;
        double del = d * c;
        h *= del;
        if (fabs(del - 1.0) < 1e-15) break;
    }
    return h;
}

static double bi_erf(double x)     { return erf(x); }
static double bi_normcdf(double x) { return 0.5 * (1.0 + erf(x * 0.70710678118654752440)); }
static double bi_norminv(double p)
{
    /* Acklam's rational approximation (2011) — identical constants and
     * branches to rt.c's lume_bi_norminv, so the three backends stay
     * byte-identical. */
    static const double a[6] = { -3.969683028665376e+01, 2.209460984245205e+02,
        -2.759285104469687e+02, 1.383577518672690e+02, -3.066479806614716e+01,
        2.506628277459239e+00 };
    static const double b[5] = { -5.447609879822406e+01, 1.615858368580409e+02,
        -1.556989798598866e+02, 6.680131188771972e+01, -1.328068155288572e+01 };
    static const double c[6] = { -7.784894002430293e-03, -3.223964580411365e-01,
        -2.400758277161838e+00, -2.549732539343734e+00, 4.374664141464968e+00,
        2.938163982698783e+00 };
    static const double d[4] = { 7.784695709041462e-03, 3.224671290700398e-01,
        2.445134137142996e+00, 3.754408661907416e+00 };
    static const double plow = 0.02425, phigh = 0.97575;
    double q, r;

    if (p <= 0.0 || p >= 1.0) return NAN;
    if (p < plow) {
        q = sqrt(-2.0 * log(p));
        return (((((c[0] * q + c[1]) * q + c[2]) * q + c[3]) * q + c[4]) * q + c[5]) /
               ((((d[0] * q + d[1]) * q + d[2]) * q + d[3]) * q + 1.0);
    }
    if (p > phigh) {
        q = sqrt(-2.0 * log(1.0 - p));
        return -(((((c[0] * q + c[1]) * q + c[2]) * q + c[3]) * q + c[4]) * q + c[5]) /
                ((((d[0] * q + d[1]) * q + d[2]) * q + d[3]) * q + 1.0);
    }
    q = p - 0.5;
    r = q * q;
    return (((((a[0] * r + a[1]) * r + a[2]) * r + a[3]) * r + a[4]) * r + a[5]) * q /
           (((((b[0] * r + b[1]) * r + b[2]) * r + b[3]) * r + b[4]) * r + 1.0);
}
static double bi_lgamma(double x)  { return lgamma(x); }
static double bi_gamma(double x)   { return exp(lgamma(x)); }

static double bi_igamma(double a, double x)
{
    if (a <= 0.0 || x < 0.0 || !isfinite(a)) return NAN;
    if (x == 0.0) return 0.0;
    if (!isfinite(x)) return 1.0;
    return x < a + 1.0 ? igamma_series(a, x) : 1.0 - igamma_cf(a, x);
}

static double bi_ibeta(double a, double b, double x)
{
    if (a <= 0.0 || b <= 0.0 || x < 0.0 || x > 1.0) return NAN;
    if (x == 0.0) return 0.0;
    if (x == 1.0) return 1.0;
    double lnbt = lgamma(a) + lgamma(b) - lgamma(a + b);
    double lfront = a * log(x) + b * log1p(-x) - lnbt;
    if (x < (a + 1.0) / (a + b + 2.0)) {
        if (lfront < -745.0) return 0.0;
        return exp(lfront) * ibeta_cf(a, b, x) / a;
    }
    double lr = b * log(1.0 - x) + a * log(x) - lnbt;
    if (lr < -745.0) return 1.0;
    return 1.0 - exp(lr) * ibeta_cf(b, a, 1.0 - x) / b;
}

static double bi_tdist_cdf(double t, double df)
{
    if (df <= 0.0 || !isfinite(df)) return NAN;
    if (!isfinite(t)) return t < 0.0 ? 0.0 : 1.0;
    double x = df / (df + t * t);
    double ib = bi_ibeta(df / 2.0, 0.5, x);
    return t >= 0.0 ? 1.0 - 0.5 * ib : 0.5 * ib;
}

static double bi_chisq_cdf(double x, double df)
{
    if (df <= 0.0 || !isfinite(df)) return NAN;
    if (x <= 0.0) return 0.0;
    if (!isfinite(x)) return 1.0;
    return bi_igamma(df / 2.0, x / 2.0);
}

static void native_erf(VM *vm, int argc, Value *args, Value *out) {
    double x;
    if (argc < 1 || !arg_num(vm, args[0], &x)) return;
    *out = val_num(bi_erf(x));
}

static void native_normcdf(VM *vm, int argc, Value *args, Value *out) {
    double x;
    if (argc < 1 || !arg_num(vm, args[0], &x)) return;
    *out = val_num(bi_normcdf(x));
}

static void native_norminv(VM *vm, int argc, Value *args, Value *out) {
    double p;
    if (argc < 1 || !arg_num(vm, args[0], &p)) return;
    if (p <= 0.0 || p >= 1.0) {
        vm_set_error(vm, "norminv(): p must be in (0,1)");
        return;
    }
    *out = val_num(bi_norminv(p));
}

static void native_gamma(VM *vm, int argc, Value *args, Value *out) {
    double x;
    if (argc < 1 || !arg_num(vm, args[0], &x)) return;
    if (x <= 0.0) {
        vm_set_error(vm, "gamma(): argument must be positive");
        return;
    }
    *out = val_num(bi_gamma(x));
}

static void native_lgamma(VM *vm, int argc, Value *args, Value *out) {
    double x;
    if (argc < 1 || !arg_num(vm, args[0], &x)) return;
    if (x <= 0.0) {
        vm_set_error(vm, "lgamma(): argument must be positive");
        return;
    }
    *out = val_num(bi_lgamma(x));
}

static void native_igamma(VM *vm, int argc, Value *args, Value *out) {
    double a, x;
    if (argc < 2) { vm_set_error(vm, "igamma() expects two numbers"); return; }
    if (!arg_num(vm, args[0], &a) || !arg_num(vm, args[1], &x)) return;
    double r = bi_igamma(a, x);
    if (isnan(r)) { vm_set_error(vm, "igamma(): bad arguments (a > 0, x >= 0)"); return; }
    *out = val_num(r);
}

static void native_ibeta(VM *vm, int argc, Value *args, Value *out) {
    double a, b, x;
    if (argc < 3) { vm_set_error(vm, "ibeta() expects three numbers"); return; }
    if (!arg_num(vm, args[0], &a) || !arg_num(vm, args[1], &b) ||
        !arg_num(vm, args[2], &x)) return;
    double r = bi_ibeta(a, b, x);
    if (isnan(r)) {
        vm_set_error(vm, "ibeta(): bad arguments (a > 0, b > 0, 0 <= x <= 1)");
        return;
    }
    *out = val_num(r);
}

static void native_tdist_cdf(VM *vm, int argc, Value *args, Value *out) {
    double t, df;
    if (argc < 2) { vm_set_error(vm, "tdist_cdf() expects two numbers"); return; }
    if (!arg_num(vm, args[0], &t) || !arg_num(vm, args[1], &df)) return;
    double r = bi_tdist_cdf(t, df);
    if (isnan(r)) { vm_set_error(vm, "tdist_cdf(): bad arguments (df > 0)"); return; }
    *out = val_num(r);
}

static void native_chisq_cdf(VM *vm, int argc, Value *args, Value *out) {
    double x, df;
    if (argc < 2) { vm_set_error(vm, "chisq_cdf() expects two numbers"); return; }
    if (!arg_num(vm, args[0], &x) || !arg_num(vm, args[1], &df)) return;
    double r = bi_chisq_cdf(x, df);
    if (isnan(r)) { vm_set_error(vm, "chisq_cdf(): bad arguments (df > 0)"); return; }
    *out = val_num(r);
}

Value b_normcdf(VM *vm, int argc, Value *args)   { return vm_native(vm, argc, args, native_normcdf); }
Value b_norminv(VM *vm, int argc, Value *args)   { return vm_native(vm, argc, args, native_norminv); }
Value b_erf(VM *vm, int argc, Value *args)       { return vm_native(vm, argc, args, native_erf); }
Value b_gamma(VM *vm, int argc, Value *args)     { return vm_native(vm, argc, args, native_gamma); }
Value b_lgamma(VM *vm, int argc, Value *args)    { return vm_native(vm, argc, args, native_lgamma); }
Value b_igamma(VM *vm, int argc, Value *args)    { return vm_native(vm, argc, args, native_igamma); }
Value b_ibeta(VM *vm, int argc, Value *args)     { return vm_native(vm, argc, args, native_ibeta); }
Value b_tdist_cdf(VM *vm, int argc, Value *args) { return vm_native(vm, argc, args, native_tdist_cdf); }
Value b_chisq_cdf(VM *vm, int argc, Value *args) { return vm_native(vm, argc, args, native_chisq_cdf); }

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

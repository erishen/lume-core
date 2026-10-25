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

/* Monte Carlo RNG (2026-10-24): deterministic PCG32 mirroring rt.c's
 * lume_bi_* exactly (same constants, same consume order), so interpreter
 * and native backends stay byte-identical. seed(n) re-bases the stream. */
static uint64_t bi_rng_state = 0x853c49e6748fea9bULL;
static uint64_t bi_rng_inc   = 0xda3e39cb94b95bdbULL;

/* native_shuffle is defined after the list helpers (list_to_doubles etc.)
 * that it needs; the b_* wrappers sit earlier, so forward-declare it. The
 * advanced samplers (2026-10-28) — native_rand_multinomial in particular —
 * are also defined before those list helpers, so forward-declare them too. */
static void native_shuffle(VM *vm, int argc, Value *args, Value *out);
static bool arg_list(VM *vm, Value v, ObjList **out);
static double *list_to_doubles(VM *vm, ObjList *L, int *n);

static uint32_t bi_rng_next(void)
{
    uint64_t old = bi_rng_state;
    bi_rng_state = old * 6364136223846793005ULL + bi_rng_inc;
    uint32_t xs = (uint32_t)(((old >> 18u) ^ old) >> 27u);
    uint32_t rot = (uint32_t)(old >> 59u);
    return (xs >> rot) | (xs << ((32u - rot) & 31u));
}

static void native_seed(VM *vm, int argc, Value *args, Value *out) {
    double s;
    if (argc < 1 || !arg_num(vm, args[0], &s)) return;
    bi_rng_state = (uint64_t)(int64_t)s;
    bi_rng_inc   = 0xda3e39cb94b95bdbULL;
    *out = val_num((double)(int64_t)s);
}

static void native_rand(VM *vm, int argc, Value *args, Value *out) {
    (void)args;
    if (argc != 0) { vm_set_error(vm, "rand() takes no arguments"); return; }
    *out = val_num((double)(bi_rng_next() >> 8) * 0.000000059604644775390625);
}

static void native_rand_range(VM *vm, int argc, Value *args, Value *out) {
    double lo, hi;
    if (argc != 2 || !arg_num(vm, args[0], &lo) || !arg_num(vm, args[1], &hi)) return;
    if (!isfinite(lo) || !isfinite(hi) || hi < lo) {
        vm_set_error(vm, "rand_range(): need lo <= hi, finite");
        return;
    }
    *out = val_num(lo + (hi - lo) * ((double)(bi_rng_next() >> 8) * 0.000000059604644775390625));
}

static void native_rand_int(VM *vm, int argc, Value *args, Value *out) {
    double lo, hi;
    if (argc != 2 || !arg_num(vm, args[0], &lo) || !arg_num(vm, args[1], &hi)) return;
    if (hi < lo) {
        vm_set_error(vm, "rand_int(): need lo <= hi");
        return;
    }
    int64_t a = (int64_t)lo, b = (int64_t)hi;
    uint64_t span = (uint64_t)(b - a) + 1ULL;
    uint64_t r = (uint64_t)bi_rng_next() | ((uint64_t)bi_rng_next() << 32u);
    *out = val_num((double)(a + (int64_t)(r % span)));
}

static void native_rand_normal(VM *vm, int argc, Value *args, Value *out) {
    double mu, sigma;
    if (argc != 2 || !arg_num(vm, args[0], &mu) || !arg_num(vm, args[1], &sigma)) return;
    double u1 = (double)(bi_rng_next() >> 8) * 0.000000059604644775390625
                + 0.0000000298023223876953125;
    double u2 = (double)(bi_rng_next() >> 8) * 0.000000059604644775390625
                + 0.0000000298023223876953125;
    double z = sqrt(-2.0 * log(u1)) * cos(6.28318530717958647693 * u2);
    *out = val_num(mu + sigma * z);
}

static void native_rand_bool(VM *vm, int argc, Value *args, Value *out) {
    double p;
    if (argc < 1 || !arg_num(vm, args[0], &p)) return;
    if (!isfinite(p) || p < 0.0 || p > 1.0) {
        vm_set_error(vm, "rand_bool(): need p in [0,1]");
        return;
    }
    *out = val_num((double)(((double)(bi_rng_next() >> 8) * 0.000000059604644775390625) < p));
}

static void native_rand_exponential(VM *vm, int argc, Value *args, Value *out) {
    double rate;
    if (argc < 1 || !arg_num(vm, args[0], &rate)) return;
    if (!isfinite(rate) || rate <= 0.0) {
        vm_set_error(vm, "rand_exponential(): need rate > 0");
        return;
    }
    double u = (double)(bi_rng_next() >> 8) * 0.000000059604644775390625
               + 0.0000000298023223876953125;
    *out = val_num(-log(u) / rate);
}

static void native_rand_poisson(VM *vm, int argc, Value *args, Value *out) {
    double lambda;
    if (argc < 1 || !arg_num(vm, args[0], &lambda)) return;
    if (!isfinite(lambda) || lambda <= 0.0) {
        vm_set_error(vm, "rand_poisson(): need lambda > 0");
        return;
    }
    double l = exp(-lambda);
    if (!(l > 0.0)) {
        vm_set_error(vm, "rand_poisson(): lambda too large (exp underflows)");
        return;
    }
    double p = 1.0;
    int64_t k = 0;
    do {
        k++;
        double u = (double)(bi_rng_next() >> 8) * 0.000000059604644775390625
                   + 0.0000000298023223876953125;
        p *= u;
    } while (p > l);
    *out = val_num((double)(k - 1));
}

/* Continuous samplers (2026-10-25): gamma / beta / chisq / student_t /
 * cauchy mirroring rt.c's lume_bi_* exactly (same constants, same consume
 * order) — interpreter and native backends stay byte-identical. */
static double bi_gamma_sample(double shape)
{
    if (shape < 1.0)
        return bi_gamma_sample(shape + 1.0)
               * pow((double)(bi_rng_next() >> 8) * 0.000000059604644775390625,
                     1.0 / shape);
    double d = shape - 0.3333333333333333;
    double c = 1.0 / sqrt(9.0 * d);
    for (;;) {
        double u1 = (double)(bi_rng_next() >> 8) * 0.000000059604644775390625
                    + 0.0000000298023223876953125;
        double u2 = (double)(bi_rng_next() >> 8) * 0.000000059604644775390625
                    + 0.0000000298023223876953125;
        double z = sqrt(-2.0 * log(u1)) * cos(6.28318530717958647693 * u2);
        double v = (1.0 + c * z);
        v = v * v * v;
        if (v <= 0.0) continue;
        double u = (double)(bi_rng_next() >> 8) * 0.000000059604644775390625;
        if (u < 1.0 - 0.0331 * z * z * z * z) return d * v;
        if (log(u) < 0.5 * z * z + d * (1.0 - v + log(v))) return d * v;
    }
}

static void native_rand_cauchy(VM *vm, int argc, Value *args, Value *out) {
    double loc, scale;
    if (argc < 2 || !arg_num(vm, args[0], &loc) || !arg_num(vm, args[1], &scale)) return;
    if (!isfinite(loc) || !isfinite(scale) || scale <= 0.0) {
        vm_set_error(vm, "rand_cauchy(): need scale > 0");
        return;
    }
    double u = (double)(bi_rng_next() >> 8) * 0.000000059604644775390625;
    *out = val_num(loc + scale * tan(3.14159265358979323846 * (u - 0.5)));
}

static void native_rand_gamma(VM *vm, int argc, Value *args, Value *out) {
    double shape, rate;
    if (argc < 2 || !arg_num(vm, args[0], &shape) || !arg_num(vm, args[1], &rate)) return;
    if (!isfinite(shape) || !isfinite(rate) || shape <= 0.0 || rate <= 0.0) {
        vm_set_error(vm, "rand_gamma(): need shape > 0, rate > 0");
        return;
    }
    *out = val_num(bi_gamma_sample(shape) / rate);
}

static void native_rand_beta(VM *vm, int argc, Value *args, Value *out) {
    double a, b;
    if (argc < 2 || !arg_num(vm, args[0], &a) || !arg_num(vm, args[1], &b)) return;
    if (!isfinite(a) || !isfinite(b) || a <= 0.0 || b <= 0.0) {
        vm_set_error(vm, "rand_beta(): need a > 0, b > 0");
        return;
    }
    double ga = bi_gamma_sample(a);
    double gb = bi_gamma_sample(b);
    *out = val_num(ga / (ga + gb));
}

static void native_rand_chisq(VM *vm, int argc, Value *args, Value *out) {
    double k;
    if (argc < 1 || !arg_num(vm, args[0], &k)) return;
    if (!isfinite(k) || k <= 0.0) {
        vm_set_error(vm, "rand_chisq(): need k > 0");
        return;
    }
    *out = val_num(2.0 * bi_gamma_sample(0.5 * k));
}

static void native_rand_student_t(VM *vm, int argc, Value *args, Value *out) {
    double df;
    if (argc < 1 || !arg_num(vm, args[0], &df)) return;
    if (!isfinite(df) || df <= 0.0) {
        vm_set_error(vm, "rand_student_t(): need df > 0");
        return;
    }
    double u1 = (double)(bi_rng_next() >> 8) * 0.000000059604644775390625
                + 0.0000000298023223876953125;
    double u2 = (double)(bi_rng_next() >> 8) * 0.000000059604644775390625
                + 0.0000000298023223876953125;
    double z = sqrt(-2.0 * log(u1)) * cos(6.28318530717958647693 * u2);
    double v = 2.0 * bi_gamma_sample(0.5 * df);
    *out = val_num(z / sqrt(v / df));
}

/* Inverse-transform samplers (2026-10-26): geometric / weibull / logistic
 * / pareto / laplace mirroring rt.c's lume_bi_* exactly (same constants,
 * same one-draw consume order). All use the 2^-25 anti-zero bias; invalid
 * parameters error (native side answers -1 / NAN). */
static void native_rand_geometric(VM *vm, int argc, Value *args, Value *out) {
    double p;
    if (argc < 1 || !arg_num(vm, args[0], &p)) return;
    if (!isfinite(p) || p <= 0.0 || p > 1.0) {
        vm_set_error(vm, "rand_geometric(): need p in (0, 1]");
        return;
    }
    double ub = (double)(bi_rng_next() >> 8) * 0.000000059604644775390625
                + 0.0000000298023223876953125;
    *out = val_num((double)(int64_t)floor(log(ub) / log(1.0 - p)));
}

static void native_rand_weibull(VM *vm, int argc, Value *args, Value *out) {
    double shape, scale;
    if (argc < 2 || !arg_num(vm, args[0], &shape) || !arg_num(vm, args[1], &scale)) return;
    if (!isfinite(shape) || !isfinite(scale) || shape <= 0.0 || scale <= 0.0) {
        vm_set_error(vm, "rand_weibull(): need shape > 0, scale > 0");
        return;
    }
    double ub = (double)(bi_rng_next() >> 8) * 0.000000059604644775390625
                + 0.0000000298023223876953125;
    *out = val_num(scale * pow(-log(ub), 1.0 / shape));
}

static void native_rand_logistic(VM *vm, int argc, Value *args, Value *out) {
    double mu, s;
    if (argc < 2 || !arg_num(vm, args[0], &mu) || !arg_num(vm, args[1], &s)) return;
    if (!isfinite(mu) || !isfinite(s) || s <= 0.0) {
        vm_set_error(vm, "rand_logistic(): need scale s > 0");
        return;
    }
    double ub = (double)(bi_rng_next() >> 8) * 0.000000059604644775390625
                + 0.0000000298023223876953125;
    *out = val_num(mu + s * log(ub / (1.0 - ub)));
}

static void native_rand_pareto(VM *vm, int argc, Value *args, Value *out) {
    double xm, alpha;
    if (argc < 2 || !arg_num(vm, args[0], &xm) || !arg_num(vm, args[1], &alpha)) return;
    if (!isfinite(xm) || !isfinite(alpha) || xm <= 0.0 || alpha <= 0.0) {
        vm_set_error(vm, "rand_pareto(): need xm > 0, alpha > 0");
        return;
    }
    double ub = (double)(bi_rng_next() >> 8) * 0.000000059604644775390625
                + 0.0000000298023223876953125;
    *out = val_num(xm / pow(ub, 1.0 / alpha));
}

static void native_rand_laplace(VM *vm, int argc, Value *args, Value *out) {
    double mu, b;
    if (argc < 2 || !arg_num(vm, args[0], &mu) || !arg_num(vm, args[1], &b)) return;
    if (!isfinite(mu) || !isfinite(b) || b <= 0.0) {
        vm_set_error(vm, "rand_laplace(): need scale b > 0");
        return;
    }
    double ub = (double)(bi_rng_next() >> 8) * 0.000000059604644775390625
                + 0.0000000298023223876953125;
    double s = (ub < 0.5) ? -1.0 : 1.0;
    *out = val_num(mu - b * s * log(1.0 - 2.0 * fabs(ub - 0.5)));
}

/* Mixed samplers (2026-10-27): binomial / lognormal / gumbel / rayleigh /
 * triangular mirroring rt.c's lume_bi_* exactly (same constants, same
 * consume order). rand_binomial takes an integer n and a probability p;
 * invalid parameters error (native side answers -1 / NAN). */
static void native_rand_binomial(VM *vm, int argc, Value *args, Value *out) {
    double n, p;
    if (argc < 2 || !arg_num(vm, args[0], &n) || !arg_num(vm, args[1], &p)) return;
    if (n < 0.0 || !isfinite(p) || p < 0.0 || p > 1.0) {
        vm_set_error(vm, "rand_binomial(): need n >= 0, p in [0, 1]");
        return;
    }
    int64_t nn = (int64_t)n;
    int64_t k = 0;
    for (int64_t i = 0; i < nn; i++) {
        double u = (double)(bi_rng_next() >> 8) * 0.000000059604644775390625;
        if (u < p) k++;
    }
    *out = val_num((double)k);
}

static void native_rand_lognormal(VM *vm, int argc, Value *args, Value *out) {
    double mu, sigma;
    if (argc < 2 || !arg_num(vm, args[0], &mu) || !arg_num(vm, args[1], &sigma)) return;
    if (!isfinite(mu) || !isfinite(sigma) || sigma <= 0.0) {
        vm_set_error(vm, "rand_lognormal(): need sigma > 0");
        return;
    }
    double u1 = (double)(bi_rng_next() >> 8) * 0.000000059604644775390625
                + 0.0000000298023223876953125;
    double u2 = (double)(bi_rng_next() >> 8) * 0.000000059604644775390625
                + 0.0000000298023223876953125;
    double z = sqrt(-2.0 * log(u1)) * cos(6.28318530717958647693 * u2);
    *out = val_num(exp(mu + sigma * z));
}

static void native_rand_gumbel(VM *vm, int argc, Value *args, Value *out) {
    double mu, beta;
    if (argc < 2 || !arg_num(vm, args[0], &mu) || !arg_num(vm, args[1], &beta)) return;
    if (!isfinite(mu) || !isfinite(beta) || beta <= 0.0) {
        vm_set_error(vm, "rand_gumbel(): need scale beta > 0");
        return;
    }
    double ub = (double)(bi_rng_next() >> 8) * 0.000000059604644775390625
                + 0.0000000298023223876953125;
    *out = val_num(mu - beta * log(-log(ub)));
}

static void native_rand_rayleigh(VM *vm, int argc, Value *args, Value *out) {
    double scale;
    if (argc < 1 || !arg_num(vm, args[0], &scale)) return;
    if (!isfinite(scale) || scale <= 0.0) {
        vm_set_error(vm, "rand_rayleigh(): need scale > 0");
        return;
    }
    double ub = (double)(bi_rng_next() >> 8) * 0.000000059604644775390625
                + 0.0000000298023223876953125;
    *out = val_num(scale * sqrt(-2.0 * log(ub)));
}

static void native_rand_triangular(VM *vm, int argc, Value *args, Value *out) {
    double lo, hi;
    if (argc < 2 || !arg_num(vm, args[0], &lo) || !arg_num(vm, args[1], &hi)) return;
    if (!isfinite(lo) || !isfinite(hi) || hi <= lo) {
        vm_set_error(vm, "rand_triangular(): need hi > lo");
        return;
    }
    double u = (double)(bi_rng_next() >> 8) * 0.000000059604644775390625;
    double span = hi - lo;
    if (u < 0.5)
        *out = val_num(lo + sqrt(0.5 * u) * span);
    else
        *out = val_num(hi - sqrt(0.5 * (1.0 - u)) * span);
}

/* Advanced samplers (2026-10-28): zipf / negative-binomial / erlang /
 * von Mises / multinomial mirroring rt.c's lume_bi_* exactly (same
 * constants, same consume order). rand_multinomial answers a fresh int
 * list; the rest are scalars (zipf / negative-binomial TY_INT, erlang /
 * von Mises TY_FLOAT). Invalid parameters error on the interpreter
 * (native side answers -1 / NAN / an empty list). */
static void native_rand_zipf(VM *vm, int argc, Value *args, Value *out) {
    double n, s;
    if (argc < 2 || !arg_num(vm, args[0], &n) || !arg_num(vm, args[1], &s)) return;
    if (n < 1.0 || !isfinite(s) || s <= 1.0) {
        vm_set_error(vm, "rand_zipf(): need n >= 1, s > 1");
        return;
    }
    int64_t nn = (int64_t)n;
    double total = 0.0;
    for (int64_t i = 1; i <= nn; i++) total += pow((double)i, -s);
    double t = (double)(bi_rng_next() >> 8) * 0.000000059604644775390625 * total;
    double acc = 0.0;
    int64_t ans = nn;
    for (int64_t i = 1; i <= nn; i++) {
        acc += pow((double)i, -s);
        if (t < acc) { ans = i; break; }
    }
    *out = val_num((double)ans);
}

static void native_rand_negative_binomial(VM *vm, int argc, Value *args, Value *out) {
    double r, p;
    if (argc < 2 || !arg_num(vm, args[0], &r) || !arg_num(vm, args[1], &p)) return;
    if (r < 1.0 || !isfinite(p) || p <= 0.0 || p >= 1.0) {
        vm_set_error(vm, "rand_negative_binomial(): need r >= 1, p in (0, 1)");
        return;
    }
    int64_t rr = (int64_t)r;
    int64_t k = 0;
    double lq = log(1.0 - p);
    for (int64_t i = 0; i < rr; i++) {
        double ub = (double)(bi_rng_next() >> 8) * 0.000000059604644775390625
                    + 0.0000000298023223876953125;
        k += (int64_t)(log(ub) / lq);
    }
    *out = val_num((double)k);
}

static void native_rand_erlang(VM *vm, int argc, Value *args, Value *out) {
    double k, rate;
    if (argc < 2 || !arg_num(vm, args[0], &k) || !arg_num(vm, args[1], &rate)) return;
    if (k < 1.0 || !isfinite(rate) || rate <= 0.0) {
        vm_set_error(vm, "rand_erlang(): need k >= 1, rate > 0");
        return;
    }
    int64_t kk = (int64_t)k;
    double s = 0.0;
    for (int64_t i = 0; i < kk; i++) {
        double ub = (double)(bi_rng_next() >> 8) * 0.000000059604644775390625
                    + 0.0000000298023223876953125;
        s += -log(ub) / rate;
    }
    *out = val_num(s);
}

static void native_rand_von_mises(VM *vm, int argc, Value *args, Value *out) {
    double mu, kappa;
    if (argc < 2 || !arg_num(vm, args[0], &mu) || !arg_num(vm, args[1], &kappa)) return;
    if (!isfinite(mu) || !isfinite(kappa) || kappa < 0.0) {
        vm_set_error(vm, "rand_von_mises(): need kappa >= 0");
        return;
    }
    if (kappa == 0.0) {
        double u = (double)(bi_rng_next() >> 8) * 0.000000059604644775390625;
        *out = val_num(mu + (u - 0.5) * 6.28318530717958647693);
        return;
    }
    double tau = 1.0 + sqrt(1.0 + 4.0 * kappa * kappa);
    double rho = (tau - sqrt(2.0 * tau)) / (2.0 * kappa);
    if (rho == 0.0) {
        double u = (double)(bi_rng_next() >> 8) * 0.000000059604644775390625;
        *out = val_num(mu + (u - 0.5) * 6.28318530717958647693);
        return;
    }
    double r = (1.0 + rho * rho) / (2.0 * rho);
    for (;;) {
        double u1 = (double)(bi_rng_next() >> 8) * 0.000000059604644775390625;
        double u2 = (double)(bi_rng_next() >> 8) * 0.000000059604644775390625
                    + 0.0000000298023223876953125;
        double z = cos(3.14159265358979323846 * u1);
        double f = (1.0 + r * z) / (r + z);
        double c = kappa * (r - f);
        if (c * (2.0 - c) - u2 > 0.0 || log(c / u2) + 1.0 - c >= 0.0) {
            *out = val_num(mu + (u1 < 0.5 ? -1.0 : 1.0) * acos(f));
            return;
        }
    }
}

static void native_rand_multinomial(VM *vm, int argc, Value *args, Value *out) {
    double n;
    ObjList *L;
    if (argc < 2 || !arg_num(vm, args[0], &n) || !arg_list(vm, args[1], &L)) return;
    if (n < 0.0 || n != n) { vm_set_error(vm, "rand_multinomial(): need n >= 0"); return; }
    int64_t nn = (int64_t)n;
    int m; double *w = list_to_doubles(vm, L, &m);
    if (!w) return;
    if (m < 1) { free(w); vm_set_error(vm, "rand_multinomial(): need a non-empty probs list"); return; }
    double total = 0.0;
    for (int i = 0; i < m; i++) {
        if (w[i] < 0.0 || w[i] != w[i]) { free(w); vm_set_error(vm, "rand_multinomial(): probs must be non-negative"); return; }
        total += w[i];
    }
    if (!(total > 0.0)) { free(w); vm_set_error(vm, "rand_multinomial(): probs must sum to > 0"); return; }
    int64_t *cnt = (int64_t *)malloc((size_t)m * sizeof(int64_t));
    if (!cnt) { free(w); vm_set_error(vm, "out of memory"); return; }
    for (int i = 0; i < m; i++) cnt[i] = 0;
    for (int64_t j = 0; j < nn; j++) {
        double t = (double)(bi_rng_next() >> 8) * 0.000000059604644775390625 * total;
        double acc = 0.0;
        for (int i = 0; i < m; i++) {
            acc += w[i];
            if (t < acc) { cnt[i]++; break; }
        }
    }
    free(w);
    Obj *res = AS_OBJ(make_list(vm));
    vm_push(vm, val_obj(res));
    for (int i = 0; i < m; i++) list_push(vm, res, val_int(cnt[i]));
    free(cnt);
    vm_pop(vm);
    *out = val_obj(res);
}

/* Financial path simulators + Black-Scholes (2026-10-28): independent
 * interpreter-side implementations of the same formulas as rt.c's
 * lume_bi_sim_* / lume_bi_bs_*, driven by this VM's own bi_rng stream, so
 * each backend advances its own PCG32 state identically. Invalid
 * parameters answer an empty list / NAN here too, matching the native
 * side. A path draws through the two-draw Box-Muller pair per point and —
 * for Merton — the Knuth Poisson loop, whose draw count is stream-fixed. */
static double bi_rand_normal_1(double mu, double sigma)
{
    double u1 = (double)(bi_rng_next() >> 8) * 0.000000059604644775390625
                + 0.0000000298023223876953125;
    double u2 = (double)(bi_rng_next() >> 8) * 0.000000059604644775390625
                + 0.0000000298023223876953125;
    double z = sqrt(-2.0 * log(u1)) * cos(6.28318530717958647693 * u2);
    return mu + sigma * z;
}

static int64_t bi_poisson_knuth(double lambda)
{
    if (!isfinite(lambda) || lambda <= 0.0) return -1;
    double l = exp(-lambda);
    if (!(l > 0.0)) return -1;
    double p = 1.0;
    int64_t k = 0;
    do {
        k++;
        double u = (double)(bi_rng_next() >> 8) * 0.000000059604644775390625
                   + 0.0000000298023223876953125;
        p *= u;
    } while (p > l);
    return k - 1;
}

static void native_sim_gbm(VM *vm, int argc, Value *args, Value *out) {
    double S0, mu, sigma, T, nv;
    if (argc != 5 || !arg_num(vm, args[0], &S0) || !arg_num(vm, args[1], &mu) ||
        !arg_num(vm, args[2], &sigma) || !arg_num(vm, args[3], &T) || !arg_num(vm, args[4], &nv)) return;
    int64_t n = (int64_t)nv;
    Obj *res = AS_OBJ(make_list(vm));
    vm_push(vm, val_obj(res));
    if (!isfinite(S0) || S0 <= 0.0 || !isfinite(sigma) || sigma < 0.0 ||
        !isfinite(T) || T <= 0.0 || n <= 0 || !isfinite(mu)) {
        vm_pop(vm); *out = val_obj(res); return;
    }
    double dt = T / (double)n;
    double drift = (mu - 0.5 * sigma * sigma) * dt;
    double vol = sigma * sqrt(dt);
    list_push(vm, res, val_num(S0));
    double s = S0;
    for (int64_t i = 1; i <= n; i++) {
        s *= exp(drift + vol * bi_rand_normal_1(0.0, 1.0));
        list_push(vm, res, val_num(s));
    }
    vm_pop(vm);
    *out = val_obj(res);
}

static void native_sim_bridge(VM *vm, int argc, Value *args, Value *out) {
    double x0, xT, sigma, T, nv;
    if (argc != 5 || !arg_num(vm, args[0], &x0) || !arg_num(vm, args[1], &xT) ||
        !arg_num(vm, args[2], &sigma) || !arg_num(vm, args[3], &T) || !arg_num(vm, args[4], &nv)) return;
    int64_t n = (int64_t)nv;
    Obj *res = AS_OBJ(make_list(vm));
    vm_push(vm, val_obj(res));
    if (!isfinite(x0) || !isfinite(xT) || !isfinite(sigma) || sigma < 0.0 ||
        !isfinite(T) || T <= 0.0 || n <= 0) {
        vm_pop(vm); *out = val_obj(res); return;
    }
    double dt = T / (double)n;
    double vol = sigma * sqrt(dt);
    double *w = (double *)calloc((size_t)(n + 1), sizeof(double));
    if (!w) { vm_pop(vm); *out = val_obj(res); return; }
    for (int64_t i = 1; i <= n; i++)
        w[i] = w[i - 1] + vol * bi_rand_normal_1(0.0, 1.0);
    double span = xT - x0;
    for (int64_t i = 0; i <= n; i++) {
        double f = (double)i / (double)n;
        list_push(vm, res, val_num(x0 + span * f + (w[i] - f * w[n])));
    }
    free(w);
    vm_pop(vm);
    *out = val_obj(res);
}

static void native_sim_ou(VM *vm, int argc, Value *args, Value *out) {
    double x0, mu, theta, sigma, T, nv;
    if (argc != 6 || !arg_num(vm, args[0], &x0) || !arg_num(vm, args[1], &mu) ||
        !arg_num(vm, args[2], &theta) || !arg_num(vm, args[3], &sigma) ||
        !arg_num(vm, args[4], &T) || !arg_num(vm, args[5], &nv)) return;
    int64_t n = (int64_t)nv;
    Obj *res = AS_OBJ(make_list(vm));
    vm_push(vm, val_obj(res));
    if (!isfinite(x0) || !isfinite(theta) || theta <= 0.0 || !isfinite(sigma) ||
        sigma < 0.0 || !isfinite(T) || T <= 0.0 || n <= 0 || !isfinite(mu)) {
        vm_pop(vm); *out = val_obj(res); return;
    }
    double dt = T / (double)n;
    double decay = exp(-theta * dt);
    double sd = sigma * sqrt((1.0 - exp(-2.0 * theta * dt)) / (2.0 * theta));
    list_push(vm, res, val_num(x0));
    double x = x0;
    for (int64_t i = 1; i <= n; i++) {
        x = mu + (x - mu) * decay + sd * bi_rand_normal_1(0.0, 1.0);
        list_push(vm, res, val_num(x));
    }
    vm_pop(vm);
    *out = val_obj(res);
}

static void native_sim_merton(VM *vm, int argc, Value *args, Value *out) {
    double S0, mu, sigma, lambda, jmu, jsig, T, nv;
    if (argc != 8 || !arg_num(vm, args[0], &S0) || !arg_num(vm, args[1], &mu) ||
        !arg_num(vm, args[2], &sigma) || !arg_num(vm, args[3], &lambda) ||
        !arg_num(vm, args[4], &jmu) || !arg_num(vm, args[5], &jsig) ||
        !arg_num(vm, args[6], &T) || !arg_num(vm, args[7], &nv)) return;
    int64_t n = (int64_t)nv;
    Obj *res = AS_OBJ(make_list(vm));
    vm_push(vm, val_obj(res));
    if (!isfinite(S0) || S0 <= 0.0 || !isfinite(sigma) || sigma < 0.0 ||
        !isfinite(lambda) || lambda < 0.0 || !isfinite(jsig) || jsig < 0.0 ||
        !isfinite(T) || T <= 0.0 || n <= 0 || !isfinite(mu) || !isfinite(jmu)) {
        vm_pop(vm); *out = val_obj(res); return;
    }
    double dt = T / (double)n;
    double drift = (mu - 0.5 * sigma * sigma) * dt;
    double vol = sigma * sqrt(dt);
    double jrate = lambda * dt;
    list_push(vm, res, val_num(S0));
    double s = S0;
    for (int64_t i = 1; i <= n; i++) {
        double dz = bi_rand_normal_1(0.0, 1.0);
        int64_t nj = bi_poisson_knuth(jrate);
        double jl = (double)nj * jmu;
        if (nj > 0) jl += jsig * sqrt((double)nj) * bi_rand_normal_1(0.0, 1.0);
        s *= exp(drift + vol * dz + jl);
        list_push(vm, res, val_num(s));
    }
    vm_pop(vm);
    *out = val_obj(res);
}

static int bi_bs_valid(double S, double K, double r, double sigma, double T,
                       double *d1, double *d2)
{
    if (!isfinite(S) || S <= 0.0 || !isfinite(K) || K <= 0.0 ||
        !isfinite(r) || !isfinite(sigma) || sigma <= 0.0 || !isfinite(T) || T <= 0.0)
        return 0;
    double st = sigma * sqrt(T);
    *d1 = (log(S / K) + (r + 0.5 * sigma * sigma) * T) / st;
    *d2 = *d1 - st;
    return 1;
}

static double bi_normpdf(double x)
{
    return exp(-0.5 * x * x) * 0.39894228040143267794;
}

static void native_bs_call(VM *vm, int argc, Value *args, Value *out) {
    double S, K, r, sigma, T;
    if (argc != 5 || !arg_num(vm, args[0], &S) || !arg_num(vm, args[1], &K) ||
        !arg_num(vm, args[2], &r) || !arg_num(vm, args[3], &sigma) || !arg_num(vm, args[4], &T)) return;
    double d1, d2;
    if (!bi_bs_valid(S, K, r, sigma, T, &d1, &d2)) { *out = val_num(NAN); return; }
    double df = exp(-r * T);
    *out = val_num(S * bi_normcdf(d1) - K * df * bi_normcdf(d2));
}

static void native_bs_put(VM *vm, int argc, Value *args, Value *out) {
    double S, K, r, sigma, T;
    if (argc != 5 || !arg_num(vm, args[0], &S) || !arg_num(vm, args[1], &K) ||
        !arg_num(vm, args[2], &r) || !arg_num(vm, args[3], &sigma) || !arg_num(vm, args[4], &T)) return;
    double d1, d2;
    if (!bi_bs_valid(S, K, r, sigma, T, &d1, &d2)) { *out = val_num(NAN); return; }
    double df = exp(-r * T);
    *out = val_num(K * df * bi_normcdf(-d2) - S * bi_normcdf(-d1));
}

static void native_bs_delta_call(VM *vm, int argc, Value *args, Value *out) {
    double S, K, r, sigma, T;
    if (argc != 5 || !arg_num(vm, args[0], &S) || !arg_num(vm, args[1], &K) ||
        !arg_num(vm, args[2], &r) || !arg_num(vm, args[3], &sigma) || !arg_num(vm, args[4], &T)) return;
    double d1, d2;
    if (!bi_bs_valid(S, K, r, sigma, T, &d1, &d2)) { *out = val_num(NAN); return; }
    *out = val_num(bi_normcdf(d1));
}

static void native_bs_delta_put(VM *vm, int argc, Value *args, Value *out) {
    double S, K, r, sigma, T;
    if (argc != 5 || !arg_num(vm, args[0], &S) || !arg_num(vm, args[1], &K) ||
        !arg_num(vm, args[2], &r) || !arg_num(vm, args[3], &sigma) || !arg_num(vm, args[4], &T)) return;
    double d1, d2;
    if (!bi_bs_valid(S, K, r, sigma, T, &d1, &d2)) { *out = val_num(NAN); return; }
    *out = val_num(bi_normcdf(d1) - 1.0);
}

static void native_bs_gamma(VM *vm, int argc, Value *args, Value *out) {
    double S, K, r, sigma, T;
    if (argc != 5 || !arg_num(vm, args[0], &S) || !arg_num(vm, args[1], &K) ||
        !arg_num(vm, args[2], &r) || !arg_num(vm, args[3], &sigma) || !arg_num(vm, args[4], &T)) return;
    double d1, d2;
    if (!bi_bs_valid(S, K, r, sigma, T, &d1, &d2)) { *out = val_num(NAN); return; }
    *out = val_num(bi_normpdf(d1) / (S * sigma * sqrt(T)));
}

static void native_bs_vega(VM *vm, int argc, Value *args, Value *out) {
    double S, K, r, sigma, T;
    if (argc != 5 || !arg_num(vm, args[0], &S) || !arg_num(vm, args[1], &K) ||
        !arg_num(vm, args[2], &r) || !arg_num(vm, args[3], &sigma) || !arg_num(vm, args[4], &T)) return;
    double d1, d2;
    if (!bi_bs_valid(S, K, r, sigma, T, &d1, &d2)) { *out = val_num(NAN); return; }
    *out = val_num(S * bi_normpdf(d1) * sqrt(T));
}

/* Hypothesis testing (2026-10-29): mirrors rt.c's lume_bi_* — the same
 * Welford sample variance, the same distribution CDFs, the same
 * invalid-input policy (error on the interpreter / NAN natively). */
static double bi_list_sum(VM *vm, ObjList *L, int *n)
{
    int cnt; double *xs = list_to_doubles(vm, L, &cnt);
    if (!xs) return NAN;
    double s = 0.0;
    for (int i = 0; i < cnt; i++) s += xs[i];
    free(xs);
    if (n) *n = cnt;
    return s;
}

static void native_z_test(VM *vm, int argc, Value *args, Value *out) {
    ObjList *L;
    double mu, sigma;
    if (argc != 3 || !arg_list(vm, args[0], &L) ||
        !arg_num(vm, args[1], &mu) || !arg_num(vm, args[2], &sigma)) return;
    if (!isfinite(mu) || !isfinite(sigma) || sigma <= 0.0) { *out = val_num(NAN); return; }
    int n; double s = bi_list_sum(vm, L, &n);
    if (s != s || n < 1) { *out = val_num(NAN); return; }
    *out = val_num((s / (double)n - mu) / (sigma / sqrt((double)n)));
}

static void native_z_pvalue(VM *vm, int argc, Value *args, Value *out) {
    double z;
    if (argc < 1 || !arg_num(vm, args[0], &z)) return;
    if (!isfinite(z)) { *out = val_num(NAN); return; }
    double at = z < 0.0 ? -z : z;
    double p = 2.0 * (1.0 - bi_normcdf(at));
    *out = val_num(p > 1.0 ? 1.0 : p);
}

static void native_tstat(VM *vm, int argc, Value *args, Value *out) {
    ObjList *L;
    double mu;
    if (argc != 2 || !arg_list(vm, args[0], &L) ||
        !arg_num(vm, args[1], &mu)) return;
    if (!isfinite(mu)) { *out = val_num(NAN); return; }
    int n; double *xs = list_to_doubles(vm, L, &n);
    if (!xs || n < 2) { free(xs); *out = val_num(NAN); return; }
    double m = 0.0, m2 = 0.0;
    for (int i = 0; i < n; i++) {
        double d = xs[i] - m;
        m += d / (double)(i + 1);
        m2 += d * (xs[i] - m);
    }
    free(xs);
    double sd = sqrt(m2 / (double)(n - 1));
    if (sd <= 0.0) { *out = val_num(NAN); return; }
    *out = val_num((m - mu) / (sd / sqrt((double)n)));
}

static void native_ttest_p(VM *vm, int argc, Value *args, Value *out) {
    double t, df;
    if (argc < 2 || !arg_num(vm, args[0], &t) || !arg_num(vm, args[1], &df)) return;
    if (!isfinite(t) || !isfinite(df) || df <= 0.0) { *out = val_num(NAN); return; }
    double at = t < 0.0 ? -t : t;
    double p = 2.0 * (1.0 - bi_tdist_cdf(at, df));
    *out = val_num(p > 1.0 ? 1.0 : p);
}

static void native_chisq_stat(VM *vm, int argc, Value *args, Value *out) {
    ObjList *LO, *LE;
    if (argc != 2 || !arg_list(vm, args[0], &LO) || !arg_list(vm, args[1], &LE)) return;
    int no, ne;
    double *o = list_to_doubles(vm, LO, &no);
    if (!o) return;
    double *e = list_to_doubles(vm, LE, &ne);
    if (!e || no != ne || no < 1) { free(o); free(e); *out = val_num(NAN); return; }
    double total = 0.0;
    for (int i = 0; i < no; i++) {
        if (!isfinite(o[i]) || !isfinite(e[i]) || e[i] <= 0.0) {
            free(o); free(e); *out = val_num(NAN); return;
        }
        double d = o[i] - e[i];
        total += d * d / e[i];
    }
    free(o); free(e);
    *out = val_num(total);
}

static void native_chisq_p(VM *vm, int argc, Value *args, Value *out) {
    double x, df;
    if (argc < 2 || !arg_num(vm, args[0], &x) || !arg_num(vm, args[1], &df)) return;
    if (!isfinite(x) || x < 0.0 || !isfinite(df) || df <= 0.0) { *out = val_num(NAN); return; }
    double p = 1.0 - bi_chisq_cdf(x, df);
    *out = val_num(p < 0.0 ? 0.0 : p);
}

/* F-test family (2026-11-02): mirror rt.c — the sample-variance ratio
 * and the F upper tail through bi_ibeta. */
static double interp_sample_var(VM *vm, ObjList *L)
{
    if (L->count < 2) return NAN;
    int n; double *xs = list_to_doubles(vm, L, &n);
    if (!xs) return NAN;
    double m = 0.0, m2 = 0.0;
    for (int i = 0; i < n; i++) {
        double d = xs[i] - m;
        m += d / (double)(i + 1);
        m2 += d * (xs[i] - m);
    }
    free(xs);
    return m2 / (double)(n - 1);
}

static void native_f_stat(VM *vm, int argc, Value *args, Value *out) {
    ObjList *A, *B;
    if (argc < 2 || !arg_list(vm, args[0], &A) || !arg_list(vm, args[1], &B)) return;
    double va = interp_sample_var(vm, A), vb = interp_sample_var(vm, B);
    if (!(va == va) || !(vb == vb)) { *out = val_num(NAN); return; }
    *out = val_num(vb == 0.0 ? NAN : va / vb);
}

static void native_f_pvalue(VM *vm, int argc, Value *args, Value *out) {
    double f, df1, df2;
    if (argc < 3 || !arg_num(vm, args[0], &f) || !arg_num(vm, args[1], &df1) ||
        !arg_num(vm, args[2], &df2)) return;
    if (!isfinite(f) || f < 0.0 || !isfinite(df1) || !isfinite(df2) ||
        df1 <= 0.0 || df2 <= 0.0) { *out = val_num(NAN); return; }
    double x = f * df1 / (f * df1 + df2);
    double p = 1.0 - bi_ibeta(df1 / 2.0, df2 / 2.0, x);
    *out = val_num(p < 0.0 ? 0.0 : p);
}

/* Multivariate / copula samplers (2026-10-30): independent interpreter-side
 * implementations of the same formulas as rt.c's lume_bi_rand_dirichlet /
 * rand_binormal / rand_gauss_copula / rand_t_copula, driven by this VM's own
 * bi_rng stream (bi_rand_normal_1 per Box-Muller z, bi_gamma_sample for the
 * shared chi-square draw, bi_normcdf / bi_tdist_cdf for the probability
 * transforms), so each backend advances its own PCG32 state identically.
 * Each answers a fresh flat float list; invalid parameters error here and
 * answer an empty list natively. */
static void native_rand_dirichlet(VM *vm, int argc, Value *args, Value *out) {
    ObjList *L;
    if (argc < 1 || !arg_list(vm, args[0], &L)) return;
    int n; double *a = list_to_doubles(vm, L, &n);
    if (!a) return;
    if (n < 1) { free(a); vm_set_error(vm, "rand_dirichlet(): need a non-empty alphas list"); return; }
    for (int i = 0; i < n; i++)
        if (a[i] <= 0.0 || a[i] != a[i]) { free(a); vm_set_error(vm, "rand_dirichlet(): alphas must be positive"); return; }
    double *g = (double *)malloc((size_t)n * sizeof(double));
    if (!g) { free(a); vm_set_error(vm, "out of memory"); return; }
    double sum = 0.0;
    for (int i = 0; i < n; i++) { g[i] = bi_gamma_sample(a[i]); sum += g[i]; }
    free(a);
    if (!(sum > 0.0)) { free(g); vm_set_error(vm, "rand_dirichlet(): sample sum is 0"); return; }
    Obj *res = AS_OBJ(make_list(vm));
    vm_push(vm, val_obj(res));
    for (int i = 0; i < n; i++) list_push(vm, res, val_num(g[i] / sum));
    free(g);
    vm_pop(vm);
    *out = val_obj(res);
}

static void native_rand_binormal(VM *vm, int argc, Value *args, Value *out) {
    double mu1, mu2, sd1, sd2, rho, n;
    if (argc < 6 || !arg_num(vm, args[0], &mu1) || !arg_num(vm, args[1], &mu2) ||
        !arg_num(vm, args[2], &sd1) || !arg_num(vm, args[3], &sd2) ||
        !arg_num(vm, args[4], &rho) || !arg_num(vm, args[5], &n)) return;
    if (!isfinite(mu1) || !isfinite(mu2) || !isfinite(sd1) || sd1 < 0.0 ||
        !isfinite(sd2) || sd2 < 0.0 || !isfinite(rho) || rho <= -1.0 ||
        rho >= 1.0 || n < 1.0 || n != n) {
        vm_set_error(vm, "rand_binormal(): invalid parameters"); return;
    }
    int64_t nn = (int64_t)n;
    double c = sqrt(1.0 - rho * rho);
    Obj *res = AS_OBJ(make_list(vm));
    vm_push(vm, val_obj(res));
    for (int64_t j = 0; j < nn; j++) {
        double z1 = bi_rand_normal_1(0.0, 1.0);
        double z2 = bi_rand_normal_1(0.0, 1.0);
        list_push(vm, res, val_num(mu1 + sd1 * z1));
        list_push(vm, res, val_num(mu2 + sd2 * (rho * z1 + c * z2)));
    }
    vm_pop(vm);
    *out = val_obj(res);
}

static void native_rand_gauss_copula(VM *vm, int argc, Value *args, Value *out) {
    double rho, n;
    if (argc < 2 || !arg_num(vm, args[0], &rho) || !arg_num(vm, args[1], &n)) return;
    if (!isfinite(rho) || rho <= -1.0 || rho >= 1.0 || n < 1.0 || n != n) {
        vm_set_error(vm, "rand_gauss_copula(): invalid parameters"); return;
    }
    int64_t nn = (int64_t)n;
    double c = sqrt(1.0 - rho * rho);
    Obj *res = AS_OBJ(make_list(vm));
    vm_push(vm, val_obj(res));
    for (int64_t j = 0; j < nn; j++) {
        double z1 = bi_rand_normal_1(0.0, 1.0);
        double z2 = bi_rand_normal_1(0.0, 1.0);
        list_push(vm, res, val_num(bi_normcdf(z1)));
        list_push(vm, res, val_num(bi_normcdf(rho * z1 + c * z2)));
    }
    vm_pop(vm);
    *out = val_obj(res);
}

static void native_rand_t_copula(VM *vm, int argc, Value *args, Value *out) {
    double rho, df, n;
    if (argc < 3 || !arg_num(vm, args[0], &rho) || !arg_num(vm, args[1], &df) ||
        !arg_num(vm, args[2], &n)) return;
    if (!isfinite(rho) || rho <= -1.0 || rho >= 1.0 || !isfinite(df) || df <= 0.0 ||
        n < 1.0 || n != n) {
        vm_set_error(vm, "rand_t_copula(): invalid parameters"); return;
    }
    int64_t nn = (int64_t)n;
    double c = sqrt(1.0 - rho * rho);
    Obj *res = AS_OBJ(make_list(vm));
    vm_push(vm, val_obj(res));
    for (int64_t j = 0; j < nn; j++) {
        double z1 = bi_rand_normal_1(0.0, 1.0);
        double z2 = bi_rand_normal_1(0.0, 1.0);
        double v = 2.0 * bi_gamma_sample(0.5 * df);
        double s = sqrt(df / v);
        list_push(vm, res, val_num(bi_tdist_cdf(z1 * s, df)));
        list_push(vm, res, val_num(bi_tdist_cdf((rho * z1 + c * z2) * s, df)));
    }
    vm_pop(vm);
    *out = val_obj(res);
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
Value b_seed(VM *vm, int argc, Value *args)      { return vm_native(vm, argc, args, native_seed); }
Value b_rand(VM *vm, int argc, Value *args)      { return vm_native(vm, argc, args, native_rand); }
Value b_rand_range(VM *vm, int argc, Value *args){ return vm_native(vm, argc, args, native_rand_range); }
Value b_rand_int(VM *vm, int argc, Value *args)  { return vm_native(vm, argc, args, native_rand_int); }
Value b_rand_normal(VM *vm, int argc, Value *args){ return vm_native(vm, argc, args, native_rand_normal); }
Value b_rand_bool(VM *vm, int argc, Value *args)        { return vm_native(vm, argc, args, native_rand_bool); }
Value b_rand_exponential(VM *vm, int argc, Value *args) { return vm_native(vm, argc, args, native_rand_exponential); }
Value b_rand_poisson(VM *vm, int argc, Value *args)     { return vm_native(vm, argc, args, native_rand_poisson); }
Value b_shuffle(VM *vm, int argc, Value *args)          { return vm_native(vm, argc, args, native_shuffle); }
Value b_rand_cauchy(VM *vm, int argc, Value *args)      { return vm_native(vm, argc, args, native_rand_cauchy); }
Value b_rand_gamma(VM *vm, int argc, Value *args)       { return vm_native(vm, argc, args, native_rand_gamma); }
Value b_rand_beta(VM *vm, int argc, Value *args)        { return vm_native(vm, argc, args, native_rand_beta); }
Value b_rand_chisq(VM *vm, int argc, Value *args)       { return vm_native(vm, argc, args, native_rand_chisq); }
Value b_rand_student_t(VM *vm, int argc, Value *args)   { return vm_native(vm, argc, args, native_rand_student_t); }
Value b_rand_geometric(VM *vm, int argc, Value *args)   { return vm_native(vm, argc, args, native_rand_geometric); }
Value b_rand_weibull(VM *vm, int argc, Value *args)     { return vm_native(vm, argc, args, native_rand_weibull); }
Value b_rand_logistic(VM *vm, int argc, Value *args)    { return vm_native(vm, argc, args, native_rand_logistic); }
Value b_rand_pareto(VM *vm, int argc, Value *args)      { return vm_native(vm, argc, args, native_rand_pareto); }
Value b_rand_laplace(VM *vm, int argc, Value *args)     { return vm_native(vm, argc, args, native_rand_laplace); }
Value b_rand_binomial(VM *vm, int argc, Value *args)    { return vm_native(vm, argc, args, native_rand_binomial); }
Value b_rand_lognormal(VM *vm, int argc, Value *args)   { return vm_native(vm, argc, args, native_rand_lognormal); }
Value b_rand_gumbel(VM *vm, int argc, Value *args)      { return vm_native(vm, argc, args, native_rand_gumbel); }
Value b_rand_rayleigh(VM *vm, int argc, Value *args)    { return vm_native(vm, argc, args, native_rand_rayleigh); }
Value b_rand_triangular(VM *vm, int argc, Value *args)  { return vm_native(vm, argc, args, native_rand_triangular); }
Value b_rand_zipf(VM *vm, int argc, Value *args)        { return vm_native(vm, argc, args, native_rand_zipf); }
Value b_rand_negative_binomial(VM *vm, int argc, Value *args) { return vm_native(vm, argc, args, native_rand_negative_binomial); }
Value b_rand_erlang(VM *vm, int argc, Value *args)      { return vm_native(vm, argc, args, native_rand_erlang); }
Value b_rand_von_mises(VM *vm, int argc, Value *args)   { return vm_native(vm, argc, args, native_rand_von_mises); }
Value b_rand_multinomial(VM *vm, int argc, Value *args) { return vm_native(vm, argc, args, native_rand_multinomial); }
Value b_sim_gbm(VM *vm, int argc, Value *args)      { return vm_native(vm, argc, args, native_sim_gbm); }
Value b_sim_bridge(VM *vm, int argc, Value *args)   { return vm_native(vm, argc, args, native_sim_bridge); }
Value b_sim_ou(VM *vm, int argc, Value *args)       { return vm_native(vm, argc, args, native_sim_ou); }
Value b_sim_merton(VM *vm, int argc, Value *args)   { return vm_native(vm, argc, args, native_sim_merton); }
Value b_bs_call(VM *vm, int argc, Value *args)      { return vm_native(vm, argc, args, native_bs_call); }
Value b_bs_put(VM *vm, int argc, Value *args)       { return vm_native(vm, argc, args, native_bs_put); }
Value b_bs_delta_call(VM *vm, int argc, Value *args){ return vm_native(vm, argc, args, native_bs_delta_call); }
Value b_bs_delta_put(VM *vm, int argc, Value *args) { return vm_native(vm, argc, args, native_bs_delta_put); }
Value b_bs_gamma(VM *vm, int argc, Value *args)     { return vm_native(vm, argc, args, native_bs_gamma); }
Value b_bs_vega(VM *vm, int argc, Value *args)      { return vm_native(vm, argc, args, native_bs_vega); }
Value b_z_test(VM *vm, int argc, Value *args)        { return vm_native(vm, argc, args, native_z_test); }
Value b_z_pvalue(VM *vm, int argc, Value *args)      { return vm_native(vm, argc, args, native_z_pvalue); }
Value b_tstat(VM *vm, int argc, Value *args)         { return vm_native(vm, argc, args, native_tstat); }
Value b_ttest_p(VM *vm, int argc, Value *args)       { return vm_native(vm, argc, args, native_ttest_p); }
Value b_chisq_stat(VM *vm, int argc, Value *args)    { return vm_native(vm, argc, args, native_chisq_stat); }
Value b_chisq_p(VM *vm, int argc, Value *args)       { return vm_native(vm, argc, args, native_chisq_p); }
Value b_f_stat(VM *vm, int argc, Value *args)        { return vm_native(vm, argc, args, native_f_stat); }
Value b_f_pvalue(VM *vm, int argc, Value *args)      { return vm_native(vm, argc, args, native_f_pvalue); }
Value b_rand_dirichlet(VM *vm, int argc, Value *args)    { return vm_native(vm, argc, args, native_rand_dirichlet); }
Value b_rand_binormal(VM *vm, int argc, Value *args)     { return vm_native(vm, argc, args, native_rand_binormal); }
Value b_rand_gauss_copula(VM *vm, int argc, Value *args) { return vm_native(vm, argc, args, native_rand_gauss_copula); }
Value b_rand_t_copula(VM *vm, int argc, Value *args)     { return vm_native(vm, argc, args, native_rand_t_copula); }
Value b_erf(VM *vm, int argc, Value *args)       { return vm_native(vm, argc, args, native_erf); }
Value b_gamma(VM *vm, int argc, Value *args)     { return vm_native(vm, argc, args, native_gamma); }
Value b_lgamma(VM *vm, int argc, Value *args)    { return vm_native(vm, argc, args, native_lgamma); }
Value b_igamma(VM *vm, int argc, Value *args)    { return vm_native(vm, argc, args, native_igamma); }
Value b_ibeta(VM *vm, int argc, Value *args)     { return vm_native(vm, argc, args, native_ibeta); }
Value b_tdist_cdf(VM *vm, int argc, Value *args) { return vm_native(vm, argc, args, native_tdist_cdf); }
Value b_chisq_cdf(VM *vm, int argc, Value *args) { return vm_native(vm, argc, args, native_chisq_cdf); }


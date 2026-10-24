/* ===================== rt.c — table of contents =====================
 *
 *   1. Print helpers (lume_print_i64 / _double / _bool / _str / _null)                 (L66)
 *   2. String allocation & runtime helpers (LUME_STR_MIN)                              (L143)
 *   3. Statistical math builtins (normcdf / igamma / ibeta / t / chi-squared CDF)      (L218)
 *   4. Runtime value types: LumeSlot / LumeList / LumeMap / LumeClosure                (L373)
 *   5. List runtime: push / grow                                                       (L425)
 *   6. argv() — CLI trailing arguments (SPEC 5)                                        (L500)
 *   7. Value rendering: slot / list -> text & JSON (container_body / slot_emit / jb_*) (L548)
 *   8. Map runtime: grow / find                                                        (L885)
 *   9. Error handling & list sort                                                      (L978)
 *  10. Indexing & container type predicates                                            (L1057)
 *  11. Map put / get / accumulate                                                      (L1293)
 *  12. Value equality: slot / value / any                                              (L1537)
 *  13. Map helpers: len / print                                                        (L1702)
 *  14. JSON parser (text -> tagged-any tree)                                           (L1768)
 *  15. Builtins: contains / log / parse / bool                                         (L2122)
 *  16. Slot accessors: get / accumulate / length                                       (L2396)
 *
 *  Compiled standalone by backend.c (LUME_RT_SRC) and linted via LINT_SRCS.
 *  No rt.h: this TU is self-contained; native/LLVM emit code links it.
 * ==================================================================== */

/* lume runtime (native backend) — non-variadic print helpers.
 *
 * Why this file exists: on macOS/arm64, `va_start` reads an argument save area
 * that the *caller* is required to build — that is why the C compiler emits a
 * `str x8, [sp]` sequence before `bl _printf`. A hand-written .ll that simply
 * leaves a value in x1 prints garbage (the callee re-reads whatever the stack
 * happens to hold). Declaring printf with a fixed prototype does not help,
 * because the broken part is the caller-side save area, not the signature.
 *
 * These helpers are compiled *from C*, so the compiler emits a correctly set
 * up call; the IR backend only issues ordinary non-variadic calls to them,
 * which pass arguments in plain integer registers.
 */

#include <math.h>
#include <regex.h>
#include <stdint.h>
#include <stdbool.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

/* Windows CRT defaults stdout/stderr to text mode, translating a '\n' written
 * by the C runtime into '\r\n'. That makes program output redirected to a file
 * differ from the LF-terminated expected files the test suite diffs against (and
 * from POSIX output), so native-text / native-consistency / ai-verify fail on
 * Windows while the same binaries pass on macOS/Linux. Put redirect/piped
 * streams into binary mode so output is byte-identical across platforms. This
 * mirrors the per-entry _setmode calls already in bridge_mcp.c / bridge_lsp.c,
 * lifted to the shared runtime so the interpreter and every native-compiled
 * program (both link rt.o) are covered from one place. Guarded to Windows: the
 * functions and macros below do not exist elsewhere. */
#if defined(_WIN32)
#include <io.h>
#include <fcntl.h>
static void lume_stdio_binary_init(void) __attribute__((constructor));
static void lume_stdio_binary_init(void) {
    if (!_isatty(_fileno(stdout))) _setmode(_fileno(stdout), _O_BINARY);
    if (!_isatty(_fileno(stderr))) _setmode(_fileno(stderr), _O_BINARY);
}
#endif

/* ============ 1. Print helpers (lume_print_i64 / _double / _bool / _str / _null) ============ */
static int emit(const char *s)
{
    fputs(s, stdout);
    return (int)strlen(s);
}

/* Defined below but called from lume_print_str's null guard. */
int64_t lume_print_null(void);

int64_t lume_print_i64(int64_t v)
{
    char buf[40];
    /* (long long): int64_t is `long` on Linux LP64, so a bare %lld makes the
     * runtime cc -O2 step (backend.c) print -Wformat warnings — and
     * native_backends.sh treats any compiler output as a failure. */
    int n = snprintf(buf, sizeof buf, "%lld\n", (long long)v);
    emit(buf);
    return n;
}

int64_t lume_print_double(double v)
{
    char buf[64];
    /* %g, not %f: the interpreter prints floats the same way (`print(1.0)`
     * is "1", `print(1e6)` is "1e+06"), and this helper is what a compiled
     * script calls, so the two have to agree or the same program prints
     * differently compiled than interpreted. */
    int n = snprintf(buf, sizeof buf, "%g\n", v);
    emit(buf);
    return n;
}

int64_t lume_print_bool(int64_t v)
{
    return emit(v ? "true\n" : "false\n");
}

int64_t lume_print_str(const char *s)
{
    /* Same line-ending contract as the other helpers: every print() ends the
     * line, so mixed int/string output stays readable.
     *
     * The null guard is load-bearing, not defensive padding. A `let x: string
     * = null` reaches here as a null `i8*` (both native backends represent
     * TY_NULL as an opaque pointer), and handing that to snprintf's "%s" is
     * undefined behaviour -- glibc happens to print "(null)", but nothing
     * promises it. The backends now reject null for scalar annotations, so
     * only the string-annotated case can land here, and it must print the
     * same bare word the interpreter prints. */
    if (!s) return lume_print_null();
    char buf[512];
    int n = snprintf(buf, sizeof buf, "%s\n", s);
    if (n > (int)sizeof buf - 1) n = (int)sizeof buf - 1;
    return emit(buf);
}

/* `null` has no value to pass — it prints the bare word and takes no
 * argument, so `print(null)` matches the interpreter's "null" exactly. */
int64_t lume_print_null(void)
{
    return emit("null\n");
}

/* ------------------------------------------------------------------ *
 * Builtin helpers.
 *
 * lume's own builtins are Value-based: `Native2(VM *, int, Value *, Value *)`.
 * A compile-time backend has no VM to hand them and no way to rebuild the
 * Value layout in IR, so the native backend exposes its own plain-scalar
 * helpers and lowers builtin calls to those instead. They are deliberately
 * dumb — no VM, no error reporting, no allocation policy — and the type
 * checker is what keeps them honest.
 * ------------------------------------------------------------------ */

static char *dup_str(const char *s)
{
/* ============ 2. String allocation & runtime helpers (LUME_STR_MIN) ============ */
    /* Every heap string gets at least LUME_STR_MIN zeroed bytes, even a
     * 1-char one: lume_any_eq tells a string from a container by reading the
     * container `kind` tag at a fixed offset, and a short malloc would make
     * that read run off the allocation. calloc also keeps those bytes
     * determinate rather than indeterminate. */
    enum { LUME_STR_MIN = 32 };
    size_t n = strlen(s) + 1;
    if (n < LUME_STR_MIN) n = LUME_STR_MIN;
    char *p = (char *)calloc(1, n);
    if (p) memcpy(p, s, strlen(s) + 1);
    return p;
}

char *lume_bi_str_i64(int64_t v)
{
    char buf[32];
    snprintf(buf, sizeof buf, "%lld", (long long)v);
    return dup_str(buf);
}

char *lume_bi_str_double(double v)
{
    char buf[48];
    snprintf(buf, sizeof buf, "%g", v);
    return dup_str(buf);
}

char *lume_bi_str_bool(int64_t v) { return dup_str(v ? "true" : "false"); }

char *lume_bi_cat(const char *a, const char *b)
{
    size_t na = a ? strlen(a) : 0;
    size_t nb = b ? strlen(b) : 0;
    char *p = malloc(na + nb + 1);
    if (!p) return NULL;
    memcpy(p, a, na);
    memcpy(p + na, b, nb);
    p[na + nb] = '\0';
    return p;
}

int64_t lume_bi_abs(int64_t v)     { return v < 0 ? -v : v; }
int64_t lume_bi_min(int64_t a, int64_t b) { return a < b ? a : b; }
int64_t lume_bi_max(int64_t a, int64_t b) { return a > b ? a : b; }
double lume_bi_minf(double a, double b) { return a < b ? a : b; }
double lume_bi_maxf(double a, double b) { return a > b ? a : b; }

double lume_bi_sqrt(double v) { return sqrt(v); }
double lume_bi_pow(double a, double b) { return pow(a, b); }
double lume_bi_floor(double v) { return floor(v); }
double lume_bi_ceil(double v)  { return ceil(v); }
double lume_bi_round(double v) { return round(v); }

/* ------------------------------------------------------------------ *
 * Statistical distribution builtins (P4, 2026-10-20).
 *
 * The medical-stats pipeline needs p-values: correlation r -> t tail,
 * group-baseline t / chi-square, regression coefficient t / Wald tests.
 * The engine's hand-written `normal_quantile` set the house style for
 * self-contained numerical code, so these live here as plain C on top of
 * math.h — erf/lgamma from the C library, the regularized incomplete
 * Gamma and Beta as self-authored series / continued fractions (the
 * standard Temme / Lentz forms), and t / chi-square CDFs as one-liners
 * on top of them.
 *
 * The native backend has no error channel of its own, so out-of-domain
 * arguments come back as NaN (the interpreter's twin implementations
 * report vm_set_error instead). The CDFs clamp at their tails the way a
 * CDF should (x <= 0 -> 0, +inf -> 1).
 * ------------------------------------------------------------------ */

/* Regularized incomplete Gamma P(a,x), lower tail. Series form, good for
 * x < a+1. The prefactor goes through exp() so huge a or x underflows to
 * zero instead of overflowing to inf. */
/* ============ 3. Statistical math builtins (normcdf / igamma / ibeta / t / chi-squared CDF) ============ */
static double igamma_series(double a, double x)
{
    double lnf = -x + a * log(x) - lgamma(a);
    if (lnf < -745.0) return 0.0;      /* exp underflow */
    double term = 1.0 / a, sum = term;
    for (int n = 1; n < 2000; n++) {
        term *= x / (a + n);
        sum += term;
        if (term < sum * 1e-15) break;
    }
    return exp(lnf) * sum;
}

/* Regularized incomplete Gamma Q(a,x) = 1 - P(a,x), upper tail, via the
 * Temme continued fraction, good for x >= a+1. Evaluated with the modified
 * Lentz algorithm so a divergent-looking early term cannot blow up. */
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

/* Continued fraction for the incomplete Beta (Lentz), the I_x(a,b) core. */
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

double lume_bi_erf(double x) { return erf(x); }

double lume_bi_normcdf(double x)
{
    /* Phi(x) = (1 + erf(x/sqrt(2)))/2; 1/sqrt(2) written out so MSVC's
     * missing M_SQRT1_2 cannot bite. */
    return 0.5 * (1.0 + erf(x * 0.70710678118654752440));
}

/* Inverse standard-normal CDF Phi^{-1}(p). Acklam's rational
 * approximation (rev. 2011): max |error| ~1.15e-9, with dedicated
 * low/high-tail branches so extreme p keep full precision. p outside
 * (0,1) answers NAN — the native backend has no error channel (the
 * interpreter's twin reports vm_set_error). */
double lume_bi_norminv(double p)
{
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

/* ------------------------------------------------------------------ *
 * Monte Carlo RNG builtins (2026-10-24).
 *
 * Deterministic PCG32 (O'Neill, PCG-XSH-RR) behind a fixed default seed,
 * so the same program produces the same stream on every run and every
 * backend — the interpreter (builtins_math.c) and both native backends
 * share the same constants and the same consume order, keeping output
 * byte-identical. seed(n) re-bases the stream for reproducible
 * experiments. rand() maps the top 24 bits to [0,1); rand_normal adds a
 * 2^-25 bias so u1/u2 stay in (0,1) and log() cannot hit zero.
 * ------------------------------------------------------------------ */
static uint64_t rng_state = 0x853c49e6748fea9bULL;
static uint64_t rng_inc   = 0xda3e39cb94b95bdbULL;

static uint32_t rng_next(void)
{
    uint64_t old = rng_state;
    rng_state = old * 6364136223846793005ULL + rng_inc;
    uint32_t xs = (uint32_t)(((old >> 18u) ^ old) >> 27u);
    uint32_t rot = (uint32_t)(old >> 59u);
    return (xs >> rot) | (xs << ((32u - rot) & 31u));
}

int64_t lume_bi_seed(int64_t s)
{
    rng_state = (uint64_t)s;
    rng_inc   = 0xda3e39cb94b95bdbULL;
    return s;
}

double lume_bi_rand(void)
{
    return (double)(rng_next() >> 8) * 0.000000059604644775390625; /* 2^-24 */
}

double lume_bi_rand_range(double lo, double hi)
{
    if (!isfinite(lo) || !isfinite(hi) || hi < lo) return NAN;
    return lo + (hi - lo) * lume_bi_rand();
}

int64_t lume_bi_rand_int(int64_t lo, int64_t hi)
{
    if (hi < lo) return -1;   /* no error channel natively; interp reports */
    uint64_t span = (uint64_t)(hi - lo) + 1ULL;
    uint64_t r = (uint64_t)rng_next() | ((uint64_t)rng_next() << 32u);
    return lo + (int64_t)(r % span);
}

double lume_bi_rand_normal(double mu, double sigma)
{
    double u1 = (double)(rng_next() >> 8) * 0.000000059604644775390625
                + 0.0000000298023223876953125; /* +2^-25 */
    double u2 = (double)(rng_next() >> 8) * 0.000000059604644775390625
                + 0.0000000298023223876953125;
    double z = sqrt(-2.0 * log(u1)) * cos(6.28318530717958647693 * u2);
    return mu + sigma * z;
}

/* Bernoulli draw: 1 with probability p (0 <= p <= 1), else 0. The draw is
 * the same rand() the interpreter uses, so the fixed stream keeps every
 * backend identical; an out-of-range p answers 0 natively (the interpreter
 * reports an error). */
int64_t lume_bi_rand_bool(double p)
{
    if (!isfinite(p) || p < 0.0 || p > 1.0) return 0;
    return ((double)(rng_next() >> 8) * 0.000000059604644775390625) < p;
}

/* Exponential(rate): inverse-CDF -ln(u)/rate, with the same 2^-25 anti-zero
 * bias on u as rand_normal (u == 0 would give +inf). */
double lume_bi_rand_exponential(double rate)
{
    if (!isfinite(rate) || rate <= 0.0) return NAN;
    double u = (double)(rng_next() >> 8) * 0.000000059604644775390625
               + 0.0000000298023223876953125;
    return -log(u) / rate;
}

/* Poisson(lambda): Knuth's multiplicative method. Each step consumes one
 * u (biased like rand_normal); exp(-lambda) == 0 means lambda is so large
 * the loop could never terminate, so that answers -1 natively (the
 * interpreter reports an error). */
int64_t lume_bi_rand_poisson(double lambda)
{
    if (!isfinite(lambda) || lambda <= 0.0) return -1;
    double l = exp(-lambda);
    if (!(l > 0.0)) return -1;
    double p = 1.0;
    int64_t k = 0;
    do {
        k++;
        double u = (double)(rng_next() >> 8) * 0.000000059604644775390625
                   + 0.0000000298023223876953125;
        p *= u;
    } while (p > l);
    return k - 1;
}

double lume_bi_lgamma(double x) { return lgamma(x); }

double lume_bi_gamma(double x)  { return exp(lgamma(x)); }

/* Regularized incomplete Gamma P(a,x): series below a+1, 1 - Q above. */
double lume_bi_igamma(double a, double x)
{
    if (a <= 0.0 || x < 0.0 || !isfinite(a)) return NAN;
    if (x == 0.0) return 0.0;
    if (!isfinite(x)) return 1.0;
    return x < a + 1.0 ? igamma_series(a, x) : 1.0 - igamma_cf(a, x);
}

/* Regularized incomplete Beta I_x(a,b). Uses the symmetry I_x(a,b) =
 * 1 - I_{1-x}(b,a) on the right half so the continued fraction stays on
 * its convergent side. */
double lume_bi_ibeta(double a, double b, double x)
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
    /* Right half: 1 - I_{1-x}(b,a); front shares lnbt so only the flipped
     * powers change. */
    double lr = b * log(1.0 - x) + a * log(x) - lnbt;
    if (lr < -745.0) return 1.0;
    return 1.0 - exp(lr) * ibeta_cf(b, a, 1.0 - x) / b;
}

/* Student t CDF. P(T <= t) = 1 - 0.5 * I_{df/(df+t^2)}(df/2, 1/2), with the
 * mirror P(T <= -t) = 1 - P(T <= t) fixing the negative half. */
double lume_bi_tdist_cdf(double t, double df)
{
    if (df <= 0.0 || !isfinite(df)) return NAN;
    if (!isfinite(t)) return t < 0.0 ? 0.0 : 1.0;
    double x = df / (df + t * t);
    double ib = lume_bi_ibeta(df / 2.0, 0.5, x);
    return t >= 0.0 ? 1.0 - 0.5 * ib : 0.5 * ib;
}

/* Chi-square CDF: P(X <= x) = igamma(df/2, x/2). */
double lume_bi_chisq_cdf(double x, double df)
{
    if (df <= 0.0 || !isfinite(df)) return NAN;
    if (x <= 0.0) return 0.0;
    if (!isfinite(x)) return 1.0;
    return lume_bi_igamma(df / 2.0, x / 2.0);
}


int64_t   lume_bi_int(double v)  { return (int64_t)v; }
double lume_bi_float(int64_t v)  { return (double)v; }
int64_t lume_bi_len_s(const char *s) { return s ? (int64_t)strlen(s) : 0; }

/* ------------------------------------------------------------------ *
 * List and map objects.
 *
 * The interpreter keeps every value behind a GC-managed Value, so a list is
 * an Obj and a map is an Obj. The native backend has no GC, so these are
 * plain malloc'd records reached through an opaque pointer — codegen knows
 * the static element type and picks whichever helper fits, which is why the
 * slots carry a tag instead of a whole union: one push() lowers onto one
 * helper for ints, floats and strings alike.
 *
 * `num` holds an integer or the bit pattern of a double, `str` the body of
 * a string. A missing index reads as a zero slot rather than crashing: the
 * native path has no error channel of its own, and the type checker's
 * bounds rules are the thing that is supposed to keep indices in range.
 * ------------------------------------------------------------------ */

#define LUME_SLOT_INT    0
#define LUME_SLOT_FLOAT  1
#define LUME_SLOT_STR    2
/* ============ 4. Runtime value types: LumeSlot / LumeList / LumeMap / LumeClosure ============ */
/* A nested container: another LumeList or LumeMap, reached through the same
 * opaque i8* everything else travels as. Before this tag every slot held a
 * scalar, so `{ a: { b: 1 } }` and `[[1,2],[3]]` could not be built at all —
 * the emitters refused with "map values must be int, float or string" /
 * "list elements must be int, float or string" while the interpreter has
 * always accepted them. */
#define LUME_SLOT_OBJ    3
/* A bool. The interpreter renders `true`/`false` inside containers, so the
 * native side needs its own tag rather than borrowing LUME_SLOT_INT (which
 * rendered container bools as 1/0). `num` is 0 or 1. */
#define LUME_SLOT_BOOL   4

typedef struct { int64_t num; const char *str; int tag; } LumeSlot;

/* Which container kind an LUME_SLOT_OBJ pointer refers to. A bare i8* does
 * not say, and the printer has to know to walk it as a list or as a map, so
 * the tag lives in the object itself and the slot just carries the address. */
#define LUME_CTR_LIST    0x4c49   /* 'LI' */
#define LUME_CTR_MAP     0x4d41   /* 'MA' */

typedef struct { int64_t len, cap; int kind; LumeSlot *items; }    LumeList;
typedef struct { int64_t len, cap; int kind; char **keys; LumeSlot *vals; } LumeMap;

/* A first-class function value for the native backends. `fn` is the raw
 * function pointer (a bitcast i8* in IR); `cap` is reserved for capture and
 * is always null in phase 1 (no-capture closures only). The whole struct is
 * opaque to the emitted IR — it travels as an i8* and the backends read
 * `.fn` / `.cap` by fixed byte offset (two pointers, so cap sits at +8). The
 * closure ABI is shared by both native backends so a `let f = (x) => ...`
 * yields the same record whether it runs through the text emitter or the
 * libLLVM emitter. */
typedef struct { void *fn; void *cap; } LumeClosure;

static void *rt_alloc(size_t n)
{
    void *p = calloc(1, n ? n : 1);
    if (!p) { fputs("lume(native): out of memory\n", stderr); exit(70); }
    return p;
}

/* Build a closure record. Called once per evaluation of a function literal;
 * each call returns a distinct heap record so closures stored in variables
 * keep stable addresses. Returns an opaque i8* to the compiled IR. */
LumeClosure *lume_closure_make(void *fn, void *cap)
{
    LumeClosure *c = rt_alloc(sizeof(LumeClosure));
    c->fn = fn;
    c->cap = cap;
    return c;
}

/* ============ 5. List runtime: push / grow ============ */
static void list_grow(LumeList *l)
{
    int64_t cap = l->cap ? l->cap * 2 : 8;
    LumeSlot *it = realloc(l->items, (size_t)cap * sizeof *it);
    if (!it) { fputs("lume(native): out of memory\n", stderr); exit(70); }
    memset(it + (size_t)l->cap, 0, (size_t)(cap - l->cap) * sizeof *it);
    l->items = it;
    l->cap = cap;
}

LumeList *lume_list_new(void)
{
    LumeList *l = rt_alloc(sizeof(LumeList));
    l->kind = LUME_CTR_LIST;
    return l;
}

int64_t lume_list_len(const LumeList *l) { return l ? l->len : 0; }

/* One push per element type: the caller picked the flavour from the static
 * type, so the slot can be filled without a tag round-trip. */
int64_t lume_list_push_i(LumeList *l, int64_t v)
{
    if (!l) return 0;
    if (l->len >= l->cap) list_grow(l);
    l->items[l->len].num = v;
    l->items[l->len].tag = LUME_SLOT_INT;
    return ++l->len;
}

/* Push a bool with its own tag so print()/str() render `true`/`false` the way
 * the interpreter does (an INT-tagged 1 rendered as `1`). */
int64_t lume_list_push_b(LumeList *l, int64_t v)
{
    if (!l) return 0;
    if (l->len >= l->cap) list_grow(l);
    l->items[l->len].num = v ? 1 : 0;
    l->items[l->len].tag = LUME_SLOT_BOOL;
    return ++l->len;
}

int64_t lume_list_push_f(LumeList *l, double v)
{
    if (!l) return 0;
    if (l->len >= l->cap) list_grow(l);
    int64_t bits;
    memcpy(&bits, &v, sizeof bits);
    l->items[l->len].num = bits;
    l->items[l->len].tag = LUME_SLOT_FLOAT;
    return ++l->len;
}

int64_t lume_list_push_s(LumeList *l, const char *v)
{
    if (!l) return 0;
    if (l->len >= l->cap) list_grow(l);
    l->items[l->len].str = v;
    l->items[l->len].tag = LUME_SLOT_STR;
    return ++l->len;
}

/* Push a nested container. `v` is the lume_list_new() / lume_map_new()
 * pointer, and its own `kind` is what tells a reader how to walk it later, so
 * one function serves both container kinds. */
int64_t lume_list_push_obj(LumeList *l, void *v)
{
    if (!l) return 0;
    if (l->len >= l->cap) list_grow(l);
    l->items[l->len].num = (int64_t)(intptr_t)v;
    l->items[l->len].str = NULL;
    l->items[l->len].tag = LUME_SLOT_OBJ;
    return ++l->len;
}

/* ============ 6. argv() — CLI trailing arguments (SPEC 5) ============ */
/* ---- argv() (SPEC 5): the script's trailing CLI arguments ----
 * The generated entry point calls lume_cli_init once, before any top-level
 * statement runs; lume_bi_argv then hands the stashed list back. Interpreter
 * parity: the list holds strings and is empty when the program received no
 * arguments. The argv[] array the C runtime hands main() lives for the whole
 * process, so push_s can store the pointers without copying. */
static LumeList *lume_argv_cache;
static int lume_argv_ready;

void lume_cli_init(int argc, char **argv)
{
    lume_argv_cache = lume_list_new();
    lume_argv_ready = 1;
    for (int i = 1; i < argc; i++) {
        if (!argv[i]) break;
        lume_list_push_s(lume_argv_cache, argv[i]);
    }
}

void *lume_bi_argv(void)
{
    if (!lume_argv_ready) {          /* embedded path that never ran the entry */
        lume_argv_cache = lume_list_new();
        lume_argv_ready = 1;
    }
    return lume_argv_cache;
}

/* Render one slot the way the interpreter would print it. The interpreter's
 * print() falls back to JSON for anything that is not a plain scalar, so a
 * string inside a list or a map keeps its quotes and escapes — `["a","b"]`,
 * not `[a,b]`.
 *
 * Writes straight to stdout and returns the byte count rather than handing
 * back a string: a nested container (LUME_SLOT_OBJ) is printed by recursing
 * into its own elements, which cannot render into a fixed buffer — the depth
 * is unbounded and two of them can sit side by side in one list. Scalars
 * still go through a static buffer, each emitted before the next is written. */
/* Mutually recursive: a container body holds slots, and a slot may hold a
 * container. Declared before either body so both can call the other. */
static int slot_emit(LumeSlot s);

/* Write a container's body -- the elements between the brackets, no brackets
 * and no trailing newline. Both printers and slot_emit's LUME_SLOT_OBJ branch
 * go through here, so a nested container renders by exactly the same rules as
 * a top-level one and the three cannot drift. The cast to LumeList is safe for
 * both shapes: `kind` is the first int in each and `len`/`cap` follow it. */
/* ============ 7. Value rendering: slot / list -> text & JSON (container_body / slot_emit / jb_*) ============ */
static int container_body(const void *p)
{
    const LumeList *as_list = (const LumeList *)p;
    if (as_list->kind != LUME_CTR_LIST) {
        const LumeMap *m = (const LumeMap *)p;
        int n = 0;
        for (int64_t i = 0; i < m->len; i++) {
            if (i) n += emit(",");
            n += emit("\"");
            n += emit(m->keys[i]);
            n += emit("\":");
            n += slot_emit(m->vals[i]);
        }
        return n;
    }
    int n = 0;
    for (int64_t i = 0; i < as_list->len; i++) {
        if (i) n += emit(",");
        n += slot_emit(as_list->items[i]);
    }
    return n;
}

static int slot_emit(LumeSlot s)
{
    if (s.tag == LUME_SLOT_OBJ) {
        /* The slot holds the container's address; its own `kind` says which
         * of the two container shapes it is, since an i8* does not. The
         * printers below end with a newline, so the brackets are written
         * here -- this is a container *inside* a value, not a whole one. */
        const void *p = (const void *)(intptr_t)s.num;
        if (!p) return emit("null");
        const LumeList *shape = (const LumeList *)p;
        int n = emit(shape->kind == LUME_CTR_LIST ? "[" : "{");
        n += container_body(p);
        return n + emit(shape->kind == LUME_CTR_LIST ? "]" : "}");
    }

    static char buf[1024];
    if (!s.str) s.str = "";
    switch (s.tag) {
    case LUME_SLOT_STR: {
        size_t n = 0;
        buf[0] = '\0';
        buf[n++] = '"';
        for (const unsigned char *p = (const unsigned char *)s.str;
             *p && n + 8 < sizeof buf; p++) {
            switch (*p) {
            case '"':  n += (size_t)snprintf(buf + n, sizeof buf - n, "\\\""); break;
            case '\\': n += (size_t)snprintf(buf + n, sizeof buf - n, "\\\\"); break;
            case '\n': n += (size_t)snprintf(buf + n, sizeof buf - n, "\\n");  break;
            case '\t': n += (size_t)snprintf(buf + n, sizeof buf - n, "\\t");  break;
            case '\r': n += (size_t)snprintf(buf + n, sizeof buf - n, "\\r");  break;
            default:
                if (*p < 0x20)
                    n += (size_t)snprintf(buf + n, sizeof buf - n, "\\u%04x", *p);
                else
                    buf[n++] = (char)*p;
            }
        }
        /* The closing quote has to be NUL-terminated, not just written: emit() is
         * fputs + strlen, so whatever the previous caller left in this static
         * buffer would still be read after it. That went unnoticed while every
         * string in a program was written once per run — the first use of the
         * buffer was always preceded by a longer one filling it, or by nothing
         * at all and a zeroed buffer. Recursion is what made it visible: a
         * nested container is the second string rendered into the same buffer,
         * so `{ a: "AAAAAAAAAAAAAAAA" }` followed by `{ x: { y: "s" } }` printed
         * `{"y":"s"AAAAAAAAAAAAAA}` -- the 16-byte tail of the earlier string,
         * because the short one left the old bytes in place after its quote. */
        size_t end = n < sizeof buf ? n : sizeof buf - 1;
        buf[end] = '"';
        buf[end + 1 < sizeof buf ? end + 1 : end] = '\0';
        return emit(buf);
    }
    case LUME_SLOT_FLOAT: {
        double d = 0;
        memcpy(&d, &s.num, sizeof d);
        snprintf(buf, sizeof buf, "%g", d);
        return emit(buf);
    }
    case LUME_SLOT_BOOL:
        return emit(s.num ? "true" : "false");
    default:              snprintf(buf, sizeof buf, "%lld", (long long)s.num); return emit(buf);
    }
}

/* Serialize a slot to a malloc'd JSON string with the same rendering rules
 * as print() (a string keeps its quotes and escapes, a float prints via %g).
 * Grows a private buffer instead of sharing slot_emit's static one: str()
 * can be called on values of arbitrary depth and size, and the static
 * buffer's documented tail bug (a shorter string leaving the previous one's
 * bytes after its closing quote) becomes a real correctness hazard here.
 * Caller frees the result. */
typedef struct { char *p; size_t len, cap; } JsonBuf;

static void jb_putc(JsonBuf *b, char c)
{
    if (b->len + 1 >= b->cap) {
        b->cap = b->cap ? b->cap * 2 : 64;
        b->p = realloc(b->p, b->cap);
        if (!b->p) { fputs("lume(native): out of memory\n", stderr); exit(70); }
    }
    b->p[b->len++] = c;
}

static void jb_puts(JsonBuf *b, const char *s) { while (*s) jb_putc(b, *s++); }

static void jb_escape(JsonBuf *b, const char *s)
{
    for (const unsigned char *p = (const unsigned char *)s; *p; p++) {
        switch (*p) {
        case '"':  jb_puts(b, "\\\""); break;
        case '\\': jb_puts(b, "\\\\"); break;
        case '\n': jb_puts(b, "\\n");  break;
        case '\t': jb_puts(b, "\\t");  break;
        case '\r': jb_puts(b, "\\r");  break;
        case '\b': jb_puts(b, "\\b");  break;
        case '\f': jb_puts(b, "\\f");  break;
        default:
            if (*p < 0x20) {
                char tmp[8];
                snprintf(tmp, sizeof tmp, "\\u%04x", *p);
                jb_puts(b, tmp);
            } else {
                jb_putc(b, (char)*p);
            }
        }
    }
}

/* Mutually recursive with itself: a slot may hold a container, a container
 * body holds slots. */
static void slot_to_json(JsonBuf *b, LumeSlot s)
{
    switch (s.tag) {
    case LUME_SLOT_OBJ: {
        const void *p = (const void *)(intptr_t)s.num;
        if (!p) { jb_puts(b, "null"); return; }
        const LumeList *shape = (const LumeList *)p;
        if (shape->kind == LUME_CTR_LIST) {
            jb_putc(b, '[');
            for (int64_t i = 0; i < shape->len; i++) {
                if (i) jb_putc(b, ',');
                slot_to_json(b, shape->items[i]);
            }
            jb_putc(b, ']');
        } else {
            const LumeMap *m = (const LumeMap *)p;
            jb_putc(b, '{');
            for (int64_t i = 0; i < m->len; i++) {
                if (i) jb_putc(b, ',');
                jb_putc(b, '"');
                jb_escape(b, m->keys[i]);
                jb_puts(b, "\":");
                slot_to_json(b, m->vals[i]);
            }
            jb_putc(b, '}');
        }
        return;
    }
    case LUME_SLOT_STR:
        jb_putc(b, '"');
        jb_escape(b, s.str ? s.str : "");
        jb_putc(b, '"');
        return;
    case LUME_SLOT_FLOAT: {
        double d = 0;
        memcpy(&d, &s.num, sizeof d);
        char tmp[32];
        snprintf(tmp, sizeof tmp, "%g", d);
        jb_puts(b, tmp);
        return;
    }
    case LUME_SLOT_BOOL:
        jb_puts(b, s.num ? "true" : "false");
        return;
    default: {
        char tmp[32];
        snprintf(tmp, sizeof tmp, "%lld", (long long)s.num);
        jb_puts(b, tmp);
        return;
    }
    }
}

/* Entry point for str() on containers: `obj` is the LumeMap or LumeList
 * pointer the emitted IR passes as i8*. Returns a malloc'd JSON string
 * (never NULL; a null pointer serializes as "null"). Caller frees. */
char *lume_rt_to_json_obj(const void *obj)
{
    JsonBuf b = { 0, 0, 0 };
    LumeSlot s;
    memset(&s, 0, sizeof s);
    s.tag = LUME_SLOT_OBJ;
    s.num = (int64_t)(intptr_t)obj;
    slot_to_json(&b, s);
    jb_putc(&b, '\0');
    return b.p;
}

/* Entry point for stringify() on a string: the interpreter's json_append_value
 * quotes it (stringify("x") is "\"x\""), so the native flavour must too.
 * Same escape set as jb_escape, wrapped in double quotes. Caller frees. */
char *lume_rt_quote_str(const char *s)
{
    JsonBuf b = { 0, 0, 0 };
    jb_putc(&b, '"');
    if (s) jb_escape(&b, s);
    jb_putc(&b, '"');
    jb_putc(&b, '\0');
    return b.p;
}

/* Element access for the three statically known element types — the call
 * site picks the flavour, and a string slot read through the int helper
 * reads 0 rather than a dangling pointer. */
int64_t lume_list_at_i(const LumeList *l, int64_t i)
{
    if (!l || i < 0 || i >= l->len) return 0;
    return l->items[i].tag == LUME_SLOT_STR ? 0 : l->items[i].num;
}

double lume_list_at_f(const LumeList *l, int64_t i)
{
    double d = 0;
    if (!l || i < 0 || i >= l->len) return d;
    if (l->items[i].tag == LUME_SLOT_STR) return d;
    /* lume_range pushes a whole value as an INT slot even in a float-arg
     * range (the interpreter's rule), so the float reader widens it instead
     * of reinterpreting the integer as a bit pattern. */
    if (l->items[i].tag == LUME_SLOT_INT) return (double)l->items[i].num;
    memcpy(&d, &l->items[i].num, sizeof d);
    return d;
}

const char *lume_list_at_s(const LumeList *l, int64_t i)
{
    if (!l || i < 0 || i >= l->len) return "";
    return l->items[i].tag == LUME_SLOT_STR ? l->items[i].str : "";
}

/* Read a nested container back out of a list -- the counterpart of
 * lume_map_get_obj, and the reason `for (let row in [[1],[2]])` has a way to
 * see the inner lists. Null for an out-of-range index or a scalar element. */
void *lume_list_at_obj(const LumeList *l, int64_t i)
{
    if (!l || i < 0 || i >= l->len || l->items[i].tag != LUME_SLOT_OBJ) return NULL;
    return (void *)(intptr_t)l->items[i].num;
}

/* Read back a boxed LumeSlot for ANY element kind (null/int/float/str/bool/
 * obj), used by `for (x in xs)` when xs decayed to list<any> (e.g. [null, 2,
 * null]). Contrast with lume_list_at_obj, which only returns non-NULL for
 * LUME_SLOT_OBJ — an any-typed iteration must see every element, so it cannot
 * use that accessor. Returns NULL for an out-of-range index. The returned
 * pointer is into the list's own items array and stays valid while the list
 * lives (the whole iteration). */
void *lume_list_at_slot(const LumeList *l, int64_t i)
{
    if (!l || i < 0 || i >= l->len) return NULL;
    return (void *)&l->items[i];
}

/* Convert one list element to the text sqlite wants (native sql builtins
 * in bridge_native.c): strings pass through; int/float render like the
 * interpreter's str(); bool renders 1/0; null (and containers) become
 * NULL — the "bind SQL NULL" sentinel, exactly like the interpreter's
 * sql_arg_bind. One static buffer is enough: the caller binds each
 * parameter before the next call. */
const char *lume_list_param_cstr(const LumeList *l, int64_t i)
{
    if (!l || i < 0 || i >= l->len) return NULL;
    const LumeSlot *s = &l->items[i];
    switch (s->tag) {
    case LUME_SLOT_STR:
        return s->str ? s->str : "";
    case LUME_SLOT_INT: {
        static char b[32];
        snprintf(b, sizeof b, "%lld", (long long)s->num);
        return b;
    }
    case LUME_SLOT_FLOAT: {
        static char b[48];
        double d;
        memcpy(&d, &s->num, sizeof d);
        snprintf(b, sizeof b, "%g", d);
        return b;
    }
    case LUME_SLOT_BOOL:
        return s->num ? "1" : "0";
    default:
        return NULL;                    /* null / container */
    }
}

/* Print a single boxed value the way the interpreter would — used by
 * print(x) when x is an `any`-typed value (a for-in loop variable, a
 * get()/index read off a json() tree or a str_match row). NULL prints the
 * bare word "null" (matching lume_print_null and the interpreter).
 * Reuses slot_emit(), which renders one LumeSlot by its tag (scalars,
 * escaped strings, and nested containers alike), so native and
 * interpreter agree byte-for-byte.
 *
 * A top-level *string* slot is the one place slot_emit's quoting is
 * wrong for print(): the interpreter's print() of a string value writes
 * the bare string, and only a string *inside* a container keeps its
 * quotes (that is what slot_to_json / container_body implement). Going
 * through slot_emit here printed `"12"` for `print(get(m, "text"))`
 * where the interpreter printed `12` — the same rule str() already
 * applies through lume_slot_str_scalar. */
int64_t lume_print_slot(const LumeSlot *s)
{
    if (!s) return emit("null\n");
    if (s->tag == LUME_SLOT_STR)
        return emit(s->str ? s->str : "") + emit("\n");
    int n = slot_emit(*s);
    return n + emit("\n");
}

int64_t lume_list_print(const LumeList *l)
{
    /* A null list value (a null element read back, or `let l: list = null`)
     * prints the bare word the interpreter prints for null -- not "[]", which
     * is what a real, allocated empty list prints. */
    if (!l) return emit("null\n");
    /* An opaque i8* that is really a string (e.g. the result of a map read)
     * reaches here with a bogus kind; dereferencing its slots would crash.
     * The interpreter tags values, the native path does not, so it can only
     * tell a container from a string by this field. Anything that is not a
     * list is not printable this way (SPEC 6.2). */
    if (l->kind != LUME_CTR_LIST) { fputs("(opaque value)\n", stdout); return 0; }
    return emit("[") + container_body(l) + emit("]\n");
}


/* ============ 8. Map runtime: grow / find ============ */
/* ---------------------------------------------------------------- maps --- */

static void map_grow(LumeMap *m)
{
    int64_t cap = m->cap ? m->cap * 2 : 8;
    char **ks = realloc(m->keys, (size_t)cap * sizeof *ks);
    if (!ks) { fputs("lume(native): out of memory\n", stderr); exit(70); }
    memset(ks + (size_t)m->cap, 0, (size_t)(cap - m->cap) * sizeof *ks);
    LumeSlot *vs = realloc(m->vals, (size_t)cap * sizeof *vs);
    if (!vs) { fputs("lume(native): out of memory\n", stderr); exit(70); }
    memset(vs + (size_t)m->cap, 0, (size_t)(cap - m->cap) * sizeof *vs);
    m->keys = ks;
    m->vals = vs;
    m->cap  = cap;
}

static int64_t map_find(const LumeMap *m, const char *k)
{
    if (!m || !k) return -1;
    for (int64_t i = 0; i < m->len; i++)
        if (m->keys[i] && strcmp(m->keys[i], k) == 0) return i;
    return -1;
}

LumeMap *lume_map_new(void)
{
    LumeMap *m = rt_alloc(sizeof(LumeMap));
    m->kind = LUME_CTR_MAP;
    return m;
}

/* `{ m with k: v, ... }` — the native half of the structure update: a fresh
 * map holding the same keys and slots. Slots are shared verbatim, so a nested
 * container stays aliased between the original and the update, exactly like
 * the interpreter's shallow copy (value.c) and Lean's functional update. The
 * caller then overwrites the updated keys with the usual lume_map_put_*. */
LumeMap *lume_map_clone(const LumeMap *src)
{
    if (!src) return lume_map_new();
    LumeMap *m = rt_alloc(sizeof(LumeMap));
    m->kind = LUME_CTR_MAP;
    m->len = m->cap = src->len;
    if (m->cap) {
        m->keys = malloc((size_t)m->cap * sizeof *m->keys);
        m->vals = malloc((size_t)m->cap * sizeof *m->vals);
        for (int64_t i = 0; i < src->len; i++) {
            m->keys[i] = dup_str(src->keys[i]);
            m->vals[i] = src->vals[i];
        }
    }
    return m;
}

/* ---------- strict accessors for the index syntax (SPEC 6.2) ----------
 *
 * `l[0]` / `m["k"]` are strict: an out-of-range index or a missing key is a
 * runtime error, where the existing lume_list_at_* / lume_map_get_* helpers
 * answer 0 / a default. Those stay as they are because `el()` and `get()` are
 * specified in terms of them and quietly tightening them would change every
 * existing caller. These are for the syntax instead.
 *
 * A negative index counts from the end, so l[-1] is the last element --
 * resolved here once, for both native backends, rather than in each of them.
 *
 * The numeric flavours report failure out of band, via lume_index_failed(),
 * because a stored 0 is a legal value and cannot be told from a failed read by
 * the return alone. The string flavours take an `ok` out-param instead, since a
 * string has no sentinel of its own. */
static int g_index_failed = 0;
/* Distinguishes *why* an opaque (`i8*`) index read failed, so the message can
 * name the real cause instead of a misleading one. The `m["k"]` syntax lowers
 * every map read to lume_index_ptr (the i8* accessor), which only carries
 * string and container values -- a scalar (int / float / bool) value cannot
 * travel through it. Such a read is *not* "no such key": the key is present,
 * the value is just a scalar the pointer accessor cannot return. The language
 * documents binding `m["k"]` to a typed variable or using get(m, k) for that,
 * so the failure reason steers the user there rather than at a missing key.
 *   0 = key absent / index out of range (the generic `what` message applies)
 *   1 = the value is a scalar the pointer accessor cannot return */
static int g_index_fail_kind = 0;

/* try(fn) support (SPEC 6.3). A compiled program normally has no error channel
 * back to a caller -- a failed index read prints and exits, exactly like every
 * other fatal runtime condition. Inside try() the runtime records the first
 * recoverable error in a slot instead, and the IR reads it back after the call
 * to build the { ok, err } map the interpreter produces. Outside try() the
 * behaviour is unchanged (lume_error_set still prints and exits). */
static char g_native_err[512];
static int  g_in_try = 0;

void lume_error_begin(void) { g_in_try = 1; g_native_err[0] = '\0'; }
const char *lume_error_end(void) { g_in_try = 0; return g_native_err[0] ? g_native_err : NULL; }
/* ============ 9. Error handling & list sort ============ */
void lume_error_set(const char *msg)
{
    if (g_in_try) {
        if (!msg) { g_native_err[0] = '\0'; return; }
        size_t n = strlen(msg);
        if (n > sizeof g_native_err - 1) n = sizeof g_native_err - 1;
        memcpy(g_native_err, msg, n);
        g_native_err[n] = '\0';
    } else {
        fprintf(stderr, "lume(native): %s\n", msg ? msg : "runtime error");
        exit(1);
    }
}

/* sort(l) — in-place, returns l (SPEC collection tools). The interpreter's
 * native_sort (builtins.c) requires a homogeneous list — all int (exact i64
 * compare), all float (double), or all string (strcmp byte order) — and
 * rejects bools, containers and mixes; empty and single-element lists are
 * already sorted and skip the check. The native backends' slots carry their
 * tag, so this helper applies the same rule by scanning the slots and uses
 * the same comparators, with the same two error wordings. */
static int cmp_sort_slot_i(const void *a, const void *b)
{
    const LumeSlot *sa = (const LumeSlot *)a, *sb = (const LumeSlot *)b;
    return (sa->num > sb->num) - (sa->num < sb->num);
}
static int cmp_sort_slot_f(const void *a, const void *b)
{
    const LumeSlot *sa = (const LumeSlot *)a, *sb = (const LumeSlot *)b;
    double da, db;
    memcpy(&da, &sa->num, sizeof da);
    memcpy(&db, &sb->num, sizeof db);
    return (da > db) - (da < db);
}
static int cmp_sort_slot_s(const void *a, const void *b)
{
    const LumeSlot *sa = (const LumeSlot *)a, *sb = (const LumeSlot *)b;
    return strcmp(sa->str ? sa->str : "", sb->str ? sb->str : "");
}
void lume_list_sort(LumeList *l)
{
    if (!l || l->len <= 1) return;   /* empty / single: already sorted */
    int kind = -1, bad = 0;
    for (int64_t i = 0; i < l->len; i++) {
        int k;
        switch (l->items[i].tag) {
        case LUME_SLOT_INT:   k = 0; break;
        case LUME_SLOT_FLOAT: k = 1; break;
        case LUME_SLOT_STR:   k = 2; break;
        default:              k = 3; break;   /* bool / container / null */
        }
        if (k == 3) { bad = 1; break; }       /* not a number or string */
        if (kind == -1) kind = k;
        else if (kind != k) { bad = 2; break; }  /* mixed number kinds */
    }
    if (bad == 1)
        lume_error_set("sort() needs a list of numbers or strings");
    else if (bad == 2)
        lume_error_set("sort() needs a homogeneous list "
                       "(all numbers or all strings)");
    if (bad) return;
    if (kind == 0)
        qsort(l->items, (size_t)l->len, sizeof *l->items, cmp_sort_slot_i);
    else if (kind == 1)
        qsort(l->items, (size_t)l->len, sizeof *l->items, cmp_sort_slot_f);
    else
        qsort(l->items, (size_t)l->len, sizeof *l->items, cmp_sort_slot_s);
}

int lume_index_failed(void) { return g_index_failed; }

/* Report a failed index read. Inside try() this records the message and
 * returns -- the IR that called it falls through to the value the strict
 * accessor already returned (0 / NULL) and the try wrapper builds { ok: null,
 * err: msg }. Outside try() it prints and exits, which is what every other
 * fatal runtime condition in the native path does. The interpreter reports
 * the same "index read failed" text (interp.c) so try() captures an identical
 * err string on every backend (SPEC 6.2). */
/* ============ 10. Indexing & container type predicates ============ */
void lume_index_error(const char *what)
{
    if (g_in_try) {
        if (g_index_fail_kind == 1)
            lume_error_set("map value at this key is a scalar; "
                           "use get(m, k, d) to read it -- typed binding via "
                           "m[\"k\"] only works for string / container values");
        else
            lume_error_set(what ? what : "index read failed");
        return;
    }
    if (g_index_fail_kind == 1)
        fputs("lume(native): map value at this key is a scalar; "
              "use get(m, k, d) to read it -- typed binding via m[\"k\"] only "
              "works for string / container values\n",
              stderr);
    else
        fprintf(stderr, "lume(native): %s\n", what ? what : "index read failed");
    exit(1);
}

int64_t lume_list_at_strict(const LumeList *l, int64_t i)
{
    g_index_failed = 0;
    if (!l) { g_index_failed = 1; return 0; }
    if (i < 0) i += l->len;
    if (i < 0 || i >= l->len) { g_index_failed = 1; return 0; }
    return l->items[i].tag == LUME_SLOT_STR ? 0 : l->items[i].num;
}

double lume_list_at_strict_f(const LumeList *l, int64_t i)
{
    g_index_failed = 0;
    if (!l) { g_index_failed = 1; return 0; }
    if (i < 0) i += l->len;
    if (i < 0 || i >= l->len) { g_index_failed = 1; return 0; }
    double d = 0;
    if (l->items[i].tag != LUME_SLOT_STR) memcpy(&d, &l->items[i].num, sizeof d);
    return d;
}

/* An element that travels as an i8* -- a string or another container -- cannot
 * be told apart by the IR type, so the accessor dispatches on the slot tag at
 * runtime and returns the pointer either way. A string slot yields its char*;
 * an object slot yields the container; any other slot is a type mismatch and
 * fails. That is what lets `m["k"]` work without the emitter knowing what the
 * map holds, which it cannot know. */
void *lume_list_at_strict_ptr(const LumeList *l, int64_t i)
{
    g_index_failed = 0;
    if (!l) { g_index_failed = 1; return NULL; }
    if (i < 0) i += l->len;
    if (i < 0 || i >= l->len) { g_index_failed = 1; return NULL; }
    LumeSlot sl = l->items[i];
    if (sl.tag == LUME_SLOT_STR) return (void *)(intptr_t)sl.str;
    if (sl.tag == LUME_SLOT_OBJ) return (void *)(intptr_t)sl.num;
    g_index_failed = 1;
    return NULL;
}

/* A string element has no sentinel, so `ok` reports whether one was there. */
const char *lume_list_at_strict_str(const LumeList *l, int64_t i, int *ok)
{
    *ok = 0;
    if (!l) return NULL;
    if (i < 0) i += l->len;
    if (i < 0 || i >= l->len) return NULL;
    if (l->items[i].tag != LUME_SLOT_STR) return NULL;
    *ok = 1;
    return l->items[i].str;
}

int64_t lume_map_at_strict_i(const LumeMap *m, const char *k)
{
    g_index_failed = 0;
    int64_t i = map_find(m, k);
    if (i < 0 || m->vals[i].tag == LUME_SLOT_STR) { g_index_failed = 1; return 0; }
    return m->vals[i].num;
}

double lume_map_at_strict_f(const LumeMap *m, const char *k)
{
    g_index_failed = 0;
    int64_t i = map_find(m, k);
    if (i < 0 || m->vals[i].tag == LUME_SLOT_STR) { g_index_failed = 1; return 0; }
    double d = 0;
    memcpy(&d, &m->vals[i].num, sizeof d);
    return d;
}

/* Same tag dispatch as lume_list_at_strict_ptr(): a key whose value is a
 * string and a key whose value is a container both come back as the i8* the IR
 * already has, and the caller cannot (and need not) tell them apart. */
void *lume_map_at_strict_ptr(const LumeMap *m, const char *k)
{
    g_index_failed = 0;
    g_index_fail_kind = 0;
    int64_t i = map_find(m, k);
    if (i < 0) { g_index_failed = 1; return NULL; }
    LumeSlot sv = m->vals[i];
    if (sv.tag == LUME_SLOT_STR) return (void *)(intptr_t)sv.str;
    if (sv.tag == LUME_SLOT_OBJ) return (void *)(intptr_t)sv.num;
    /* A scalar (int / float / bool) value cannot travel through the i8*
     * pointer accessor the syntax always lowers to. The key exists; the
     * value just is not one this accessor can hand back. Flag it so the
     * message points at get() / a typed binding instead of "no such key". */
    g_index_failed = 1;
    g_index_fail_kind = 1;
    return NULL;
}

const char *lume_map_at_strict_str(const LumeMap *m, const char *k, int *ok)
{
    *ok = 0;
    int64_t i = map_find(m, k);
    if (i < 0 || m->vals[i].tag != LUME_SLOT_STR) return NULL;
    *ok = 1;
    return m->vals[i].str;
}

/* ---------- unified index entry (SPEC 6.2) ----------
 *
 * Both emitters lower `m["k"]` and `l[0]` through these, for one reason: the
 * emitter often cannot know which of the two it is looking at. `m["p"][1]`
 * reads a container out of a map and what comes back is an opaque i8* -- the
 * same pointer a string is -- so there is nothing in the IR to dispatch on.
 *
 * The container knows, though: `kind` sits at the same offset in LumeList and
 * LumeMap, so reading one field off the pointer answers it. That branch lives
 * here, once, rather than being duplicated across two emitters and guessed at
 * each one.
 *
 * Each sets g_index_failed when the read did not happen; the caller checks the
 * flag and calls lume_index_error(). A negative list index counts from the end.
 */

/* The boxed shape of null: an OBJ slot with no payload. Every "absent"
 * answer in the slot readers hands one back, so print() renders the bare
 * word "null" the way the interpreter's null does — a NULL pointer would
 * print the same word but could not be told from a failed read by a
 * caller that wants to keep going (the emitters' strictness check reads
 * g_index_failed, not the pointer). */
static LumeSlot *null_slot_box(void)
{
    LumeSlot *box = rt_alloc(sizeof(LumeSlot));
    memset(box, 0, sizeof *box);
    box->tag = LUME_SLOT_OBJ;
    return box;
}

int lume_is_map(void *c)
{
    return c && ((const LumeList *)c)->kind == LUME_CTR_MAP;
}

int lume_is_list(void *c)
{
    return c && ((const LumeList *)c)->kind == LUME_CTR_LIST;
}

/* The key travels as its own argument in the shape its container needs: an i64
 * for a list index (so `l[0]` emits `i64 0`, not `i8* 0` -- an integer constant
 * spelled as a pointer is rejected by clang), and a char* for a map key. Both are
 * always passed; the unused one is ignored, which is cheaper than two entry
 * points per shape. */
int64_t lume_index_int(void *c, int64_t idx, const char *key)
{
    g_index_failed = 0;
    if (!c) { g_index_failed = 1; return 0; }
    if (((const LumeList *)c)->kind == LUME_CTR_LIST)
        return lume_list_at_strict((const LumeList *)c, idx);
    return lume_map_at_strict_i((const LumeMap *)c, key);
}

double lume_index_float(void *c, int64_t idx, const char *key)
{
    g_index_failed = 0;
    if (!c) { g_index_failed = 1; return 0; }
    if (((const LumeList *)c)->kind == LUME_CTR_LIST)
        return lume_list_at_strict_f((const LumeList *)c, idx);
    return lume_map_at_strict_f((const LumeMap *)c, key);
}

/* The i8* result case: a string element and a nested container are the same
 * pointer in IR, so this dispatches on the slot tag and hands back whichever it
 * is. */
void *lume_index_ptr(void *c, int64_t idx, const char *key)
{
    g_index_failed = 0;
    if (!c) { g_index_failed = 1; return NULL; }
    if (((const LumeList *)c)->kind == LUME_CTR_LIST)
        return lume_list_at_strict_ptr((const LumeList *)c, idx);
    return lume_map_at_strict_ptr((const LumeMap *)c, key);
}

/* The slot-flavoured sibling: `l[0]` / `m["k"]` whose result the emitter
 * types as `any` — a list literal whose elements share no static shape
 * (`[{"a":1},{"a":2}]`), so list_at_fn has no accessor for them and the
 * int/pointer flavours would read the element as one scalar kind: an int
 * read answers 0 for a string field and the raw pointer for a container
 * one, neither of which is the value. This hands back a boxed copy of the
 * slot itself, whatever it holds — exactly what a for-in over the same
 * list gives its loop variable (lume_list_at_slot), and what every
 * downstream reader of an `any` (print, len, get, ==, str) expects.
 *
 * The text emitter used to route an any element through lume_index_int
 * here and then pass the i64 result where an i8* was declared, which
 * assembled into reading the element's integer payload as a pointer: the
 * libLLVM emitter picked lume_index_ptr and got away with the raw
 * container only because the slot readers below tolerate one. An absent
 * element sets the failure flag like its siblings and answers the null
 * slot. */
void *lume_index_slot(void *c, int64_t idx, const char *key)
{
    g_index_failed = 0;
    if (!c) { g_index_failed = 1; return null_slot_box(); }
    if (((const LumeList *)c)->kind == LUME_CTR_LIST) {
        const LumeList *l = (const LumeList *)c;
        if (idx < 0) idx += l->len;
        if (idx < 0 || idx >= l->len) { g_index_failed = 1; return null_slot_box(); }
        LumeSlot *box = rt_alloc(sizeof(LumeSlot));
        *box = l->items[idx];
        return box;
    }
    const LumeMap *m = (const LumeMap *)c;
    int64_t i = map_find(m, key);
    if (i < 0) { g_index_failed = 1; return null_slot_box(); }
    LumeSlot *box = rt_alloc(sizeof(LumeSlot));
    *box = m->vals[i];
    return box;
}


int64_t lume_map_len(const LumeMap *m) { return m ? m->len : 0; }

/* ============ 11. Map put / get / accumulate ============ */
static int64_t map_put(LumeMap *m, const char *k, int64_t num, const char *str, int tag)
{
    if (!m) return 0;
    int64_t i = map_find(m, k);
    if (i < 0) {
        if (m->len >= m->cap) map_grow(m);
        i = m->len++;
        /* Key ownership: the map keeps its own copy, because a key that came
         * from a literal is a global constant but one that came from a
         * `put(m, expr, v)` call may be a temporary the IR drops. */
        m->keys[i] = dup_str(k);
    }
    m->vals[i].num = num;
    m->vals[i].str = str;
    m->vals[i].tag = tag;
    return m->len;
}

int64_t lume_map_put_i(LumeMap *m, const char *k, int64_t v) { return map_put(m, k, v, NULL, LUME_SLOT_INT); }
int64_t lume_map_put_b(LumeMap *m, const char *k, int64_t v) { return map_put(m, k, v ? 1 : 0, NULL, LUME_SLOT_BOOL); }
static double slot_float_of(LumeSlot s); /* forward decl (defined in the json parser below) */

int64_t lume_map_put_f(LumeMap *m, const char *k, double v)
{
    int64_t bits;
    memcpy(&bits, &v, sizeof bits);
    return map_put(m, k, bits, NULL, LUME_SLOT_FLOAT);
}
int64_t lume_map_put_s(LumeMap *m, const char *k, const char *v) { return map_put(m, k, 0, v, LUME_SLOT_STR); }

/* Store a nested container as a value -- the map half of lume_list_push_obj.
 * `map_put` is the shared writer; all it needs is the object tag and the
 * pointer in the num field. */
int64_t lume_map_put_obj(LumeMap *m, const char *k, void *v)
{
    return map_put(m, k, (int64_t)(intptr_t)v, NULL, LUME_SLOT_OBJ);
}

/* Store a tagged-any slot's payload as a map field — the ok half of
 * try(fn) when fn returns any. The slot's own tag picks the writer
 * (int/float/string/bool payloads land as scalars, container payloads
 * as obj pointers), mirroring the json object reader's dispatch below
 * (json_value:1326-1332), so the resulting map prints exactly like the
 * interpreter's { ok: <value> }. A null slot, or an OBJ slot with a
 * null payload, stores obj-null. */
int64_t lume_map_put_slot(LumeMap *m, const char *k, const LumeSlot *s)
{
    if (!s) return lume_map_put_obj(m, k, NULL);
    switch (s->tag) {
    case LUME_SLOT_INT:   return lume_map_put_i(m, k, s->num);
    case LUME_SLOT_FLOAT: return lume_map_put_f(m, k, slot_float_of(*s));
    case LUME_SLOT_STR:   return lume_map_put_s(m, k, s->str ? s->str : "");
    case LUME_SLOT_BOOL:  return lume_map_put_b(m, k, s->num);
    case LUME_SLOT_OBJ:   return lume_map_put_obj(m, k, (void *)(intptr_t)s->num);
    default:              return lume_map_put_obj(m, k, NULL);
    }
}


/* get() with the interpreter's default: an absent key yields `dflt`. */
int64_t lume_map_get_i(const LumeMap *m, const char *k, int64_t dflt)
{
    int64_t i = map_find(m, k);
    if (i < 0) return dflt;
    return m->vals[i].tag == LUME_SLOT_STR ? 0 : m->vals[i].num;
}
/* Read a nested container back out. Returns null for an absent key or one that
 * does not hold a container -- the caller hands the result straight to another
 * container helper or to a printer, both of which take a null pointer as an
 * empty container. This is what makes `get(m, k, { x: 1 })` and a nested read
 * work: the getter has to give back a pointer, not the int/float/string the
 * other three return.
 *
 * The third parameter exists only so the call site can pass a default the way
 * it does for every other flavour (get() picks its getter from the default's
 * type). A container default is never stored -- an absent key reads as null,
 * which the container helpers already treat as empty. */
void *lume_map_get_obj(const LumeMap *m, const char *k, void *dflt)
{
    (void)dflt;
    int64_t i = map_find(m, k);
    if (i < 0 || m->vals[i].tag != LUME_SLOT_OBJ) return NULL;
    return (void *)(intptr_t)m->vals[i].num;
}

/* try()'s err slot: a string when a recoverable error fired, a null obj slot
 * when it did not. Neither slot flavour alone can serve both states, so this
 * reads either: the string itself or the obj payload (NULL for null). Scalars
 * cannot appear in the slot the type checker gives it. */
void *lume_map_get_any(const LumeMap *m, const char *k, void *dflt)
{
    int64_t i = map_find(m, k);
    if (i < 0) return dflt;
    LumeSlot s = m->vals[i];
    if (s.tag == LUME_SLOT_STR) return (void *)s.str;
    if (s.tag == LUME_SLOT_OBJ) return (void *)(intptr_t)s.num;
    return NULL;
}

/* get(m, k) with no default on a map whose static shape declares no
 * common value type — a str_match row, a json-parsed row, a map literal
 * the scan side could not type. This is the interpreter's dynamic read:
 * the value comes back as whatever the cell holds, which for the native
 * side means a boxed copy of the slot, the same box lume_slot_get_any
 * hands back for an any receiver, so every downstream reader (print,
 * len, get, ==, str) treats the two identically. The typed getters above
 * would answer 0 for a string field and the raw pointer for a container
 * one — `get(str_match("[0-9]+", "a12")[0], "text")` read 0 where the
 * interpreter read "12".
 *
 * An absent key reads as the null slot, which is what the interpreter
 * answers for it (the typed getters answer their flavour's zero, the
 * documented phase-1 difference a three-argument default exists to
 * close). The third parameter exists only so the call site can pass a
 * default the way it does for every other flavour; it is ignored here. */
void *lume_map_get_slot(const LumeMap *m, const char *k, void *dflt)
{
    (void)dflt;
    int64_t i = map_find(m, k);
    if (i < 0) return null_slot_box();
    LumeSlot *box = rt_alloc(sizeof(LumeSlot));
    *box = m->vals[i];
    return box;
}

/* ---- cast accessors: int(m,"k"[,d]) / float(m,"k"[,d]) / str(m,"k"[,d]) /
 * bool(m,"k"[,d]) --
 * The interpreter's value_from_map + native_int / native_float /
 * native_str / native_bool (src/builtins.c). The default applies to an
 * ABSENT key and -- unlike the direct casts -- a found string that does
 * not whole-string parse also falls back to it ("12x" -> dflt, "42" ->
 * 42). A found number converts the way the cast converts; a found BOOL
 * falls back to the default for int/float (the interpreter's IS_NUM is
 * int|float only, so bool lands in native_int's final else) and takes
 * its own truthiness for bool(); a found container falls back for
 * int/float and JSON-renders for str (native_str -> str_of_value). The
 * accessor's answer is always the cast's own type no matter what the
 * field holds, which is why these exist next to the typed getters
 * above: no single lume_map_get_* can express it. */
int64_t lume_bi_bool_obj(const void *p); /* defined with the bool builtins below */
int64_t lume_map_acc_i(const LumeMap *m, const char *k, int64_t dflt)
{
    int64_t i = map_find(m, k);
    if (i < 0) return dflt;
    switch (m->vals[i].tag) {
    case LUME_SLOT_INT:   return m->vals[i].num;
    case LUME_SLOT_STR: {
        const char *s = m->vals[i].str;
        char *end = NULL;
        if (s && *s) {
            long long v = strtoll(s, &end, 10);
            if (end && end != s && *end == '\0') return (int64_t)v;
        }
        return dflt;
    }
    case LUME_SLOT_FLOAT: return (int64_t)slot_float_of(m->vals[i]);
    default:              return dflt;   /* BOOL, null and containers: the interpreter's else */
    }
}

double lume_map_acc_f(const LumeMap *m, const char *k, double dflt)
{
    int64_t i = map_find(m, k);
    if (i < 0) return dflt;
    switch (m->vals[i].tag) {
    case LUME_SLOT_INT:   return (double)m->vals[i].num;
    case LUME_SLOT_STR: {
        const char *s = m->vals[i].str;
        char *end = NULL;
        if (s && *s) {
            double v = strtod(s, &end);
            if (end && end != s && *end == '\0') return v;
        }
        return dflt;
    }
    case LUME_SLOT_FLOAT: return slot_float_of(m->vals[i]);
    default:              return dflt;   /* BOOL, null and containers */
    }
}

/* str(m, "k"[, d]): a found scalar serializes exactly as str() of that
 * value would (the interpreter's native_str -> str_of_value); a found
 * container JSON-renders (str_of_value -> json_append_value), which is
 * the one shape the typed getters never served — lume_rt_to_json_obj
 * renders it here with print()'s rules, the same bytes the three
 * backends were swept to agree on. A null is an OBJ slot with no
 * payload and renders the bare word "null".
 *
 * The default arrives pre-serialized: the emitters run the default
 * expression through the same str() conversion the direct call would
 * use, so this always receives text. A NULL default means the
 * two-argument form, whose absent-key answer is the interpreter's
 * str_of_value(null) — the word "null", not empty text. */
char *lume_map_acc_s(const LumeMap *m, const char *k, const char *dflt)
{
    int64_t i = map_find(m, k);
    if (i < 0) return dup_str(dflt ? dflt : "null");
    switch (m->vals[i].tag) {
    case LUME_SLOT_STR:   return dup_str(m->vals[i].str ? m->vals[i].str : "");
    case LUME_SLOT_INT:   return lume_bi_str_i64(m->vals[i].num);
    case LUME_SLOT_FLOAT: return lume_bi_str_double(slot_float_of(m->vals[i]));
    case LUME_SLOT_BOOL:  return lume_bi_str_bool(m->vals[i].num);
    case LUME_SLOT_OBJ:
        return m->vals[i].num
            ? lume_rt_to_json_obj((const void *)(intptr_t)m->vals[i].num)
            : dup_str("null");
    default:              return dup_str(dflt ? dflt : "null");
    }
}

/* bool(m, "k"[, d]): the interpreter's native_bool -> value_truthy
 * (value.c) of the found value, or of the default when the key is
 * absent. Truthiness is defined for every tag — non-empty string,
 * non-zero number, the bool itself, a non-empty container, null false
 * — so unlike the numeric accessors there is no conversion and no
 * fallback: only the found/absent split. */
int64_t lume_map_acc_b(const LumeMap *m, const char *k, int64_t dflt)
{
    int64_t i = map_find(m, k);
    if (i < 0) return dflt;
    switch (m->vals[i].tag) {
    case LUME_SLOT_STR:   return m->vals[i].str && *m->vals[i].str;
    case LUME_SLOT_BOOL:  return m->vals[i].num != 0;
    case LUME_SLOT_FLOAT: return slot_float_of(m->vals[i]) != 0.0;
    case LUME_SLOT_OBJ:   return lume_bi_bool_obj((const void *)(intptr_t)m->vals[i].num);
    default:              return m->vals[i].num != 0;   /* INT */
    }
}

/* Does this Result hold an error? `?` branches on it right after the call and
 * before the payload is read, which is the order the interpreter uses
 * (interp.c's propagate branch looks for "err" first and only then reads "ok").
 * A Result with neither key -- a callee typed any -- counts as no error, so a
 * loose call keeps working rather than propagating something that is not there. */
int64_t lume_map_has(const LumeMap *m, const char *k)
{
    return map_find(m, k) >= 0;
}

/* Content string equality for the emitters. The interpreter's `==` walks
 * values_equal (strcmp on the bytes), but the emitters spell `==` as an
 * icmp on the two i8* pointers, which only ever agrees by pointer-luck.
 * match's tag dispatch needs the real thing, so it calls this. */
/* ============ 12. Value equality: slot / value / any ============ */
int64_t lume_str_eq(const char *a, const char *b)
{
    if (a == b) return 1;
    if (!a || !b) return 0;
    return strcmp(a, b) == 0;
}

/* ---- structural equality (SPEC 1.8) ------------------------------------
 * The emitters spell container `==` as a call to lume_value_eq. Before this
 * it was an icmp on the two i8* pointers, which answers false for two maps
 * that hold the same keys — `[1,2] == [1,2]` was false on every backend.
 * LumeList and LumeMap both start with `int64_t len, cap;` and keep their kind
 * tag at the same offset, so one probe tells them apart. */

/* The `kind` field sits after two longs in both container headers. Reading
 * it is only sound for a real container or a dup_str'd string (which is
 * zero-padded to at least 32 bytes, see dup_str) — the emitters only route
 * values here whose static type is a container, or an any that came out of
 * the runtime's own heap. */
static int ctr_kind(const void *p)
{
    if (!p) return -1;
    return *(const int *)((const char *)p + 2 * sizeof(int64_t));
}

/* Is this pointer a boxed LumeSlot* rather than a raw container or a
 * string? A slot's `tag` and a container's `kind` sit at the same offset
 * — both follow two int64 fields — and the two value sets are disjoint
 * (slot tags are 0..4, container kinds 'LI'/'MA'), so one read tells them
 * apart. An `any` value travels either way: json(), for-in and get-on-any
 * hand back a box, an index read hands back the raw container, and
 * lume_any_eq has to compare across both. */
static int ptr_is_slot(const void *p)
{
    if (!p) return 0;
    int t = *(const int *)((const char *)p + 2 * sizeof(int64_t));
    return t == LUME_SLOT_INT || t == LUME_SLOT_FLOAT || t == LUME_SLOT_STR ||
           t == LUME_SLOT_BOOL || t == LUME_SLOT_OBJ;
}

static int is_ctr_kind(int k)
{
    return k == LUME_CTR_LIST || k == LUME_CTR_MAP;
}

int64_t lume_value_eq(void *a, void *b);   /* defined below; slot_eq recurses */

static int64_t slot_eq(const LumeSlot *x, const LumeSlot *y)
{
    if (x->tag != y->tag) return 0;
    switch (x->tag) {
    case LUME_SLOT_INT:
        return x->num == y->num;
    case LUME_SLOT_BOOL:
        /* both sides normalized to 0/1 by the push/put helpers */
        return x->num == y->num;
    case LUME_SLOT_FLOAT: {
        double a, b;
        memcpy(&a, &x->num, sizeof a);
        memcpy(&b, &y->num, sizeof b);
        return a == b;
    }
    case LUME_SLOT_STR:
        return lume_str_eq(x->str, y->str);
    case LUME_SLOT_OBJ:
        return lume_value_eq((void *)(intptr_t)x->num,
                             (void *)(intptr_t)y->num);
    }
    return 0;
}

int64_t lume_value_eq(void *a, void *b)
{
    if (a == b) return 1;            /* same object (also covers null == null) */
    if (!a || !b) return 0;          /* one side null: not equal */
    int ka = ctr_kind(a), kb = ctr_kind(b);
    if (ka != kb || !is_ctr_kind(ka)) return 0;   /* different kinds / opaque */
    if (ka == LUME_CTR_LIST) {
        const LumeList *x = (const LumeList *)a, *y = (const LumeList *)b;
        if (x->len != y->len) return 0;
        for (int64_t i = 0; i < x->len; i++)
            if (!slot_eq(&x->items[i], &y->items[i])) return 0;
        return 1;
    }
    const LumeMap *x = (const LumeMap *)a, *y = (const LumeMap *)b;
    if (x->len != y->len) return 0;
    /* Order-independent, the way map membership works: equal lengths plus
     * every key of one side present in the other pins both key sets. An
     * enum's "$" tag is just another key, so tag and payload both count. */
    for (int64_t i = 0; i < x->len; i++) {
        int64_t j = map_find(y, x->keys[i]);
        if (j < 0) return 0;
        if (!slot_eq(&x->vals[i], &y->vals[j])) return 0;
    }
    return 1;
}

/* `==` where one side is statically `any`. An any is an opaque i8* with
 * no tag of its own, and it may travel as a boxed LumeSlot* (json(),
 * for-in, get-on-any) or as the raw container/string pointer an index
 * read hands back, so each side is first resolved to the value it
 * carries. Two containers then compare structurally (lume_value_eq), two
 * strings by bytes (lume_str_eq — which is what an any holding a string
 * needs, and what the interpreter's values_equal does for the same
 * pair), two scalars by tag and payload, a null payload by null, and
 * anything across those kinds is never equal. */
int64_t lume_any_eq(void *a, void *b)
{
    if (a == b) return 1;
    const LumeSlot *sa = ptr_is_slot(a) ? (const LumeSlot *)a : NULL;
    const LumeSlot *sb = ptr_is_slot(b) ? (const LumeSlot *)b : NULL;
    /* Resolve each side to (tag, payload). A boxed slot carries its own
     * tag; a raw pointer is a container when it says so at the kind
     * offset and a string otherwise (the emitters only ever hand a raw
     * any out as one of those two). */
    int ta, tb;
    void *pa, *pb;
    if (sa) {
        ta = sa->tag;
        pa = (void *)(intptr_t)sa->num;
    } else if (a && is_ctr_kind(ctr_kind(a))) {
        ta = LUME_SLOT_OBJ;
        pa = a;
    } else {
        ta = LUME_SLOT_STR;
        pa = a;                       /* the string itself */
    }
    if (sb) {
        tb = sb->tag;
        pb = (void *)(intptr_t)sb->num;
    } else if (b && is_ctr_kind(ctr_kind(b))) {
        tb = LUME_SLOT_OBJ;
        pb = b;
    } else {
        tb = LUME_SLOT_STR;
        pb = b;
    }
    if (ta != tb) return 0;
    switch (ta) {
    case LUME_SLOT_OBJ:
        /* A null payload on either side is the value null: two nulls are
         * equal, a null never equals a live container. */
        if (!pa || !pb) return pa == pb;
        return lume_value_eq(pa, pb);
    case LUME_SLOT_STR: {
        const char *xa = sa ? (sa->str ? sa->str : "") : (const char *)pa;
        const char *xb = sb ? (sb->str ? sb->str : "") : (const char *)pb;
        return lume_str_eq(xa, xb);
    }
    case LUME_SLOT_INT:
    case LUME_SLOT_BOOL:
        return sa && sb && sa->num == sb->num;
    case LUME_SLOT_FLOAT: {
        if (!sa || !sb) return 0;
        double da, db;
        memcpy(&da, &sa->num, sizeof da);
        memcpy(&db, &sb->num, sizeof db);
        return da == db;
    }
    default:
        return 0;
    }
}

/* ============ 13. Map helpers: len / print ============ */
double lume_map_get_f(const LumeMap *m, const char *k, double dflt)
{
    int64_t i = map_find(m, k);
    double d = dflt;
    if (i >= 0 && m->vals[i].tag != LUME_SLOT_STR) memcpy(&d, &m->vals[i].num, sizeof d);
    return d;
}
const char *lume_map_get_s(const LumeMap *m, const char *k, const char *dflt)
{
    int64_t i = map_find(m, k);
    if (i < 0) return dflt;
    return m->vals[i].tag == LUME_SLOT_STR ? m->vals[i].str : "";
}

const char *lume_map_key_at(const LumeMap *m, int64_t i)
{
    if (!m || i < 0 || i >= m->len) return "";
    return m->keys[i];
}

/* keys() — the interpreter hands back a *list*, so this builds one and the
 * call site treats the result as a list value. */
LumeList *lume_map_keys(const LumeMap *m)
{
    LumeList *l = lume_list_new();
    if (!m) return l;
    for (int64_t i = 0; i < m->len; i++) lume_list_push_s(l, m->keys[i]);
    return l;
}

/* range(stop) / range(start, stop) / range(start, stop, step) — the same
 * double arithmetic the interpreter's native_range() runs, whole values
 * included, so a compiled range prints and iterates exactly like the
 * interpreted one. step == 0 is the interpreter's run-time error. */
LumeList *lume_range(double a, double b, double step)
{
    if (step == 0) {
        fputs("lume: range() step must not be zero\n", stderr);
        exit(1);
    }
    LumeList *l = lume_list_new();
    if (step > 0) {
        for (double v = a; v < b; v += step)
            if (v == (double)(long long)v) lume_list_push_i(l, (long long)v);
            else lume_list_push_f(l, v);
    } else {
        for (double v = a; v > b; v += step)
            if (v == (double)(long long)v) lume_list_push_i(l, (long long)v);
            else lume_list_push_f(l, v);
    }
    return l;
}

int64_t lume_map_print(const LumeMap *m)
{
    /* Same null-value rule as lume_list_print: a null map value prints
     * "null", while a real empty map (a live lume_map_new pointer) still
     * prints "{}" below. */
    if (!m) return emit("null\n");
    /* Same opaque-i8* guard as lume_list_print: a string the emitter could
     * not type (map value types are not tracked) looks like a map here but is
     * not one. Printing it as a map would dereference string bytes as slots. */
    if (m->kind != LUME_CTR_MAP) { fputs("(opaque value)\n", stdout); return 0; }
    return emit("{") + container_body(m) + emit("}\n");
}
/* ============ 14. JSON parser (text -> tagged-any tree) ============ */
/* ------------------------------------------------------------------ *
 * JSON parsing (the reverse of slot_to_json): text -> tagged-any tree.
 *
 * The interpreter's json() decodes into GC Values; the native backends
 * have no GC, so this builds the same shape out of rt objects — a
 * LumeSlot per value, maps and lists as rt containers, string bytes
 * copied via dup_str. Number rules match the interpreter exactly: a
 * JSON number with no '.', 'e' or 'E' becomes an INT slot, anything
 * else a FLOAT slot. \uXXXX escapes (including surrogate pairs) decode
 * to UTF-8. A parse failure fills errbuf with "json parse error" plus
 * the byte offset, so a later native try() path can surface the same
 * text the interpreter prints.
 * ------------------------------------------------------------------ */

typedef struct { const char *p; char err[128]; } JsonSt;

static void js_ws(JsonSt *st)
{
    while (*st->p == ' ' || *st->p == '\t' || *st->p == '\n' || *st->p == '\r')
        st->p++;
}

/* Decode one JSON string starting at the opening quote into b. Returns
 * a pointer just past the closing quote, or NULL on malformed input.
 * Caller NUL-terminates via b.p[b.len] (jb_putc keeps cap > len). */
static const char *json_read_string(const char *p, JsonBuf *b)
{
    if (*p != '"') return NULL;
    p++;
    while (*p != '"') {
        if (!*p) return NULL;
        if (*p == '\\') {
            p++;
            switch (*p) {
                case 'n': jb_putc(b, '\n'); p++; break;
                case 't': jb_putc(b, '\t'); p++; break;
                case 'r': jb_putc(b, '\r'); p++; break;
                case 'b': jb_putc(b, '\b'); p++; break;
                case 'f': jb_putc(b, '\f'); p++; break;
                case '/': jb_putc(b, '/');  p++; break;
                case '\\': jb_putc(b, '\\'); p++; break;
                case '"': jb_putc(b, '"');  p++; break;
                case 'u': {
                    unsigned long cp = 0;
                    if (p[1] == 0 || p[2] == 0 || p[3] == 0 || p[4] == 0) return NULL;
                    for (int i = 1; i <= 4; i++) {
                        char c = p[i];
                        cp <<= 4;
                        if (c >= '0' && c <= '9') cp |= (unsigned)(c - '0');
                        else if (c >= 'a' && c <= 'f') cp |= (unsigned)(c - 'a' + 10);
                        else if (c >= 'A' && c <= 'F') cp |= (unsigned)(c - 'A' + 10);
                        else return NULL;
                    }
                    p += 5;
                    if (cp >= 0xD800 && cp <= 0xDBFF && p[0] == '\\' && p[1] == 'u') {
                        unsigned long lo = 0;
                        for (int i = 2; i <= 5; i++) {
                            char c = p[i];
                            lo <<= 4;
                            if (c >= '0' && c <= '9') lo |= (unsigned)(c - '0');
                            else if (c >= 'a' && c <= 'f') lo |= (unsigned)(c - 'a' + 10);
                            else if (c >= 'A' && c <= 'F') lo |= (unsigned)(c - 'A' + 10);
                            else return NULL;
                        }
                        if (lo >= 0xDC00 && lo <= 0xDFFF) {
                            cp = 0x10000 + ((cp - 0xD800) << 10) + (lo - 0xDC00);
                            p += 6;
                        }
                    }
                    if (cp < 0x80) jb_putc(b, (char)cp);
                    else if (cp < 0x800) {
                        jb_putc(b, (char)(0xC0 | (cp >> 6)));
                        jb_putc(b, (char)(0x80 | (cp & 0x3F)));
                    } else if (cp < 0x10000) {
                        jb_putc(b, (char)(0xE0 | (cp >> 12)));
                        jb_putc(b, (char)(0x80 | ((cp >> 6) & 0x3F)));
                        jb_putc(b, (char)(0x80 | (cp & 0x3F)));
                    } else {
                        jb_putc(b, (char)(0xF0 | (cp >> 18)));
                        jb_putc(b, (char)(0x80 | ((cp >> 12) & 0x3F)));
                        jb_putc(b, (char)(0x80 | ((cp >> 6) & 0x3F)));
                        jb_putc(b, (char)(0x80 | (cp & 0x3F)));
                    }
                    break;
                }
                default:
                    return NULL;
            }
        } else {
            jb_putc(b, *p);
            p++;
        }
    }
    return p + 1;
}

/* The num field of a FLOAT slot holds the double's bits (map_put_f and
 * slot_emit both rely on this), so reading one back is a memcpy. */
static double slot_float_of(LumeSlot s)
{
    double d = 0;
    memcpy(&d, &s.num, sizeof d);
    return d;
}

/* Parse one JSON value into a slot. On failure st->err is set and the
 * slot is a zeroed INT — callers must check st->err before using it.
 * Containers built on the way to a failure are intentionally not freed:
 * the error paths are rare, and the interpreter's vm_pop only drops the
 * stack reference, not the tree. A future lume_slot_free will own them. */
static LumeSlot json_value(JsonSt *st)
{
    LumeSlot s;
    memset(&s, 0, sizeof s);
    js_ws(st);
    char c = *st->p;

    if (c == '{') {
        st->p++;
        LumeMap *m = lume_map_new();
        js_ws(st);
        if (*st->p == '}') { st->p++; s.tag = LUME_SLOT_OBJ; s.num = (int64_t)(intptr_t)m; return s; }
        while (true) {
            js_ws(st);
            if (*st->p != '"') { snprintf(st->err, sizeof st->err, "expected object key"); break; }
            JsonBuf k = { 0, 0, 0 };
            const char *after = json_read_string(st->p, &k);
            if (k.p) k.p[k.len] = '\0';
            if (!after) { snprintf(st->err, sizeof st->err, "bad object key"); free(k.p); break; }
            st->p = after;
            js_ws(st);
            if (*st->p != ':') { snprintf(st->err, sizeof st->err, "expected ':'"); free(k.p); break; }
            st->p++;
            LumeSlot v = json_value(st);
            if (st->err[0]) { free(k.p); break; }
            const char *key = k.p ? k.p : "";
            switch (v.tag) {
            case LUME_SLOT_INT:   lume_map_put_i(m, key, v.num); break;
            case LUME_SLOT_FLOAT: lume_map_put_f(m, key, slot_float_of(v)); break;
            case LUME_SLOT_STR:   lume_map_put_s(m, key, v.str ? v.str : ""); break;
            case LUME_SLOT_BOOL:  lume_map_put_b(m, key, v.num); break;
            case LUME_SLOT_OBJ:   lume_map_put_obj(m, key, (void *)(intptr_t)v.num); break;
            }
            free(k.p);
            js_ws(st);
            if (*st->p == ',') { st->p++; continue; }
            if (*st->p == '}') { st->p++; break; }
            snprintf(st->err, sizeof st->err, "expected ',' or '}' in object");
            break;
        }
        if (st->err[0]) { s.tag = LUME_SLOT_INT; return s; }
        s.tag = LUME_SLOT_OBJ; s.num = (int64_t)(intptr_t)m;
        return s;
    }

    if (c == '[') {
        st->p++;
        LumeList *l = lume_list_new();
        js_ws(st);
        if (*st->p == ']') { st->p++; s.tag = LUME_SLOT_OBJ; s.num = (int64_t)(intptr_t)l; return s; }
        while (true) {
            LumeSlot v = json_value(st);
            if (st->err[0]) break;
            switch (v.tag) {
            case LUME_SLOT_INT:   lume_list_push_i(l, v.num); break;
            case LUME_SLOT_FLOAT: lume_list_push_f(l, slot_float_of(v)); break;
            case LUME_SLOT_STR:   lume_list_push_s(l, v.str ? v.str : ""); break;
            case LUME_SLOT_BOOL:  lume_list_push_b(l, v.num); break;
            case LUME_SLOT_OBJ:   lume_list_push_obj(l, (void *)(intptr_t)v.num); break;
            }
            js_ws(st);
            if (*st->p == ',') { st->p++; continue; }
            if (*st->p == ']') { st->p++; break; }
            snprintf(st->err, sizeof st->err, "expected ',' or ']' in array");
            break;
        }
        if (st->err[0]) { s.tag = LUME_SLOT_INT; return s; }
        s.tag = LUME_SLOT_OBJ; s.num = (int64_t)(intptr_t)l;
        return s;
    }

    if (c == '"') {
        JsonBuf b = { 0, 0, 0 };
        const char *after = json_read_string(st->p, &b);
        if (!after) { snprintf(st->err, sizeof st->err, "bad string"); free(b.p); return s; }
        st->p = after;
        if (b.p) b.p[b.len] = '\0';
        s.tag = LUME_SLOT_STR;
        s.str = dup_str(b.p ? b.p : "");
        free(b.p);
        return s;
    }

    if (c == '-' || (c >= '0' && c <= '9')) {
        const char *q = st->p;
        int is_int = 1;
        if (*q == '-') q++;
        if (*q == '-') is_int = 0;   /* "--" is malformed, not a number */
        while (*q >= '0' && *q <= '9') q++;
        if (*q == '.' || *q == 'e' || *q == 'E') is_int = 0;
        char *end = NULL;
        if (is_int) {
            long long iv = strtoll(st->p, &end, 10);
            if (end == st->p) { snprintf(st->err, sizeof st->err, "bad number"); return s; }
            st->p = end;
            s.tag = LUME_SLOT_INT; s.num = iv;
        } else {
            double d = strtod(st->p, &end);
            if (end == st->p) { snprintf(st->err, sizeof st->err, "bad number"); return s; }
            st->p = end;
            s.tag = LUME_SLOT_FLOAT;
            memcpy(&s.num, &d, sizeof d);
        }
        return s;
    }

    if (strncmp(st->p, "true", 4) == 0)  { st->p += 4; s.tag = LUME_SLOT_BOOL; s.num = 1; return s; }
    if (strncmp(st->p, "false", 5) == 0) { st->p += 5; s.tag = LUME_SLOT_BOOL; s.num = 0; return s; }
    if (strncmp(st->p, "null", 4) == 0)  { st->p += 4; s.tag = LUME_SLOT_OBJ; s.num = 0; return s; }

    snprintf(st->err, sizeof st->err, "unexpected character");
    return s;
}

/* Parse a full JSON document into a tagged-any tree. Returns a malloc'd
 * root slot on success, NULL on failure with "json parse error (at
 * offset N)" in errbuf — the same message shape the interpreter's
 * json() surfaces. The tree is malloc'd throughout and currently lives
 * for the whole process (a future lume_slot_free will own it, see the
 * note at lume_list_new's error path); a long-lived host embedding the
 * runtime should free slot trees once the ownership API lands. */
LumeSlot *lume_json_parse(const char *src, char *errbuf, size_t errbuf_size)
{
    JsonSt st;
    st.p = src ? src : "";
    st.err[0] = '\0';
    LumeSlot root = json_value(&st);
    if (st.err[0]) {
        if (errbuf && errbuf_size)
            snprintf(errbuf, errbuf_size, "json parse error (at offset %td)", st.p - src);
        return NULL;
    }
    LumeSlot *box = rt_alloc(sizeof(LumeSlot));
    *box = root;
    return box;
}

/* str() on a tagged-any value: serialize the slot tree the way the
 * interpreter's str() does. slot_to_json already implements every case
 * — scalars unquoted (INT via %lld, FLOAT via %g, BOOL via
 * true/false), strings kept with their quotes and escapes, containers
 * as JSON, the null obj slot as the bare word null. The interpreter
 * prints root maps and lists as compact JSON, so a boxed root slot
 * serializes identically. Returns malloc'd; caller frees. */
char *lume_slot_str(const LumeSlot *s)
{
    if (!s) return dup_str("null");
    JsonBuf b = { 0, 0, 0 };
    slot_to_json(&b, *s);
    jb_putc(&b, '\0');
    return b.p;
}

/* str() on a boxed slot in a scalar position (str(j["k"])): the
 * interpreter's str() of a string value is the bare string, while a
 * *container* still serializes as compact JSON (strings inside it keep
 * their quotes). slot_to_json is the container path; only a top-level
 * string slot is unquoted here. Caller frees. */
char *lume_slot_str_scalar(const LumeSlot *s)
{
    if (!s) return dup_str("null");
    if (s->tag == LUME_SLOT_STR)
        return dup_str(s->str ? s->str : "");
    JsonBuf b = { 0, 0, 0 };
    slot_to_json(&b, *s);
    jb_putc(&b, '\0');
    return b.p;
}

/* ------------------------------------------------------------------ *
 * String tools and scalar helpers (interpreter parity, 2026-10-21).
 *
 * The interpreter's implementations live in builtins_str.c / builtins.c /
 * builtins_math.c; these are the native-runtime counterparts the two
 * compile backends lower onto. Same contracts, plain scalars only:
 *   - case mapping is ASCII-only, exactly like the interpreter's
 *     native_upper/lower/capitalize (non-ASCII bytes pass through);
 *   - split/substr are byte-based, and substr's negative start counts
 *     from the end;
 *   - replace/join keep the interpreter's 16 MiB result ceiling;
 *   - str_match/regex are POSIX ERE findall over the same 10-slot
 *     regmatch_t window, yielding [{text, index, groups}] maps;
 *   - int("42")/float("1.5") parse a leading number like the
 *     interpreter's atoll/atof on the direct (non-accessor) path — a
 *     non-numeric string reads as 0;
 *   - bool(x) is value_truthy: null -> false, "" -> false, 0 -> false,
 *     an empty list/map -> false, everything else -> true.
 * ------------------------------------------------------------------ */

/* Result ceiling shared with replace()/join() in the interpreter. */
#define LUME_STR_CAP ((size_t)16u << 20)

static char *case_map(const char *s, int dir)
{
    if (!s) return dup_str("");
    size_t n = strlen(s);
    char *p = malloc(n + 1);
    if (!p) return NULL;
    for (size_t i = 0; i < n; i++) {
        char c = s[i];
        if (dir > 0 && c >= 'a' && c <= 'z') c = (char)(c - 32);
        else if (dir < 0 && c >= 'A' && c <= 'Z') c = (char)(c + 32);
        p[i] = c;
    }
    p[n] = '\0';
    return p;
}

char *lume_bi_upper(const char *s) { return case_map(s, 1); }
char *lume_bi_lower(const char *s) { return case_map(s, -1); }

/* capitalize(s): first ASCII char uppercased, the rest untouched (the
 * interpreter does NOT lowercase the tail). */
char *lume_bi_capitalize(const char *s)
{
    if (!s) return dup_str("");
    char *p = case_map(s, 0);
    if (p && p[0] >= 'a' && p[0] <= 'z') p[0] = (char)(p[0] - 32);
    return p;
}

static int rt_is_space(char c)
{
    return c == ' ' || c == '\t' || c == '\n' || c == '\r' ||
           c == '\f' || c == '\v';
}

char *lume_bi_trim(const char *s)
{
    if (!s) return dup_str("");
    size_t n = strlen(s), a = 0;
    while (a < n && rt_is_space(s[a])) a++;
    size_t b = n;
    while (b > a && rt_is_space(s[b - 1])) b--;
    char *p = malloc(b - a + 1);
    if (!p) return NULL;
    memcpy(p, s + a, b - a);
    p[b - a] = '\0';
    return p;
}

/* contains(s, sub): an empty sub is contained in everything, like the
 * interpreter's native_contains. */
/* ============ 15. Builtins: contains / log / parse / bool ============ */
int64_t lume_bi_contains(const char *s, const char *sub)
{
    if (!sub || !*sub) return 1;
    if (!s) return 0;
    return strstr(s, sub) != NULL;
}

/* split(s, sep): byte-based, strstr-driven. An empty sep yields [s]
 * (the interpreter's rule), and a trailing separator leaves a final
 * empty piece ("a," -> ["a", ""]). */
LumeList *lume_bi_split(const char *s, const char *sep)
{
    LumeList *out = lume_list_new();
    if (!out) return NULL;
    if (!s) s = "";
    size_t flen = sep ? strlen(sep) : 0;
    if (flen == 0) {
        lume_list_push_s(out, dup_str(s));
        return out;
    }
    const char *p = s, *end = s + strlen(s);
    for (;;) {
        const char *hit = strstr(p, sep);
        if (!hit) {
            lume_list_push_s(out, dup_str(p));
            return out;
        }
        size_t n = (size_t)(hit - p);
        char *piece = malloc(n + 1);
        if (!piece) return out;
        memcpy(piece, p, n);
        piece[n] = '\0';
        lume_list_push_s(out, piece);
        p = hit + flen;
        if (p > end) p = end;
    }
}

/* join(list, sep): the interpreter's str_of_value per element — strings
 * verbatim, ints %lld, floats %g, bools true/false, containers as the
 * same compact JSON print() renders. Same 16 MiB ceiling. */
char *lume_bi_join(const LumeList *l, const char *sep)
{
    JsonBuf b = { 0, 0, 0 };
    if (!l) return dup_str("");
    size_t flen = sep ? strlen(sep) : 0;
    for (int64_t i = 0; i < l->len; i++) {
        if (i && flen) {
            if (b.len + flen > LUME_STR_CAP) goto too_big;
            jb_puts(&b, sep);
        }
        LumeSlot s = l->items[i];
        switch (s.tag) {
        case LUME_SLOT_STR:
            if (s.str) {
                size_t n = strlen(s.str);
                if (b.len + n > LUME_STR_CAP) goto too_big;
                jb_puts(&b, s.str);
            }
            break;
        case LUME_SLOT_FLOAT:
        case LUME_SLOT_BOOL:
        case LUME_SLOT_OBJ:
        case LUME_SLOT_INT:
        default:
            /* Scalars render exactly like the interpreter's str_of_value
             * (%g for floats, true/false for bools, %lld for ints), and a
             * container as compact JSON. slot_to_json quotes a string, so
             * the STR tag is handled above instead. */
            slot_to_json(&b, s);
            break;
        }
    }
    jb_putc(&b, '\0');
    return b.p ? b.p : dup_str("");
too_big:
    free(b.p);
    lume_error_set("join() result too large");
    return dup_str("");
}

/* replace(s, from, to): literal global replace, byte-exact like the
 * interpreter's native_replace. An empty `from` returns s unchanged. */
char *lume_bi_replace(const char *s, const char *from, const char *to)
{
    if (!s) s = "";
    if (!from || !*from) return dup_str(s);
    if (!to) to = "";
    size_t slen = strlen(s), flen = strlen(from), tlen = strlen(to);
    JsonBuf b = { 0, 0, 0 };
    const char *p = s, *end = s + slen;
    int matched = 0;
    while (p < end) {
        if ((size_t)(end - p) >= flen && memcmp(p, from, flen) == 0) {
            if (b.len + tlen > LUME_STR_CAP) goto too_big;
            jb_puts(&b, to);
            p += flen;
            matched = 1;
        } else {
            if (b.len + 1 > LUME_STR_CAP) goto too_big;
            jb_putc(&b, *p);
            p++;
        }
    }
    if (!matched) { free(b.p); return dup_str(s); }
    jb_putc(&b, '\0');
    return b.p ? b.p : dup_str("");
too_big:
    free(b.p);
    lume_error_set("replace() result too large");
    return dup_str(s);
}

/* substr(s, start, len): byte-based; a negative start counts from the
 * end and both bounds clamp. len < 0 means "to the end of the string"
 * — the spelling the emitters use for the two-argument form, where the
 * interpreter defaults len to n - start. */
char *lume_bi_substr(const char *s, int64_t start, int64_t len)
{
    if (!s) return dup_str("");
    int64_t n = (int64_t)strlen(s);
    if (start < 0) start = n + start;
    if (start < 0) start = 0;
    if (start > n) start = n;
    if (len < 0 || start + len > n) len = n - start;
    char *p = malloc((size_t)len + 1);
    if (!p) return NULL;
    memcpy(p, s + start, (size_t)len);
    p[len] = '\0';
    return p;
}

/* str_match(pattern, text) / regex(pattern, text): POSIX ERE findall.
 * Each hit becomes {text, index, groups} — groups is the list of
 * capture groups 1..9, cut at the first unmatched one, exactly like the
 * interpreter's native_str_match (builtins.c). A zero-width match ends
 * the scan; an invalid pattern goes through the error channel so
 * try(str_match, ...) reports it like the interpreter's VM error. */
LumeList *lume_bi_str_match(const char *pat, const char *txt)
{
    LumeList *out = lume_list_new();
    if (!out) return NULL;
    if (!pat) pat = "";
    if (!txt) txt = "";
    regex_t re;
    if (regcomp(&re, pat, REG_EXTENDED) != 0) {
        lume_error_set("str_match(): invalid regex pattern");
        return out;
    }
    const char *p = txt;
    regmatch_t m[10];               /* m[0] = whole match, m[1..9] = groups */
    while (regexec(&re, p, 10, m, 0) == 0) {
        int so = m[0].rm_so, eo = m[0].rm_eo;
        if (eo <= so) break;        /* zero-width match: stop, like the interpreter */
        size_t start = (size_t)(p - txt) + (size_t)so;
        size_t mlen = (size_t)eo - (size_t)so;
        LumeMap *mo = lume_map_new();
        char *text = malloc(mlen + 1);
        if (!mo || !text) { if (mo) free(mo); break; }
        memcpy(text, txt + start, mlen);
        text[mlen] = '\0';
        lume_map_put_s(mo, "text", text);
        lume_map_put_i(mo, "index", (int64_t)start);
        LumeList *gs = lume_list_new();
        for (int gi = 1; gi < 10; gi++) {
            if (m[gi].rm_so == -1) break;
            int gso = m[gi].rm_so, geo = m[gi].rm_eo;
            size_t glen = (size_t)geo - (size_t)gso;
            char *g = malloc(glen + 1);
            if (!g) break;
            memcpy(g, p + gso, glen);
            g[glen] = '\0';
            lume_list_push_s(gs, g);
        }
        lume_map_put_obj(mo, "groups", gs);
        lume_list_push_obj(out, mo);
        p += eo;
        if (*p == '\0') break;
    }
    regfree(&re);
    return out;
}

double lume_bi_exp(double x) { return exp(x); }

/* log(x) / ln(x): the interpreter refuses a non-positive argument with
 * a VM error, so the native flavour goes through the error channel —
 * inside try() it fills { err: ... }, outside it prints and exits. */
double lume_bi_log(double x)
{
    if (x <= 0.0) {
        lume_error_set("log(): argument must be positive");
        return 0.0;
    }
    return log(x);
}

/* int("42") / float("1.5"): the interpreter's direct (non-accessor) cast
 * path is atoll/atof — a leading number parses, anything else reads as
 * 0 ("12x" -> 12, "abc" -> 0, "  7 " -> 7). The accessor path
 * int(m, "k", d) is stricter (whole-string parse, else the default) and
 * stays on the get() desugar, where the default is applied by
 * lume_map_get_* instead. */
int64_t lume_bi_parse_i64(const char *s)
{
    return s ? (int64_t)strtoll(s, NULL, 10) : 0;
}

double lume_bi_parse_double(const char *s)
{
    return s ? strtod(s, NULL) : 0.0;
}

/* bool(x) — value_truthy per static type, so the emitters pick the
 * flavour from the operand's type the way len() does. The slot flavour
 * dispatches on the runtime tag for an `any` (json()) value. */
int64_t lume_bi_bool_s(const char *s) { return s && *s; }
int64_t lume_bi_bool_i(int64_t v) { return v != 0; }
int64_t lume_bi_bool_f(double v) { return v != 0.0; }
int64_t lume_bi_bool_obj(const void *p)
{
    if (!p) return 0;
    return ((const LumeList *)p)->len > 0;
}
int64_t lume_bi_bool_slot(const LumeSlot *s)
{
    if (!s) return 0;
    switch (s->tag) {
    case LUME_SLOT_STR:   return s->str && *s->str;
    case LUME_SLOT_BOOL:  return s->num != 0;
    case LUME_SLOT_OBJ:   return lume_bi_bool_obj((const void *)(intptr_t)s->num);
    case LUME_SLOT_FLOAT: {
        double d = 0;
        memcpy(&d, &s->num, sizeof d);
        return d != 0.0;
    }
    case LUME_SLOT_INT:
    default:              return s->num != 0;
    }
}

/* The string tools take a `string` in the source, but an `any` argument
 * is legal there (the interpreter's native_upper accepts a dynamic
 * string value — `upper(get(j, "k"))` works), and an any is either a
 * boxed LumeSlot* or the raw char* an index read hands back. This is the
 * one place that knows the difference: a string slot (or a raw pointer,
 * which can only be a string) passes through, and anything else — a
 * number, a container, a null — goes through the error channel with the
 * interpreter's wording, so `upper(5)` fails the same way on both paths
 * instead of silently reading slot bytes as text. */
const char *lume_bi_str_unwrap(const void *v)
{
    if (!v) {
        lume_error_set("expected string argument");
        return "";
    }
    if (ptr_is_slot(v)) {
        const LumeSlot *s = (const LumeSlot *)v;
        if (s->tag == LUME_SLOT_STR) return s->str ? s->str : "";
        lume_error_set("expected string argument");
        return "";
    }
    return (const char *)v;
}

/* ------------------------------------------------------------------ *
 * Field/element access on a tagged-any slot (the json()/for-in value).
 * An any value is a boxed LumeSlot*, so get(j, k, d) on one has to
 * unpack the slot before the map lookup — a non-map slot reads the
 * default, matching get()'s absent-key semantics. Codegen picks the
 * flavour from the default's static type, exactly like lume_map_get_*.
 * ------------------------------------------------------------------ */

/* ============ 16. Slot accessors: get / accumulate / length ============ */
int64_t lume_slot_get_i(const LumeSlot *s, const char *k, int64_t d)
{
    if (!s || s->tag != LUME_SLOT_OBJ) return d;
    LumeMap *m = (LumeMap *)(intptr_t)s->num;
    if (!m || m->kind != LUME_CTR_MAP) return d;
    return lume_map_get_i(m, k, d);
}

double lume_slot_get_f(const LumeSlot *s, const char *k, double d)
{
    if (!s || s->tag != LUME_SLOT_OBJ) return d;
    LumeMap *m = (LumeMap *)(intptr_t)s->num;
    if (!m || m->kind != LUME_CTR_MAP) return d;
    return lume_map_get_f(m, k, d);
}

const char *lume_slot_get_s(const LumeSlot *s, const char *k, const char *d)
{
    if (!s || s->tag != LUME_SLOT_OBJ) return d;
    LumeMap *m = (LumeMap *)(intptr_t)s->num;
    if (!m || m->kind != LUME_CTR_MAP) return d;
    return lume_map_get_s(m, k, d);
}

void *lume_slot_get_obj(const LumeSlot *s, const char *k, void *d)
{
    if (!s || s->tag != LUME_SLOT_OBJ) return d;
    LumeMap *m = (LumeMap *)(intptr_t)s->num;
    if (!m || m->kind != LUME_CTR_MAP) return d;
    return lume_map_get_obj(m, k, d);
}

/* The cast accessors (int/float/str/bool) on a tagged-any receiver — a
 * json() parse, a for-in value. The interpreter's value_from_map
 * accepts any runtime map, so an any holding one behaves exactly like
 * a struct receiver: unpack the slot like the getters above, then run
 * the same lume_map_acc_* table (found string whole-string-parses or
 * falls back, found bool/container falls back for int/float, absent
 * falls back). A non-map slot reads the default; the interpreter would
 * cast the receiver itself there — the corner get()'s any path already
 * documents, unchanged. */
int64_t lume_slot_acc_i(const LumeSlot *s, const char *k, int64_t d)
{
    if (!s || s->tag != LUME_SLOT_OBJ) return d;
    LumeMap *m = (LumeMap *)(intptr_t)s->num;
    if (!m || m->kind != LUME_CTR_MAP) return d;
    return lume_map_acc_i(m, k, d);
}

double lume_slot_acc_f(const LumeSlot *s, const char *k, double d)
{
    if (!s || s->tag != LUME_SLOT_OBJ) return d;
    LumeMap *m = (LumeMap *)(intptr_t)s->num;
    if (!m || m->kind != LUME_CTR_MAP) return d;
    return lume_map_acc_f(m, k, d);
}

const char *lume_slot_acc_s(const LumeSlot *s, const char *k, const char *d)
{
    if (!s || s->tag != LUME_SLOT_OBJ) return d;
    LumeMap *m = (LumeMap *)(intptr_t)s->num;
    if (!m || m->kind != LUME_CTR_MAP) return d;
    return lume_map_acc_s(m, k, d);
}

int64_t lume_slot_acc_b(const LumeSlot *s, const char *k, int64_t d)
{
    if (!s || s->tag != LUME_SLOT_OBJ) return d;
    LumeMap *m = (LumeMap *)(intptr_t)s->num;
    if (!m || m->kind != LUME_CTR_MAP) return d;
    return lume_map_acc_b(m, k, d);
}

/* get(m, k) with no default on an any value: the key reads back as its own
 * boxed slot, whatever the cell holds — the interpreter's dynamic read.
 * An absent key (or a non-map receiver) reads as the null slot, the boxed
 * shape the literal null already travels in. The third parameter exists so
 * every getter shares one three-operand call shape in the emitters; it is
 * ignored here. */
LumeSlot *lume_slot_get_any(const LumeSlot *s, const char *k, LumeSlot *dflt)
{
    (void)dflt;
    if (!s || s->tag != LUME_SLOT_OBJ) return null_slot_box();
    LumeMap *m = (LumeMap *)(intptr_t)s->num;
    if (!m || m->kind != LUME_CTR_MAP) return null_slot_box();
    int64_t i = map_find(m, k);
    if (i >= 0) {
        LumeSlot *box = rt_alloc(sizeof(LumeSlot));
        *box = m->vals[i];
        return box;
    }
    return null_slot_box();
}

/* len() on an any value: a container slot answers with its length, a
 * scalar or a null slot with 0 (the interpreter's len(map)/len(list)
 * only sees containers; asking anything else reads 0 there too). */
int64_t lume_slot_len(const LumeSlot *s)
{
    if (!s || s->tag != LUME_SLOT_OBJ) return 0;
    const LumeList *c = (const LumeList *)(intptr_t)s->num;
    if (!c) return 0;
    return c->kind == LUME_CTR_MAP ? lume_map_len((const LumeMap *)c)
                                   : lume_list_len(c);
}

/* Element read off an any list slot (for-in / indexing): returns a
 * boxed LumeSlot copy so every element kind survives; NULL for a
 * non-list slot or an out-of-range index. */
LumeSlot *lume_slot_at(const LumeSlot *s, int64_t i)
{
    if (!s || s->tag != LUME_SLOT_OBJ) return NULL;
    const LumeList *l = (const LumeList *)(intptr_t)s->num;
    if (!l || l->kind != LUME_CTR_LIST) return NULL;
    if (i < 0 || i >= l->len) return NULL;
    LumeSlot *box = rt_alloc(sizeof(LumeSlot));
    *box = l->items[i];
    return box;
}

/* Keyed element read off an any map slot (j["k"]): returns a pointer
 * into the map's own value array so every element kind survives; NULL
 * for a non-map slot or a missing key. The caller reports the failure
 * through lume_index_error (SPEC 6.2) when it sees NULL. */
LumeSlot *lume_slot_at_key(const LumeSlot *s, const char *k)
{
    if (!s || s->tag != LUME_SLOT_OBJ) return NULL;
    LumeMap *m = (LumeMap *)(intptr_t)s->num;
    if (!m || m->kind != LUME_CTR_MAP) return NULL;
    int64_t i = map_find(m, k);
    if (i < 0) return NULL;
    return &m->vals[i];
}

/* ------------------------------------------------------------------ *
 * Quant builtins (2026-10-24): descriptive statistics, correlation /
 * linear regression, and the time-series helpers a quant workflow
 * needs. The native side mirrors the interpreter's native_* contracts
 * in builtins_math.c; element reads go through lume_list_at_f so int
 * and float slots both widen, exactly like the interpreter's list
 * reads. Return values: double helpers answer NAN on a degenerate
 * input; pct_change() builds a fresh list (length n-1, empty for n<2);
 * linreg() builds a fresh {slope, intercept, r2} map.
 * ------------------------------------------------------------------ */

static int bi_quant_cmp_d(const void *a, const void *b)
{
    double x = *(const double *)a, y = *(const double *)b;
    return x < y ? -1 : (x > y ? 1 : 0);
}

/* Extract the list as a plain double array (int slots widen). NULL with
 * n=0 when the list is null/empty. */
static double *bi_num_copy(const LumeList *l, int64_t *n)
{
    *n = l ? l->len : 0;
    if (*n == 0) return NULL;
    double *buf = (double *)calloc((size_t)*n, sizeof(double));
    if (!buf) return NULL;
    for (int64_t i = 0; i < *n; i++) buf[i] = lume_list_at_f(l, i);
    return buf;
}

double lume_bi_mean(const LumeList *l)
{
    int64_t n; double *xs = bi_num_copy(l, &n);
    if (!xs || n == 0) { free(xs); return NAN; }
    double s = 0.0;
    for (int64_t i = 0; i < n; i++) s += xs[i];
    free(xs);
    return s / (double)n;
}

/* Fisher-Yates shuffle of a fresh list (the input is left untouched): for
 * i = n-1 down to 1, swap i with rand_int(0, i). rand_int consumes two
 * PCG32 draws per step, and the fixed stream makes the permutation
 * deterministic across backends and runs. */
LumeList *lume_bi_shuffle(const LumeList *l)
{
    LumeList *out = lume_list_new();
    int64_t n; double *xs = bi_num_copy(l, &n);
    if (!xs) return out;
    for (int64_t i = n - 1; i > 0; i--) {
        int64_t j = lume_bi_rand_int(0, i);
        double t = xs[i]; xs[i] = xs[j]; xs[j] = t;
    }
    for (int64_t i = 0; i < n; i++) lume_list_push_f(out, xs[i]);
    free(xs);
    return out;
}

double lume_bi_variance(const LumeList *l)
{
    /* Sample variance (n-1 denominator), the convention quant work uses.
     * Welford's single-pass algorithm: the naive two-pass mean-then-sum
     * loses the low-order digits when the data rides on a large offset
     * (prices near 1e9 with ~1e-3 fluctuations), while the running-mean
     * form keeps the same result on ordinary data. */
    int64_t n; double *xs = bi_num_copy(l, &n);
    if (!xs || n < 2) { free(xs); return NAN; }
    double m = 0.0, m2 = 0.0;
    for (int64_t i = 0; i < n; i++) {
        double d = xs[i] - m;
        m += d / (double)(i + 1);
        m2 += d * (xs[i] - m);
    }
    free(xs);
    return m2 / (double)(n - 1);
}

double lume_bi_stddev(const LumeList *l)
{
    double v = lume_bi_variance(l);
    return v == v ? sqrt(v) : NAN;
}

double lume_bi_median(const LumeList *l)
{
    int64_t n; double *xs = bi_num_copy(l, &n);
    if (!xs || n == 0) { free(xs); return NAN; }
    qsort(xs, (size_t)n, sizeof(double), bi_quant_cmp_d);
    double med = (n % 2) ? xs[n / 2]
                         : 0.5 * (xs[n / 2 - 1] + xs[n / 2]);
    free(xs);
    return med;
}

double lume_bi_percentile(const LumeList *l, double p)
{
    /* p in [0,100]; linear interpolation between the bracketing order
     * statistics (the pandas default). */
    if (p < 0.0 || p > 100.0 || p != p) return NAN;
    int64_t n; double *xs = bi_num_copy(l, &n);
    if (!xs || n == 0) { free(xs); return NAN; }
    qsort(xs, (size_t)n, sizeof(double), bi_quant_cmp_d);
    double r = p / 100.0 * (double)(n - 1);
    int64_t lo = (int64_t)r;
    double frac = r - (double)lo;
    double v = lo + 1 < n
                   ? xs[lo] + frac * (xs[lo + 1] - xs[lo])
                   : xs[lo];
    free(xs);
    return v;
}

double lume_bi_covariance(const LumeList *x, const LumeList *y)
{
    /* Joint Welford (West form), the pairwise analogue of variance: stable
     * under large offsets, single pass. The interpreter twin in
     * builtins_math.c uses the identical recurrence so the three legs stay
     * byte-identical. */
    int64_t nx, ny;
    double *xs = bi_num_copy(x, &nx), *ys = bi_num_copy(y, &ny);
    if (!xs || !ys || nx < 2 || nx != ny) { free(xs); free(ys); return NAN; }
    double mx = 0.0, my = 0.0, c = 0.0;
    for (int64_t i = 0; i < nx; i++) {
        double dx = xs[i] - mx, dy = ys[i] - my;
        mx += dx / (double)(i + 1);
        my += dy / (double)(i + 1);
        c += dx * (ys[i] - my);
    }
    free(xs); free(ys);
    return c / (double)(nx - 1);
}

double lume_bi_correlation(const LumeList *x, const LumeList *y)
{
    /* sxy/sqrt(sxx*syy) via the same joint-Welford recurrence, so the
     * native leg matches the interpreter's independent computation
     * byte-for-byte (a covariance/stddev-based formulation would differ in
     * the last ulp from the sxx/syy form on some inputs). */
    int64_t nx, ny;
    double *xs = bi_num_copy(x, &nx), *ys = bi_num_copy(y, &ny);
    if (!xs || !ys || nx < 2 || nx != ny) { free(xs); free(ys); return NAN; }
    double mx = 0.0, my = 0.0, sxx = 0.0, sxy = 0.0, syy = 0.0;
    for (int64_t i = 0; i < nx; i++) {
        double dx = xs[i] - mx, dy = ys[i] - my;
        mx += dx / (double)(i + 1);
        my += dy / (double)(i + 1);
        sxx += dx * (xs[i] - mx);
        sxy += dx * (ys[i] - my);
        syy += dy * (ys[i] - my);
    }
    free(xs); free(ys);
    if (sxx == 0.0 || syy == 0.0) return NAN;
    return sxy / sqrt(sxx * syy);
}

/* Least-squares fit y = slope*x + intercept with r^2. A fresh map holds
 * the three doubles; NULL (never returned as NAN-carrying doubles) when
 * the data is degenerate. */
LumeMap *lume_bi_linreg(const LumeList *x, const LumeList *y)
{
    int64_t nx, ny;
    double *xs = bi_num_copy(x, &nx), *ys = bi_num_copy(y, &ny);
    if (!xs || !ys || nx < 2 || nx != ny) { free(xs); free(ys); return NULL; }
    /* Joint Welford for sxx/sxy/syy (West form): slope and r2 inherit the
     * same large-offset stability as variance/covariance. */
    double mx = 0.0, my = 0.0, sxx = 0.0, sxy = 0.0, syy = 0.0;
    for (int64_t i = 0; i < nx; i++) {
        double dx = xs[i] - mx, dy = ys[i] - my;
        mx += dx / (double)(i + 1);
        my += dy / (double)(i + 1);
        sxx += dx * (xs[i] - mx);
        sxy += dx * (ys[i] - my);
        syy += dy * (ys[i] - my);
    }
    free(xs); free(ys);
    if (sxx == 0.0) return NULL;
    double slope = sxy / sxx;
    double intercept = my - slope * mx;
    double r2 = (syy == 0.0) ? (sxy == 0.0 ? 1.0 : NAN) : sxy * sxy / (sxx * syy);
    LumeMap *m = lume_map_new();
    lume_map_put_f(m, "slope", slope);
    lume_map_put_f(m, "intercept", intercept);
    lume_map_put_f(m, "r2", r2);
    return m;
}

/* Percentage change vs the previous element, in percent (100x, the way a
 * financial return column is usually shown): out[i] = 100*(x[i+1]-x[i])/x[i].
 * A zero previous element answers NAN for that step (division by zero is
 * undefined, not 0). Returns a fresh list; n<2 gives an empty list. */
LumeList *lume_bi_pct_change(const LumeList *l)
{
    LumeList *out = lume_list_new();
    int64_t n; double *xs = bi_num_copy(l, &n);
    if (!xs || n < 2) { free(xs); return out; }
    for (int64_t i = 1; i < n; i++) {
        double prev = xs[i - 1];
        double pc = prev == 0.0 ? NAN : 100.0 * (xs[i] - prev) / prev;
        lume_list_push_f(out, pc);
    }
    free(xs);
    return out;
}

/* Max drawdown of a price series, as a positive percentage: the largest
 * peak-to-trough drop, 0 for a monotonically rising series, NAN for n<2. */
double lume_bi_max_drawdown(const LumeList *l)
{
    int64_t n; double *xs = bi_num_copy(l, &n);
    if (!xs || n < 2) { free(xs); return NAN; }
    double peak = xs[0], mdd = 0.0;
    for (int64_t i = 1; i < n; i++) {
        if (xs[i] > peak) peak = xs[i];
        double dd = peak > 0.0 ? 100.0 * (peak - xs[i]) / peak : 0.0;
        if (dd > mdd) mdd = dd;
    }
    free(xs);
    return mdd;
}

/* int -> i64 for the native text emitter (float window / alpha arguments
 * must truncate before the runtime call; the LLVM leg uses fptosi). */
int64_t lume_bi_ftoi(double v) { return (int64_t)v; }

/* Sample skewness (adjusted Fisher-Pearson, the pandas / Python
 * statistics.skew default): g1 = T1/(T2*sqrt(T2)) * n/((n-1)(n-2)) with
 * T1 = sum((x-m)^3), T2 = sum((x-m)^2). NAN for n<3 or a zero spread. */
double lume_bi_skew(const LumeList *l)
{
    int64_t n; double *xs = bi_num_copy(l, &n);
    if (!xs || n < 3) { free(xs); return NAN; }
    double m = 0.0;
    for (int64_t i = 0; i < n; i++) m += xs[i];
    m /= (double)n;
    double t1 = 0.0, t2 = 0.0;
    for (int64_t i = 0; i < n; i++) {
        double d = xs[i] - m;
        t1 += d * d * d;
        t2 += d * d;
    }
    free(xs);
    if (t2 == 0.0) return NAN;
    double g = t1 / (t2 * sqrt(t2));
    return g * (double)n / ((double)(n - 1) * (double)(n - 2));
}

/* Sample excess kurtosis (Python statistics.kurtosis / pandas kurt
 * default): n(n+1)*T1/((n-1)(n-2)(n-3)*T2^2) - 3(n-1)^2/((n-2)(n-3)),
 * T1 = sum((x-m)^4), T2 = sum((x-m)^2). NAN for n<4 or a zero spread. */
double lume_bi_kurtosis(const LumeList *l)
{
    int64_t n; double *xs = bi_num_copy(l, &n);
    if (!xs || n < 4) { free(xs); return NAN; }
    double m = 0.0;
    for (int64_t i = 0; i < n; i++) m += xs[i];
    m /= (double)n;
    double t1 = 0.0, t2 = 0.0;
    for (int64_t i = 0; i < n; i++) {
        double d = xs[i] - m;
        double d2 = d * d;
        t1 += d2 * d2;
        t2 += d2;
    }
    free(xs);
    if (t2 == 0.0) return NAN;
    return (double)n * ((double)n + 1.0) * t1
               / ((double)(n - 1) * (double)(n - 2) * (double)(n - 3) * t2 * t2)
           - 3.0 * (double)(n - 1) * (double)(n - 1)
               / ((double)(n - 2) * (double)(n - 3));
}

/* Rolling window mean: a fresh list of the same length; positions whose
 * window is not yet full (i < w-1) answer NAN, the rest the arithmetic
 * mean of xs[i-w+1..i]. A window < 1 gives an empty list (the
 * interpreter reports an error instead). */
LumeList *lume_bi_rolling_mean(const LumeList *l, int64_t w)
{
    LumeList *out = lume_list_new();
    int64_t n; double *xs = bi_num_copy(l, &n);
    if (!xs || w < 1) { free(xs); return out; }
    double s = 0.0;
    for (int64_t i = 0; i < n; i++) {
        s += xs[i];
        if (i >= w) s -= xs[i - w];
        lume_list_push_f(out, i + 1 >= w ? s / (double)w : NAN);
    }
    free(xs);
    return out;
}

/* Rolling window sample standard deviation: like rolling_mean but the
 * window's sample stddev (n-1 denominator, matching variance()). Windows
 * smaller than 2 (including w==1) answer NAN; w<1 gives an empty list. */
LumeList *lume_bi_rolling_stddev(const LumeList *l, int64_t w)
{
    LumeList *out = lume_list_new();
    int64_t n; double *xs = bi_num_copy(l, &n);
    if (!xs || w < 1) { free(xs); return out; }
    for (int64_t i = 0; i < n; i++) {
        if (i + 1 < w || w < 2) { lume_list_push_f(out, NAN); continue; }
        int64_t lo = i + 1 - w;
        double m = 0.0;
        for (int64_t j = lo; j <= i; j++) m += xs[j];
        m /= (double)w;
        double s = 0.0;
        for (int64_t j = lo; j <= i; j++) { double d = xs[j] - m; s += d * d; }
        lume_list_push_f(out, sqrt(s / (double)(w - 1)));
    }
    free(xs);
    return out;
}

/* Exponentially weighted moving average: out[0] = x[0],
 * out[i] = alpha*x[i] + (1-alpha)*out[i-1]. alpha in (0,1] (1 keeps the
 * series, 0 would freeze at the first element and is rejected); an
 * out-of-range alpha gives an empty list (interpreter reports an error). */
LumeList *lume_bi_ewma(const LumeList *l, double alpha)
{
    LumeList *out = lume_list_new();
    int64_t n; double *xs = bi_num_copy(l, &n);
    if (!xs || alpha <= 0.0 || alpha > 1.0) { free(xs); return out; }
    if (n > 0) {
        double e = xs[0];
        lume_list_push_f(out, e);
        for (int64_t i = 1; i < n; i++) {
            e = alpha * xs[i] + (1.0 - alpha) * e;
            lume_list_push_f(out, e);
        }
    }
    free(xs);
    return out;
}

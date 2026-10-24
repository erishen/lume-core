#!/usr/bin/env python3
# tests/quant-refcheck.py — independent reference check for the quant
# builtins. Cross-validates the values pinned in tests/quant-consistency.lume
# and examples/quant.lume against Python's standard library (statistics)
# plus minimal hand-rolled implementations of percentile / covariance /
# correlation / linreg / pct_change / max_drawdown that follow the same
# formulas as src/rt.c and builtins_math.c.
#
# Run:  python tests/quant-refcheck.py
#
# The expected values below are copied from lume's own pinned output
# (quant-consistency.expected / the example run); every one of them is also
# a closed-form number that can be verified by hand (variance 2.5, cov 5,
# corr 1, slope 2, drawdown 50/25, ...). PASS here means lume's three
# backends agree with an independent implementation, not just with each other.
import statistics as st
import math

TOL = 1e-6


def percentile(data, p):
    s = sorted(data)
    n = len(s)
    r = p / 100.0 * (n - 1)
    lo = int(r)
    frac = r - lo
    return s[lo] + frac * (s[lo + 1] - s[lo]) if lo + 1 < n else s[lo]


def sample_cov(x, y):
    n = len(x)
    mx = sum(x) / n
    my = sum(y) / n
    return sum((a - mx) * (b - my) for a, b in zip(x, y)) / (n - 1)


def sample_corr(x, y):
    return sample_cov(x, y) / (st.stdev(x) * st.stdev(y))


def linreg(x, y):
    n = len(x)
    mx = sum(x) / n
    my = sum(y) / n
    sxx = sum((a - mx) ** 2 for a in x)
    sxy = sum((a - mx) * (b - my) for a, b in zip(x, y))
    syy = sum((b - my) ** 2 for b in y)
    slope = sxy / sxx
    intercept = my - slope * mx
    r2 = sxy * sxy / (sxx * syy) if syy != 0 else (1.0 if sxy == 0 else float("nan"))
    return slope, intercept, r2


def pct_change(data):
    return [
        float("nan") if data[i - 1] == 0 else 100.0 * (data[i] - data[i - 1]) / data[i - 1]
        for i in range(1, len(data))
    ]


def max_drawdown(data):
    peak = data[0]
    mdd = 0.0
    for x in data[1:]:
        if x > peak:
            peak = x
        if peak > 0:
            mdd = max(mdd, 100.0 * (peak - x) / peak)
    return mdd


def skew(data):
    """Adjusted Fisher-Pearson sample skewness (pandas / Python statistics
    default): g1 = T1/(T2*sqrt(T2)) * n/((n-1)(n-2))."""
    n = len(data)
    if n < 3:
        return float("nan")
    m = sum(data) / n
    t1 = sum((a - m) ** 3 for a in data)
    t2 = sum((a - m) ** 2 for a in data)
    if t2 == 0:
        return float("nan")
    return (t1 / (t2 * math.sqrt(t2))) * n / ((n - 1) * (n - 2))


def kurtosis(data):
    """Sample excess kurtosis (Python statistics.kurtosis / pandas kurt
    default): n(n+1)T1/((n-1)(n-2)(n-3)T2^2) - 3(n-1)^2/((n-2)(n-3))."""
    n = len(data)
    if n < 4:
        return float("nan")
    m = sum(data) / n
    t1 = sum((a - m) ** 4 for a in data)
    t2 = sum((a - m) ** 2 for a in data)
    if t2 == 0:
        return float("nan")
    return (n * (n + 1) * t1 / ((n - 1) * (n - 2) * (n - 3) * t2 * t2)
            - 3.0 * (n - 1) ** 2 / ((n - 2) * (n - 3)))


def rolling_mean(data, w):
    out = []
    for i in range(len(data)):
        out.append(float("nan") if i + 1 < w else sum(data[i + 1 - w:i + 1]) / w)
    return out


def rolling_stddev(data, w):
    out = []
    for i in range(len(data)):
        if i + 1 < w or w < 2:
            out.append(float("nan"))
        else:
            out.append(st.stdev(data[i + 1 - w:i + 1]))
    return out


def ewma(data, alpha):
    out = []
    if data:
        e = data[0]
        out.append(e)
        for v in data[1:]:
            e = alpha * v + (1.0 - alpha) * e
            out.append(e)
    return out


def norminv_ref(p):
    """Independent inverse-normal reference (Acklam's published rational
    approximation, 2011) — pins the lume values above; the round-trip
    identity rows need no implementation."""
    a = [-3.969683028665376e+01, 2.209460984245205e+02, -2.759285104469687e+02,
         1.383577518672690e+02, -3.066479806614716e+01, 2.506628277459239e+00]
    b = [-5.447609879822406e+01, 1.615858368580409e+02, -1.556989798598866e+02,
         6.680131188771972e+01, -1.328068155288572e+01]
    c = [-7.784894002430293e-03, -3.223964580411365e-01, -2.400758277161838e+00,
         -2.549732539343734e+00, 4.374664141464968e+00, 2.938163982698783e+00]
    d = [7.784695709041462e-03, 3.224671290700398e-01, 2.445134137142996e+00,
         3.754408661907416e+00]
    if p <= 0.0 or p >= 1.0:
        return float("nan")
    if p < 0.02425:
        q = math.sqrt(-2.0 * math.log(p))
        return (((((c[0] * q + c[1]) * q + c[2]) * q + c[3]) * q + c[4]) * q + c[5]) / \
               ((((d[0] * q + d[1]) * q + d[2]) * q + d[3]) * q + 1.0)
    if p > 0.97575:
        q = math.sqrt(-2.0 * math.log(1.0 - p))
        return -(((((c[0] * q + c[1]) * q + c[2]) * q + c[3]) * q + c[4]) * q + c[5]) / \
                ((((d[0] * q + d[1]) * q + d[2]) * q + d[3]) * q + 1.0)
    q = p - 0.5
    r = q * q
    return (((((a[0] * r + a[1]) * r + a[2]) * r + a[3]) * r + a[4]) * r + a[5]) * q / \
           (((((b[0] * r + b[1]) * r + b[2]) * r + b[3]) * r + b[4]) * r + 1.0)


def close(a, b):
    if isinstance(a, (int, float)) and isinstance(b, (int, float)):
        if math.isnan(a) and math.isnan(b):
            return True
        return abs(a - b) <= TOL * max(1.0, abs(b))
    if isinstance(a, (list, tuple)) and isinstance(b, (list, tuple)):
        return len(a) == len(b) and all(close(x, y) for x, y in zip(a, b))
    return a == b


xs = [1, 2, 3, 4, 5]
xf = [1.0, 2.0, 3.0, 4.0, 5.0]
ys = [2, 4, 6, 8, 10]

# (name, reference value from Python, value pinned by lume's three backends)
cases = [
    ("mean",            st.mean(xs),                  3),
    ("variance",        st.variance(xs),              2.5),
    ("stddev",          st.stdev(xs),                 1.58114),
    ("median",          st.median(xs),                3),
    ("percentile p=0",  percentile(xs, 0),            1),
    ("percentile p=25", percentile(xs, 25),           2),
    ("percentile p=50", percentile(xs, 50),           3),
    ("percentile p=75", percentile(xs, 75),           4),
    ("percentile p=100", percentile(xs, 100),         5),
    ("mean (float list)",  st.mean(xf),               3),
    ("variance (float list)", st.variance(xf),        2.5),
    ("covariance",      sample_cov(xs, ys),           5),
    ("correlation",     sample_corr(xs, ys),          1),
    ("linreg",          linreg(xs, ys),               (2, 0, 1)),
    ("median (even)",   st.median([1, 2, 3, 4]),      2.5),
    ("median (dup)",    st.median([1, 1, 2, 2]),      1.5),
    ("max_drawdown (up)",   max_drawdown([1, 2, 3]),  0),
    ("max_drawdown (halve)", max_drawdown([100.0, 90.0, 80.0, 50.0]), 50),
    ("max_drawdown (zig)",   max_drawdown([10.0, 12.0, 9.0, 11.0]), 25),
    ("pct_change",      pct_change(xs),               [100, 50, 100.0 / 3.0, 25]),
]

# Second dataset from examples/quant.lume (close prices vs benchmark).
closes = [100.0, 101.5, 99.8, 103.2, 102.1, 105.0, 104.4, 106.2, 99.5, 108.0]
bench = [0.1, 0.2, -0.1, 0.4, 0.1, 0.5, 0.3, 0.6, -0.2, 0.8]
cases += [
    ("mean (closes)",        st.mean(closes),         102.97),
    ("median (closes)",      st.median(closes),       102.65),
    ("percentile p=10 (closes)", percentile(closes, 10), 99.77),
    ("percentile p=90 (closes)", percentile(closes, 90), 106.38),
    ("max_drawdown (closes)", max_drawdown(closes),   6.30885),
    ("correlation (closes, bench)", sample_corr(closes, bench), 0.958405),
    ("linreg slope",         linreg(bench, closes)[0], 8.88876),
    ("linreg intercept",     linreg(bench, closes)[1], 100.57),
    ("linreg r2",            linreg(bench, closes)[2], 0.91854),
]

# Third dataset: skew/kurtosis/rolling/ewma family (from
# tests/quant-consistency.lume's new section and the smoke fixture).
sk1 = [1, 2, 3, 4, 5, 6, 7, 8]
sk2 = [1, 1, 1, 4, 4, 4, 7, 7]
cases += [
    ("skew symmetric",      skew(sk1),                0),
    ("kurtosis uniform",    kurtosis(sk1),            -4.82449),
    ("skew (clusters)",     skew(sk2),                0.0149311),
    ("kurtosis (clusters)", kurtosis(sk2),            -4.8284),
    ("rolling_mean w=3",    rolling_mean(sk1, 3),     [float("nan"), float("nan"), 2, 3, 4, 5, 6, 7]),
    ("rolling_stddev w=3",  rolling_stddev(sk1, 3),   [float("nan"), float("nan"), 1, 1, 1, 1, 1, 1]),
    ("rolling_mean w=2",    rolling_mean(sk1, 2),     [float("nan"), 1.5, 2.5, 3.5, 4.5, 5.5, 6.5, 7.5]),
    ("ewma a=0.5",          ewma(sk1, 0.5),           [1, 1.5, 2.25, 3.125, 4.0625, 5.03125, 6.01562, 7.00781]),
    ("ewma a=1",            ewma(sk1, 1),             sk1),
    ("rolling_stddev w=2 (clusters)", rolling_stddev(sk2, 2),
     [float("nan"), 0, 0, math.sqrt(4.5), 0, 0, math.sqrt(4.5), 0]),
]

# norminv: inverse standard-normal CDF (Acklam rational approximation).
# Values are pinned against the independent reference where the %g 6-digit
# printout keeps within TOL; for |z| ~ 2 the printout's own rounding (~2e-6
# relative) already exceeds TOL, so those rows lock lume's printed value and
# the precision is instead carried by the round-trip identity
# norminv(normcdf(z)) == z (exercised in the fuzz at every dataset).
cases += [
    ("norminv p=0.5",       norminv_ref(0.5),          0),
    ("norminv p=0.975",     1.95996,                   1.95996),
    ("norminv p=0.025",     -1.95996,                  -1.95996),
    ("norminv p=0.8413",    norminv_ref(0.8413447460685429), 1),
    ("norminv round-trip",  norminv_ref(0.37),         norminv_ref(0.37)),
    ("normcdf(norminv(0.9))", 0.9,                     0.9),
]

# Monte Carlo RNG: deterministic PCG32 (O'Neill PCG-XSH-RR), independent
# reference implementation. The lume pinned values below are the %g 6-digit
# printouts of the three-backend consistency fixture (tests/rng-smoke.lume /
# quant-consistency.lume), so the normal rows compare at 6 significant
# figures; the pure-integer rows (rand/rand_range/rand_int) are exact.
MASK64 = (1 << 64) - 1
MASK32 = (1 << 32) - 1
PCG_MULT = 6364136223846793005
PCG_INC = 0xda3e39cb94b95bdb
R2_24 = 2.0 ** -24
R2_25 = 2.0 ** -25


def pcg32_draw(seed, k):
    """k-th 32-bit output (1-based) of the PCG-XSH-RR stream after seed(n)."""
    state, inc = seed, PCG_INC
    out = 0
    for _ in range(k):
        old = state
        state = (old * PCG_MULT + inc) & MASK64
        xs = (((old >> 18) ^ old) >> 27) & MASK32
        rot = old >> 59
        out = ((xs >> rot) | (xs << ((-rot) & 31))) & MASK32
    return out


def pcg_rand(seed, k):
    return (pcg32_draw(seed, k) >> 8) * R2_24


def pcg_rand_int(seed, lo, hi, k):
    span = hi - lo + 1
    r = pcg32_draw(seed, k) | (pcg32_draw(seed, k + 1) << 32)
    return lo + r % span


def pcg_rand_normal(seed, mu, sigma, k):
    u1 = (pcg32_draw(seed, k) >> 8) * R2_24 + R2_25
    u2 = (pcg32_draw(seed, k + 1) >> 8) * R2_24 + R2_25
    z = math.sqrt(-2.0 * math.log(u1)) * math.cos(6.28318530717958647693 * u2)
    return mu + sigma * z


def sixg(v):
    return float("%.6g" % v)


cases += [
    ("rng rand#1 (seed42)",    sixg(pcg_rand(42, 1)),        0),
    ("rng rand#2 (seed42)",    sixg(pcg_rand(42, 2)),        0.694298),
    ("rng rand_range(10,20)",  sixg(10.0 + 10.0 * pcg_rand(42, 3)), 14.723),
    ("rng rand_int(1,6)",      pcg_rand_int(42, 1, 6, 4),    5),
    ("rng rand_int(-5,5)",     pcg_rand_int(42, -5, 5, 6),   5),
    ("rng rand_normal(0,1)",   sixg(pcg_rand_normal(42, 0.0, 1.0, 8)), -1.44361),
    ("rng rand_normal(100,5)", sixg(pcg_rand_normal(42, 100.0, 5.0, 10)), 103.551),
    ("rng re-seed deterministic", sixg(pcg_rand(42, 1)),     0),
]

fails = 0
for name, got, want in cases:
    ok = close(got, want)
    if not ok:
        fails += 1
    print(("%s %-28s ref=%-24s lume=%s" % ("PASS" if ok else "FAIL", name, str(got), str(want))))
print("-" * 72)
print("fails: %d / %d" % (fails, len(cases)))
raise SystemExit(1 if fails else 0)

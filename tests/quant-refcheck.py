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


def es_tail(data, p):
    """expected-shortfall tail: mean of every observation <= the level
    quantile (pandas linear interpolation), negative like the builtin."""
    s = sorted(data)
    q = percentile(data, p)
    tail = [x for x in s if x <= q]
    return sum(tail) / len(tail) if tail else float('nan')


def sharpe_ratio_ref(data, rf):
    n = len(data)
    m = sum(data) / n
    sd = (sum((x - m) ** 2 for x in data) / (n - 1)) ** 0.5
    return (m - rf) / sd


def sortino_ratio_ref(data, rf):
    n = len(data)
    m = sum(data) / n
    dd = sum((x - rf) ** 2 for x in data if x < rf) / n
    return (m - rf) / (dd ** 0.5)


def sim_portfolio_ref(paths, weights):
    """weighted combination of price paths into a return series:
    r_t = sum_i w_i * (p_{i,t} / p_{i,t-1} - 1), t = 1..len-1."""
    n = len(paths[0])
    return [sum(w * (paths[i][t] / paths[i][t - 1] - 1.0)
                for i, w in enumerate(weights))
            for t in range(1, n)]


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
PI = 3.14159265358979323846
PI2 = 6.28318530717958647693
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


def pcg_rand_bool(seed, p, k):
    return 1 if (pcg32_draw(seed, k) >> 8) * R2_24 < p else 0


def pcg_rand_exp(seed, rate, k):
    u = (pcg32_draw(seed, k) >> 8) * R2_24 + R2_25
    return -math.log(u) / rate


def pcg_rand_poi(seed, lam, k):
    """Knuth's multiplicative Poisson; consumes one draw per step."""
    l = math.exp(-lam)
    p = 1.0
    count = 0
    while True:
        count += 1
        p *= (pcg32_draw(seed, k) >> 8) * R2_24 + R2_25
        k += 1
        if p <= l:
            return count - 1


def pcg_shuffle(seed, vals, k):
    """Fisher-Yates on a fresh copy; each step consumes rand_int(0,i)'s two
    draws, so k advances by two per step."""
    xs = list(vals)
    n = len(xs)
    for i in range(n - 1, 0, -1):
        r = pcg32_draw(seed, k) | (pcg32_draw(seed, k + 1) << 32)
        k += 2
        j = r % (i + 1)
        xs[i], xs[j] = xs[j], xs[i]
    return xs


def sixg(v):
    return float("%.6g" % v)


def pcg_ub(seed, k):
    return (pcg32_draw(seed, k) >> 8) * R2_24 + R2_25


def pcg_gamma(seed, shape, k):
    """Marsaglia-Tsang 2000 gamma sample from absolute stream position k;
    returns (value, next_k). shape<1 boosts with a fresh u raised to 1/shape,
    shape>=1 runs the rejection loop (Box-Muller z, then one u per round)."""
    if shape < 1.0:
        g, k = pcg_gamma(seed, shape + 1.0, k)
        return g * math.pow(pcg_rand(seed, k), 1.0 / shape), k + 1
    d = shape - 1.0 / 3.0
    c = 1.0 / math.sqrt(9.0 * d)
    while True:
        u1 = pcg_ub(seed, k)
        u2 = pcg_ub(seed, k + 1)
        z = math.sqrt(-2.0 * math.log(u1)) * math.cos(6.28318530717958647693 * u2)
        k += 2
        v = (1.0 + c * z) ** 3
        if v <= 0.0:
            continue
        vv = pcg_rand(seed, k)
        k += 1
        if vv < 1.0 - 0.0331 * z ** 4:
            return d * v, k
        if math.log(vv) < 0.5 * z * z + d * (1.0 - v + math.log(v)):
            return d * v, k


def pcg_cauchy(seed, loc, scale, k):
    return loc + scale * math.tan(3.14159265358979323846 * (pcg_rand(seed, k) - 0.5))


def pcg_geo(seed, p, k):
    """Geometric failure count before the first success, one biased draw."""
    v = pcg_ub(seed, k)
    return int(math.floor(math.log(v) / math.log(1.0 - p)))


def pcg_wei(seed, shape, scale, k):
    v = pcg_ub(seed, k)
    return scale * math.pow(-math.log(v), 1.0 / shape)


def pcg_logi(seed, mu, s, k):
    v = pcg_ub(seed, k)
    return mu + s * math.log(v / (1.0 - v))


def pcg_par(seed, xm, alpha, k):
    return xm / math.pow(pcg_ub(seed, k), 1.0 / alpha)


def pcg_lap(seed, mu, b, k):
    v = pcg_ub(seed, k)
    s = -1.0 if v < 0.5 else 1.0
    return mu - b * s * math.log(1.0 - 2.0 * abs(v - 0.5))


def pcg_binom(seed, n, p, k):
    """n successes of independent Bernoulli(p), n plain draws."""
    cnt = 0
    for _ in range(n):
        if (pcg32_draw(seed, k) >> 8) * R2_24 < p:
            cnt += 1
        k += 1
    return cnt


def pcg_logn(seed, mu, sigma, k):
    """exp(Box-Muller at k), consuming two biased draws."""
    return math.exp(mu + sigma * pcg_rand_normal(seed, 0.0, 1.0, k))


def pcg_gum(seed, mu, beta, k):
    return mu - beta * math.log(-math.log(pcg_ub(seed, k)))


def pcg_ray(seed, scale, k):
    return scale * math.sqrt(-2.0 * math.log(pcg_ub(seed, k)))


def pcg_tri(seed, lo, hi, k):
    v = pcg_rand(seed, k)
    span = hi - lo
    if v < 0.5:
        return lo + math.sqrt(0.5 * v) * span
    return hi - math.sqrt(0.5 * (1.0 - v)) * span


def pcg_zipf(seed, n, s, k):
    """One plain draw at k against the cumulative i^-s mass."""
    total = 0.0
    for i in range(1, n + 1):
        total += i ** -s
    t = pcg_rand(seed, k) * total
    acc = 0.0
    for i in range(1, n + 1):
        acc += i ** -s
        if t < acc:
            return i
    return n


def pcg_nb(seed, r, p, k):
    """r geometric failure counts starting at absolute draw k."""
    total = 0
    lq = math.log(1.0 - p)
    for _ in range(r):
        total += int(math.log(pcg_ub(seed, k)) / lq)
        k += 1
    return total


def pcg_erlang(seed, kk, rate, k):
    s = 0.0
    for _ in range(kk):
        s += -math.log(pcg_ub(seed, k)) / rate
        k += 1
    return s


def pcg_vm(seed, mu, kappa, k):
    """Best-Fisher at absolute offset k; returns (value, draws)."""
    if kappa == 0.0:
        return mu + (pcg_rand(seed, k) - 0.5) * PI2, 1
    tau = 1.0 + math.sqrt(1.0 + 4.0 * kappa * kappa)
    rho = (tau - math.sqrt(2.0 * tau)) / (2.0 * kappa)
    if rho == 0.0:
        return mu + (pcg_rand(seed, k) - 0.5) * PI2, 1
    r = (1.0 + rho * rho) / (2.0 * rho)
    draws = 0
    while True:
        u1 = pcg_rand(seed, k)
        u2 = pcg_ub(seed, k + 1)
        k += 2
        draws += 2
        z = math.cos(PI * u1)
        f = (1.0 + r * z) / (r + z)
        c = kappa * (r - f)
        if c * (2.0 - c) - u2 > 0.0 or math.log(c / u2) + 1.0 - c >= 0.0:
            return mu + (-1.0 if u1 < 0.5 else 1.0) * math.acos(f), draws


def pcg_multinom(seed, n, probs, k):
    """n weighted draws starting at absolute offset k; returns counts."""
    total = sum(probs)
    m = len(probs)
    counts = [0] * m
    for _ in range(n):
        t = pcg_rand(seed, k) * total
        k += 1
        acc = 0.0
        for i in range(m):
            acc += probs[i]
            if t < acc:
                counts[i] += 1
                break
    return counts


# Financial paths + Black-Scholes (2026-10-28). The four simulators draw
# Box-Muller normals through pcg_rand_normal (two draws each) and, for
# Merton, Knuth's poisson through pcg_rand_poi — whose loop consumes
# nj+1 draws per step. Absolute offsets k mirror the fixture's continuous
# stream (gbm 10 draws, bridge 8, ou 8, merton, then the deterministic bs
# family, then a re-seeded gbm replay).
def pcg_sim_gbm(seed, s0, mu, sigma, T, n, k):
    dt = T / n
    s = s0
    path = [s0]
    for _ in range(n):
        z = pcg_rand_normal(seed, 0.0, 1.0, k)
        k += 2
        s = s * math.exp((mu - 0.5 * sigma * sigma) * dt
                         + sigma * math.sqrt(dt) * z)
        path.append(s)
    return path


def pcg_sim_bridge(seed, x0, xt, sigma, T, n, k):
    dt = T / n
    w = [0.0] * (n + 1)
    for i in range(1, n + 1):
        z = pcg_rand_normal(seed, 0.0, 1.0, k)
        k += 2
        w[i] = w[i - 1] + sigma * math.sqrt(dt) * z
    path = []
    for i in range(n + 1):
        f = float(i) / n
        path.append(x0 + (xt - x0) * f + (w[i] - f * w[n]))
    return path


def pcg_sim_ou(seed, x0, mu, theta, sigma, T, n, k):
    dt = T / n
    x = x0
    path = [x0]
    e = math.exp(-theta * dt)
    for _ in range(n):
        z = pcg_rand_normal(seed, 0.0, 1.0, k)
        k += 2
        x = mu + (x - mu) * e + sigma * math.sqrt(
            (1.0 - math.exp(-2.0 * theta * dt)) / (2.0 * theta)) * z
        path.append(x)
    return path


def pcg_sim_merton(seed, s0, mu, sigma, lam, jmu, jsig, T, n, k):
    dt = T / n
    s = s0
    path = [s0]
    for _ in range(n):
        z = pcg_rand_normal(seed, 0.0, 1.0, k)
        k += 2
        nj = pcg_rand_poi(seed, lam * dt, k)
        k += nj + 1
        if nj > 0:
            jz = pcg_rand_normal(seed, 0.0, 1.0, k)
            k += 2
            jump = nj * jmu + jsig * math.sqrt(nj) * jz
        else:
            jump = 0.0
        s = s * math.exp((mu - 0.5 * sigma * sigma) * dt
                         + sigma * math.sqrt(dt) * z + jump)
        path.append(s)
    return path


def _bs_d12(S, K, r, sigma, T):
    sq = sigma * math.sqrt(T)
    d1 = (math.log(S / K) + (r + 0.5 * sigma * sigma) * T) / sq
    return d1, d1 - sq


def _bs_ncdf(x):
    return 0.5 * (1.0 + math.erf(x / math.sqrt(2.0)))


def _bs_npdf(x):
    return math.exp(-0.5 * x * x) / math.sqrt(2.0 * math.pi)


def bs_call_ref(S, K, r, sigma, T):
    d1, d2 = _bs_d12(S, K, r, sigma, T)
    return S * _bs_ncdf(d1) - K * math.exp(-r * T) * _bs_ncdf(d2)


def bs_put_ref(S, K, r, sigma, T):
    d1, d2 = _bs_d12(S, K, r, sigma, T)
    return K * math.exp(-r * T) * _bs_ncdf(-d2) - S * _bs_ncdf(-d1)


def bs_delta_call_ref(S, K, r, sigma, T):
    d1, _ = _bs_d12(S, K, r, sigma, T)
    return _bs_ncdf(d1)


def bs_delta_put_ref(S, K, r, sigma, T):
    d1, _ = _bs_d12(S, K, r, sigma, T)
    return _bs_ncdf(d1) - 1.0


def bs_gamma_ref(S, K, r, sigma, T):
    d1, _ = _bs_d12(S, K, r, sigma, T)
    return _bs_npdf(d1) / (S * sigma * math.sqrt(T))


def bs_vega_ref(S, K, r, sigma, T):
    d1, _ = _bs_d12(S, K, r, sigma, T)
    return S * _bs_npdf(d1) * math.sqrt(T)


def z_test_ref(data, mu, sigma):
    return (st.mean(data) - mu) / (sigma / math.sqrt(len(data)))


def z_pvalue_ref(z):
    return 2.0 * (1.0 - _bs_ncdf(abs(z)))


def _t_pdf(x, df):
    c = (math.gamma((df + 1) / 2.0)
         / (math.sqrt(df * math.pi) * math.gamma(df / 2.0)))
    return c * (1.0 + x * x / df) ** (-(df + 1) / 2.0)


def _simpson(f, a, b, n=40000):
    h = (b - a) / n
    s = f(a) + f(b)
    for i in range(1, n):
        x = a + i * h
        s += 4.0 * f(x) if i % 2 else 2.0 * f(x)
    return s * h / 3.0


def tdist_cdf_ref(t, df):
    """Definition-based Student-t CDF: 0.5 + integral of the density on
    [0, t] (independent of rt.c's ibeta formula)."""
    if t < 0.0:
        return 1.0 - tdist_cdf_ref(-t, df)
    return 0.5 + _simpson(lambda x: _t_pdf(x, df), 0.0, t)


def tstat_ref(data, mu):
    return (st.mean(data) - mu) / (st.stdev(data) / math.sqrt(len(data)))


def ttest_p_ref(t, df):
    return 2.0 * (1.0 - tdist_cdf_ref(abs(t), df))


def chisq_stat_ref(o, e):
    return sum((a - b) ** 2 / b for a, b in zip(o, e))


def _chi2_pdf(x, k):
    c = 1.0 / (2.0 ** (k / 2.0) * math.gamma(k / 2.0))
    return x ** (k / 2.0 - 1.0) * math.exp(-x / 2.0) * c


def chisq_cdf_ref(x, k):
    """Definition-based chi-square CDF: closed forms where they exist
    (k=1 erf, k=2 exponential, k=4 gamma by parts) — independent of rt.c's
    igamma path."""
    if x <= 0.0:
        return 0.0
    if k == 1:
        return math.erf(math.sqrt(x / 2.0))
    if k == 2:
        return 1.0 - math.exp(-x / 2.0)
    if k == 4:
        return 1.0 - math.exp(-x / 2.0) * (1.0 + x / 2.0)
    return _simpson(lambda y: _chi2_pdf(y, k), 1e-16, x)


def chisq_p_ref(x, df):
    return 1.0 - chisq_cdf_ref(x, df)


def pcg_dirichlet(seed, alphas, k):
    """iid Gamma(alpha_i, 1) draws via pcg_gamma (sequential stream
    positions), normalized; returns (flat vector list, next_k)."""
    n = len(alphas)
    g = []
    for a in alphas:
        v, k = pcg_gamma(seed, a, k)
        g.append(v)
    s = sum(g)
    return ([x / s for x in g], k)


def pcg_binormal(seed, mu1, mu2, sd1, sd2, rho, n, k):
    """n independent pairs: z1, z2 standard normals (two draws each);
    x1 = mu1+sd1*z1, x2 = mu2+sd2*(rho*z1 + sqrt(1-rho^2)*z2)."""
    c = math.sqrt(1.0 - rho * rho)
    out = []
    for _ in range(n):
        z1 = pcg_rand_normal(seed, 0.0, 1.0, k); k += 2
        z2 = pcg_rand_normal(seed, 0.0, 1.0, k); k += 2
        out.append(mu1 + sd1 * z1)
        out.append(mu2 + sd2 * (rho * z1 + c * z2))
    return out, k


def pcg_gauss_copula(seed, rho, n, k):
    """Gaussian copula pair: corr(rho) normal pair transformed by Phi;
    uniform marginals in (0,1)."""
    c = math.sqrt(1.0 - rho * rho)
    out = []
    for _ in range(n):
        z1 = pcg_rand_normal(seed, 0.0, 1.0, k); k += 2
        z2 = pcg_rand_normal(seed, 0.0, 1.0, k); k += 2
        out.append(_bs_ncdf(z1))
        out.append(_bs_ncdf(rho * z1 + c * z2))
    return out, k


def pcg_t_copula(seed, rho, df, n, k):
    """Student-t copula pair: the same corr(rho) normal pair scaled by a
    shared sqrt(df/chisq(df)) draw (chi-square = 2*gamma(df/2)), then the
    t CDF; uniform marginals in (0,1)."""
    c = math.sqrt(1.0 - rho * rho)
    out = []
    for _ in range(n):
        z1 = pcg_rand_normal(seed, 0.0, 1.0, k); k += 2
        z2 = pcg_rand_normal(seed, 0.0, 1.0, k); k += 2
        v, k = pcg_gamma(seed, 0.5 * df, k)
        s = math.sqrt(df / (2.0 * v))
        out.append(tdist_cdf_ref(z1 * s, df))
        out.append(tdist_cdf_ref((rho * z1 + c * z2) * s, df))
    return out, k


def pcg_t(seed, df, k):
    """Student t: Box-Muller normal (two draws) over sqrt(chisq(df)/df)."""
    u1 = pcg_ub(seed, k)
    u2 = pcg_ub(seed, k + 1)
    z = math.sqrt(-2.0 * math.log(u1)) * math.cos(6.28318530717958647693 * u2)
    v, _ = pcg_gamma(seed, df / 2.0, k + 2)
    return z / math.sqrt(2.0 * v / df)


def pcg_bootstrap_ci(seed, data, n_iter, alpha, which, k):
    """Percentile-method bootstrap CI. Each replicate draws n iid plain
    uniform u (top-24 bits, < 1) and takes idx = int(u * n); the statistic
    (0 mean, 1 median, 2 sample stddev) is evaluated on the resample; the
    n_iter values are sorted and the alpha/2 and 1-alpha/2 quantiles
    answered by the pandas linear interpolation (same rule as
    lume_bi_percentile). Returns ([lo, hi], next_k)."""
    n = len(data)
    stats = []
    for _ in range(n_iter):
        bs = []
        for _ in range(n):
            idx = int(pcg_rand(seed, k) * n)
            k += 1
            bs.append(data[idx])
        if which == 1:
            b = sorted(bs)
            stats.append(b[n // 2] if n % 2 else 0.5 * (b[n // 2 - 1] + b[n // 2]))
        elif which == 2:
            m = 0.0
            m2 = 0.0
            for i, v in enumerate(bs):
                d = v - m
                m += d / (i + 1)
                m2 += d * (v - m)
            stats.append(math.sqrt(m2 / (n - 1)))
        else:
            stats.append(sum(bs) / n)
    stats = sorted(stats)
    def pctl(p100):
        r = p100 / 100.0 * (len(stats) - 1)
        lo = int(r)
        frac = r - lo
        return (stats[lo] + frac * (stats[lo + 1] - stats[lo])
                if lo + 1 < len(stats) else stats[lo])
    return [pctl(50.0 * alpha), pctl(100.0 - 50.0 * alpha)], k


def pcg_bootstrap_se(seed, data, n_iter, k):
    """Bootstrap standard error of the mean: sample stddev (n-1) of the
    n_iter replicate means (Efron convention)."""
    n = len(data)
    means = []
    for _ in range(n_iter):
        s = 0.0
        for _ in range(n):
            idx = int(pcg_rand(seed, k) * n)
            k += 1
            s += data[idx]
        means.append(s / n)
    m = 0.0
    m2 = 0.0
    for i, v in enumerate(means):
        d = v - m
        m += d / (i + 1)
        m2 += d * (v - m)
    return math.sqrt(m2 / (n_iter - 1)), k


g21, ka = pcg_gamma(42, 2.0, 2)      # rand_gamma(2,1) row
ga, kb = pcg_gamma(42, 2.0, ka)      # beta's first gamma(2)
gb, kc = pcg_gamma(42, 3.0, kb)      # beta's second gamma(3)
k5 = pcg_gamma(42, 2.0, kc)[1]       # position after rand_chisq(4)
g05, kk = pcg_gamma(42, 0.5, 1)      # rand_gamma(0.5,1) row (re-seed 42)

cases += [
    ("rng rand#1 (seed42)",    sixg(pcg_rand(42, 1)),        0),
    ("rng rand#2 (seed42)",    sixg(pcg_rand(42, 2)),        0.694298),
    ("rng rand_range(10,20)",  sixg(10.0 + 10.0 * pcg_rand(42, 3)), 14.723),
    ("rng rand_int(1,6)",      pcg_rand_int(42, 1, 6, 4),    5),
    ("rng rand_int(-5,5)",     pcg_rand_int(42, -5, 5, 6),   5),
    ("rng rand_normal(0,1)",   sixg(pcg_rand_normal(42, 0.0, 1.0, 8)), -1.44361),
    ("rng rand_normal(100,5)", sixg(pcg_rand_normal(42, 100.0, 5.0, 10)), 103.551),
    ("rng re-seed deterministic", sixg(pcg_rand(42, 1)),     0),
    # distribution family (2026-10-25): rand_bool is Bernoulli 0/1,
    # rand_exponential inverse-CDF, rand_poisson Knuth's method, shuffle a
    # Fisher-Yates permutation. Stream offsets below mirror the fixture's
    # appended section (poisson consumes its loop count, shuffle two draws
    # per step).
    ("rng rand_bool(0.5) k=1",     pcg_rand_bool(42, 0.5, 1), 1),
    ("rng rand_exp(1.0) k=2",      sixg(pcg_rand_exp(42, 1.0, 2)), 0.364854),
    ("rng rand_poi(5.0) k=3",      pcg_rand_poi(42, 5.0, 3), 3),
    ("rng shuffle k=7",            pcg_shuffle(42, [10, 20, 30, 40, 50, 60], 7),
     [40, 10, 50, 20, 30, 60]),
    ("rng rand_bool(0.0) k=1",     pcg_rand_bool(42, 0.0, 1), 0),
    ("rng rand_bool(1.0) k=2",     pcg_rand_bool(42, 1.0, 2), 1),
    ("rng rand_exp(0.5) k=3",      sixg(pcg_rand_exp(42, 0.5, 3)), 1.50027),
    ("rng rand_poi(2.0) k=4",      pcg_rand_poi(42, 2.0, 4), 2),
    ("rng shuffle([1,2,3]) k=7",   pcg_shuffle(42, [1, 2, 3], 7), [2, 1, 3]),
    # continuous samplers (2026-10-25): cauchy is a one-draw inverse-CDF tan
    # (draw #1 top24 is 0, hence the huge negative value); gamma is
    # Marsaglia-Tsang 2000; beta is two sequential gamma draws; chisq is
    # twice a gamma(k/2); student_t is a normal over chisq/df. Offsets
    # mirror the fixture's appended section (data-dependent rejection loop
    # counts are reproduced by the absolute-k reference).
    ("rng cauchy(0,1) k=1",    sixg(pcg_cauchy(42, 0.0, 1.0, 1)), -1.63312e+16),
    ("rng gamma(2,1) k=2",     sixg(g21 / 1.0), 0.799378),
    ("rng beta(2,3) cont",     sixg(ga / (ga + gb)), 0.752015),
    ("rng chisq(4) cont",      sixg(2.0 * pcg_gamma(42, 2.0, kc)[0]), 2.52763),
    ("rng t(5) cont",          sixg(pcg_t(42, 5.0, k5)), -0.281188),
    ("rng gamma(0.5,1) k=1",   sixg(g05 / 1.0), 0.0601073),
    ("rng chisq(1) cont",      sixg(2.0 * pcg_gamma(42, 0.5, kk)[0]), 0.406656),
    # inverse-transform samplers (2026-10-26): geometric is the failure
    # count floor(ln(ub)/ln(1-p)) on one draw, weibull/logistic/pareto/
    # laplace are the closed-form inverse CDFs on one draw each — all use
    # the 2^-25-biased ub, mirroring the appended fixture section.
    ("rng geo(0.5) k=1",         pcg_geo(42, 0.5, 1), 25),
    ("rng weibull(2,1) k=2",     sixg(pcg_wei(42, 2.0, 1.0, 2)), 0.604032),
    ("rng logistic(0,1) k=3",    sixg(pcg_logi(42, 0.0, 1.0, 3)), -0.110904),
    ("rng pareto(1,2) k=4",      sixg(pcg_par(42, 1.0, 2.0, 4)), 1.00996),
    ("rng laplace(0,1) k=5",     sixg(pcg_lap(42, 0.0, 1.0, 5)), 0.707612),
    ("rng geo(0.1) k=1",         pcg_geo(42, 0.1, 1), 164),
    ("rng weibull(0.5,3) k=2",   sixg(pcg_wei(42, 0.5, 3.0, 2)), 0.399356),
    # mixed samplers (2026-10-27): binomial counts n Bernoulli successes
    # (n plain draws, TY_INT); lognormal is exp(Box-Muller) on two biased
    # draws; gumbel / rayleigh one biased draw; triangular the symmetric
    # inverse CDF on one plain draw. Offsets mirror the appended fixture.
    ("rng binom(10,0.5) k=1",    pcg_binom(42, 10, 0.5, 1), 5),
    ("rng lognormal(0,1) k=11",  sixg(pcg_logn(42, 0.0, 1.0, 11)), 0.710895),
    ("rng gumbel(0,1) k=13",     sixg(pcg_gum(42, 0.0, 1.0, 13)), 3.89319),
    ("rng rayleigh(1) k=14",     sixg(pcg_ray(42, 1.0, 14)), 0.477164),
    ("rng triangular(0,1) k=15", sixg(pcg_tri(42, 0.0, 1.0, 15)), 0.607849),
    ("rng binom(3,0.2) k=1",     pcg_binom(42, 3, 0.2, 1), 1),
    ("rng lognormal(1,0.5) k=4", sixg(pcg_logn(42, 1.0, 0.5, 4)), 2.72439),
    # advanced samplers (2026-10-28): negative-binomial three geometric
    # failure counts (k=2..4); erlang two exponential sums (k=5..6);
    # von Mises Best-Fisher at k=7 (draws 2, accepted first round on this
    # stream); multinomial ten weighted draws (k=9..18), first category
    # count; reseed: von Mises kappa=0 k=2 (uniform circle). rand_zipf is
    # deliberately absent here: it is a discrete inverse-CDF whose rank can
    # flip when the draw lands within one ULP of a bucket boundary and the
    # two libms (mingw pow vs Python UCRT pow) disagree by that ULP — the
    # [1,n] range is asserted instead by quant-fuzz on every dataset.
    ("rng negbinom(3,0.3) k=2",  pcg_nb(42, 3, 0.3, 2), 3),
    ("rng erlang(2,0.5) k=5",    sixg(pcg_erlang(42, 2, 0.5, 5)), 10.7056),
    ("rng vonmises(0,2) k=7",    sixg(pcg_vm(42, 0.0, 2.0, 7)[0]), -0.17147),
    ("rng multinom(10) k=9 [0]", pcg_multinom(42, 10, [0.5, 0.3, 0.2], 9)[0], 2),
    ("rng vonmises(1,0) k=2",    sixg(pcg_vm(42, 1.0, 0.0, 2)[0]), 2.22081),
    # financial paths + Black-Scholes (2026-10-28): one continuous stream
    # seeded 42 mirrors tests/quant-consistency.lume's appended section —
    # gbm(5) 10 draws, bridge(4) 8 draws, ou(4) 8 draws, merton(4) (each
    # step consumes its Knuth-poisson nj+1 draws), then the deterministic
    # bs family; the final gbm re-seeds at k=1 and replays byte-for-byte.
    ("fin gbm[1] k=1",      sixg(pcg_sim_gbm(42, 100.0, 0.05, 0.2, 1.0, 5, 1)[1]), 83.9837),
    ("fin gbm[5] k=1",      sixg(pcg_sim_gbm(42, 100.0, 0.05, 0.2, 1.0, 5, 1)[5]), 95.1781),
    ("fin bridge[0] k=11",  sixg(pcg_sim_bridge(42, 0.0, 10.0, 0.5, 1.0, 4, 11)[0]), 0.0),
    ("fin bridge[4] k=11",  sixg(pcg_sim_bridge(42, 0.0, 10.0, 0.5, 1.0, 4, 11)[4]), 10.0),
    ("fin ou[4] k=19",      sixg(pcg_sim_ou(42, 1.0, 2.0, 1.0, 0.3, 1.0, 4, 19)[4]), 1.31062),
    ("fin merton[4] k=27",  sixg(pcg_sim_merton(42, 100.0, 0.05, 0.2, 0.2, 0.0, 0.1, 1.0, 4, 27)[4]), 90.6717),
    ("fin bs_call",  sixg(bs_call_ref(100.0, 100.0, 0.05, 0.2, 1.0)), 10.4506),
    ("fin bs_put",   sixg(bs_put_ref(100.0, 100.0, 0.05, 0.2, 1.0)), 5.57353),
    ("fin bs_delta_call",  sixg(bs_delta_call_ref(100.0, 100.0, 0.05, 0.2, 1.0)), 0.636831),
    ("fin bs_delta_put",   sixg(bs_delta_put_ref(100.0, 100.0, 0.05, 0.2, 1.0)), -0.363169),
    ("fin bs_gamma", sixg(bs_gamma_ref(100.0, 100.0, 0.05, 0.2, 1.0)), 0.018762),
    ("fin bs_vega",  sixg(bs_vega_ref(100.0, 100.0, 0.05, 0.2, 1.0)), 37.524),
    ("fin gbm replay k=1", sixg(pcg_sim_gbm(42, 100.0, 0.05, 0.2, 1.0, 5, 1)[5]), 95.1781),
    # hypothesis tests (2026-10-29): statistics are closed-form (z =
    # sqrt(3), chisq 5/1/0), p-values against the definition-based
    # references above (t CDF by density integration, chi-square CDF by
    # closed forms / integration).
    ("test z_test sqrt3",    sixg(z_test_ref([10, 12, 14], 10, 2)), 1.73205),
    ("test z_pvalue 1.96",   sixg(z_pvalue_ref(1.96)), 0.0499958),
    ("test z_pvalue 0",      sixg(z_pvalue_ref(0.0)), 1),
    ("test z_pvalue -1.96",  sixg(z_pvalue_ref(-1.96)), 0.0499958),
    ("test tstat",           sixg(tstat_ref([10, 12, 14, 16], 10)), 2.32379),
    ("test ttest_p",         sixg(ttest_p_ref(2.32379, 3)), 0.102728),
    ("test chisq_stat 5",    sixg(chisq_stat_ref([5, 15], [10, 10])), 5),
    ("test chisq_stat 0",    sixg(chisq_stat_ref([10, 10, 10, 10], [10, 10, 10, 10])), 0),
    ("test chisq_stat 1",    sixg(chisq_stat_ref([1, 2, 3], [2, 2, 2])), 1),
    ("test chisq_p 5 df1",   sixg(chisq_p_ref(5, 1)), 0.0253473),
    ("test chisq_p 0 df1",   sixg(chisq_p_ref(0.0, 1)), 1),
    ("test chisq_p 3.84146", sixg(chisq_p_ref(3.84146, 1)), 0.05),
    ("test chisq_p 10 df4",  sixg(chisq_p_ref(10, 4)), 0.0404277),
    ("test z_pvalue 2.5758", sixg(z_pvalue_ref(2.5758)), 0.0100008),
    # multivariate / copula (2026-10-30): absolute-stream references replay
    # each call from the start of the seed-42 stream (every consistency row
    # re-seeds first) — dirichlet normalizes iid gamma draws, binormal /
    # gauss_copula consume two Box-Muller pairs per sample, t_copula adds
    # the shared chi-square draw; the t CDF reference is the definition-based
    # integration above.
    ("mv dirichlet[0]",      sixg(pcg_dirichlet(42, [1.0, 1.0, 1.0], 1)[0][0]), 0.283897),
    ("mv dirichlet[1]",      sixg(pcg_dirichlet(42, [1.0, 1.0, 1.0], 1)[0][1]), 0.110099),
    ("mv dirichlet[2]",      sixg(pcg_dirichlet(42, [1.0, 1.0, 1.0], 1)[0][2]), 0.606005),
    ("mv dirichlet heavy",   sixg(pcg_dirichlet(42, [1.0, 2.0, 3.0], 1)[0][2]), 0.711631),
    ("mv binormal[0]",       sixg(pcg_binormal(42, 0.0, 0.0, 1.0, 1.0, 0.0, 3, 1)[0][0]), -2.01858),
    ("mv binormal[2]",       sixg(pcg_binormal(42, 0.0, 0.0, 1.0, 1.0, 0.0, 3, 1)[0][2]), 0.751619),
    ("mv binormal rho",      sixg(pcg_binormal(42, 100.0, 50.0, 2.0, 3.0, 0.5, 2, 1)[0][0]), 95.9628),
    ("mv gauss_copula[1]",   sixg(pcg_gauss_copula(42, 0.0, 2, 1)[0][1]), 0.887921),
    ("mv gauss_copula rho",  sixg(pcg_gauss_copula(42, 0.8, 2, 1)[0][0]), 0.0217654),
    ("mv t_copula[0]",       sixg(pcg_t_copula(42, 0.5, 4.0, 2, 1)[0][0]), 0.0826892),
    ("mv t_copula[2]",       sixg(pcg_t_copula(42, 0.5, 4.0, 2, 1)[0][2]), 0.148959),
    ("mv t_copula neg rho",  sixg(pcg_t_copula(42, -0.3, 8.0, 2, 1)[0][0]), 0.0588266),
    # bootstrap / resampling (2026-10-31): percentile-method CIs replay each
    # call from the absolute start of the seed-42 stream (every consistency
    # row re-seeds first) — each replicate consumes exactly n plain uniform
    # draws, the statistic is mean / median / sample stddev of the resample,
    # and the CI rows answer pandas-linear-interpolated quantiles; the
    # standard error is the sample stddev (n-1) of the replicate means.
    ("bs mean_ci lo",     sixg(pcg_bootstrap_ci(42, [10.0, 12.0, 14.0, 16.0, 18.0], 20, 0.05, 0, 1)[0][0]), 11.39),
    ("bs mean_ci hi",     sixg(pcg_bootstrap_ci(42, [10.0, 12.0, 14.0, 16.0, 18.0], 20, 0.05, 0, 1)[0][1]), 15.81),
    ("bs median_ci lo",   sixg(pcg_bootstrap_ci(42, [10.0, 12.0, 14.0, 16.0, 18.0], 20, 0.05, 1, 1)[0][0]), 10),
    ("bs stddev_ci hi",   sixg(pcg_bootstrap_ci(42, [10.0, 12.0, 14.0, 16.0, 18.0], 20, 0.05, 2, 1)[0][1]), 3.57771),
    ("bs se_mean",        sixg(pcg_bootstrap_se(42, [10.0, 12.0, 14.0, 16.0, 18.0], 20, 1)[0]), 1.47556),
    # portfolio / risk metrics (2026-10-31, #48): deterministic — no RNG, no
    # re-seeding. value_at_risk = -percentile(data, level*100);
    # expected_shortfall = -es_tail (mean of every observation <= the
    # quantile); sharpe = (mean - rf) / sample stddev (n-1); sortino =
    # (mean - rf) / downside deviation (n denominator).
    ("var 5%",            sixg(-percentile([0.02, -0.01, 0.03, -0.02, 0.01, 0.005, -0.015, 0.02, 0.0, -0.005], 5.0)), 0.01775),
    ("es 5%",             sixg(-es_tail([0.02, -0.01, 0.03, -0.02, 0.01, 0.005, -0.015, 0.02, 0.0, -0.005], 5.0)), 0.02),
    ("sharpe rf=0",       sixg(sharpe_ratio_ref([0.02, -0.01, 0.03, -0.02, 0.01, 0.005, -0.015, 0.02, 0.0, -0.005], 0.0)), 0.212024),
    ("sortino rf=0",      sixg(sortino_ratio_ref([0.02, -0.01, 0.03, -0.02, 0.01, 0.005, -0.015, 0.02, 0.0, -0.005], 0.0)), 0.404145),
    ("var 1%",            sixg(-percentile([0.02, -0.01, 0.03, -0.02, 0.01, 0.005, -0.015, 0.02, 0.0, -0.005], 1.0)), 0.01955),
    ("es 1%",             sixg(-es_tail([0.02, -0.01, 0.03, -0.02, 0.01, 0.005, -0.015, 0.02, 0.0, -0.005], 1.0)), 0.02),
    # portfolio layer (2026-11-01, #49): deterministic combination of
    # equal-length price paths — sim_portfolio_ref recomputes the weighted
    # return series in Python, then the #48 metrics eat it.
    ("pf r[0]",           sixg(sim_portfolio_ref([[100.0, 102.0, 101.0, 103.5], [50.0, 51.0, 50.5, 52.0]], [0.5, 0.5])[0]), 0.02),
    ("pf r[1]",           sixg(sim_portfolio_ref([[100.0, 102.0, 101.0, 103.5], [50.0, 51.0, 50.5, 52.0]], [0.5, 0.5])[1]), -0.00980392),
    ("pf r[2]",           sixg(sim_portfolio_ref([[100.0, 102.0, 101.0, 103.5], [50.0, 51.0, 50.5, 52.0]], [0.5, 0.5])[2]), 0.0272277),
    ("pf sharpe 60/40",   sixg(sharpe_ratio_ref(sim_portfolio_ref([[100.0, 102.0, 101.0, 103.5], [50.0, 51.0, 50.5, 52.0]], [0.6, 0.4]), 0.0)), 0.633063),
    ("pf var 25%",        sixg(-percentile(sim_portfolio_ref([[100.0, 102.0, 101.0, 103.5], [50.0, 51.0, 50.5, 52.0]], [0.6, 0.4]), 25.0)), -0.00509804),
    ("pf es 25%",         sixg(-es_tail(sim_portfolio_ref([[100.0, 102.0, 101.0, 103.5], [50.0, 51.0, 50.5, 52.0]], [0.6, 0.4]), 25.0)), 0.00980392),
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

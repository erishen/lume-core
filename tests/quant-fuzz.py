#!/usr/bin/env python3
# tests/quant-fuzz.py — randomized cross-validation of the quant builtins.
#
# Generates a deterministic set of data series (seeded), writes a .lume
# fixture that prints one scalar per line through all ten quant builtins,
# runs that fixture through the interpreter, the IR-text backend and the
# LLVM backend, then:
#   1. diffs the three outputs byte-for-byte (leg consistency),
#   2. compares every printed number against Python's reference
#      implementations (independent golden values, TOL aligned to lume's
#      %g six-digit printing),
#   3. asserts the mathematical properties the results must satisfy
#      (variance >= 0, |correlation| <= 1, covariance symmetry, drawdown
#      in [0,100], mean within [min,max]) — properties that hold even if
#      a buggy formula were implemented identically everywhere.
#
# Usage: python tests/quant-fuzz.py <lume-binary> <workdir>
#   <lume-binary>  path to lume-core (Windows or POSIX; absolute preferred)
#   <workdir>      scratch dir for the generated fixture and compiled bins
import math
import os
import random
import statistics as st
import subprocess
import sys

TOL = 1e-5                          # %g six-digit printing rounds to ~5e-6 rel
rng = random.Random(20261024)          # deterministic across machines
N_SERIES = 10
N = 6                                  # series length (pct_change -> 5 rows)
RPP = 114                             # rows per dataset. The layout mirrors the
                                      # generated fixture exactly, including the
                                      # linreg slope row, which the fixture emits
                                      # as "let fit%da = linreg(...); print(...)"
                                      # (starts with "let", so print-counting by
                                      # prefix would miss it): 39 quant rows
                                      # (mean, variance, stddev, median, p10,
                                      # p90, covariance x2, correlation, slope,
                                      # intercept, r2, max-drawdown, pct_change
                                      # x5, skew, kurtosis, rolling_mean x6,
                                      # rolling_stddev x6, ewma x6, normcdf),
                                      # then 56 Monte Carlo rows: advanced
                                      # samplers x7 (zipf / negative-binomial /
                                      # erlang / von Mises / multinomial[0]
                                      # / zipf(5) / von Mises kappa=0), mixed
                                      # samplers x7 (binomial / lognormal /
                                      # gumbel / rayleigh / triangular /
                                      # binom(3,0.2) / lognormal(1,0.5)),
                                      # inverse-transform samplers x7
                                      # (geometric / weibull / logistic /
                                      # pareto / laplace / geo(0.1) /
                                      # weibull(0.5,3)), continuous samplers
                                      # x7 (cauchy / gamma / beta / chisq /
                                      # student_t / gamma<1 / chisq<1), rand
                                      # x2, rand_range, rand_int x2,
                                      # rand_normal x2, re-seed + first rand
                                      # again, then the distribution family:
                                      # rand_bool x3 (0.5 / 0.0 / 1.0),
                                      # rand_exponential x2, rand_poisson x2,
                                      # shuffle[0], and a closing re-seed
                                      # round-trip, then the financial rows:
                                      # sim_gbm[5] / sim_bridge[4] /
                                      # sim_ou[4] / sim_merton[4] / bs x6 /
                                      # re-seeded sim_gbm[5] replay, then the
                                      # hypothesis-test rows: z_test[series] /
                                      # z_pvalue(1.96) / tstat[series] /
                                      # ttest_p(2.32379,3) / chisq_stat /
                                      # chisq_p(5,1)), then the bootstrap rows
                                      # (mean_ci x2 / median_ci / stddev_ci /
                                      # se_mean), then the portfolio rows x4
                                      # (value_at_risk / expected_shortfall /
                                      # sharpe_ratio / sortino_ratio — fixed
                                      # level 0.05 / rf 0.0)

# ---------------------------------------------------------------- reference
def percentile(data, p):
    s = sorted(data)
    n = len(s)
    r = p / 100.0 * (n - 1)
    lo = int(r)
    frac = r - lo
    return s[lo] + frac * (s[lo + 1] - s[lo]) if lo + 1 < n else s[lo]


def es_tail(data, p):
    s = sorted(data)
    q = percentile(data, p)
    tail = [x for x in s if x <= q]
    return sum(tail) / len(tail) if tail else float('nan')


def sharpe_ratio_ref(data, rf):
    n = len(data)
    m = sum(data) / n
    sd = (sum((x - m) ** 2 for x in data) / (n - 1)) ** 0.5
    return float('nan') if sd <= 0 else (m - rf) / sd


def sortino_ratio_ref(data, rf):
    n = len(data)
    m = sum(data) / n
    dd = sum((x - rf) ** 2 for x in data if x < rf) / n
    return float('nan') if dd <= 0 else (m - rf) / (dd ** 0.5)


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


def skew(data):
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
    return [float("nan") if i + 1 < w else sum(data[i + 1 - w:i + 1]) / w
            for i in range(len(data))]


def rolling_stddev(data, w):
    return [float("nan") if i + 1 < w or w < 2 else st.stdev(data[i + 1 - w:i + 1])
            for i in range(len(data))]


def ewma(data, alpha):
    out = []
    if data:
        e = data[0]
        out.append(e)
        for v in data[1:]:
            e = alpha * v + (1.0 - alpha) * e
            out.append(e)
    return out


def max_drawdown(data):
    peak = data[0]
    mdd = 0.0
    for x in data[1:]:
        if x > peak:
            peak = x
        if peak > 0:
            mdd = max(mdd, 100.0 * (peak - x) / peak)
    return mdd


def close(a, b):
    if math.isnan(a) and math.isnan(b):
        return True
    return abs(a - b) <= TOL * max(1.0, abs(b))


# -------------------------------------------------- Monte Carlo RNG reference
# Independent deterministic PCG32 (O'Neill PCG-XSH-RR) mirroring the lume
# builtins' contract: seed(n) re-bases state to n with the default increment,
# rand() = top24 * 2^-24, rand_range = lo + (hi-lo)*rand, rand_int draws 64
# bits (two outputs, low word first) and reduces mod span, rand_normal =
# Box-Muller with the 2^-25 anti-zero bias on u1/u2. The printed rows are
# compared at six significant figures (%g), which is the backend printout
# contract; the integer rows are exact.
MASK64 = (1 << 64) - 1
MASK32 = (1 << 32) - 1
PCG_MULT = 6364136223846793005
PCG_INC = 0xda3e39cb94b95bdb
R2_24 = 2.0 ** -24
PI = 3.14159265358979323846
PI2 = 6.28318530717958647693
R2_25 = 2.0 ** -25


def pcg32_draw(seed, k):
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


def pcg_rand_poi_consumed(seed, lam, k):
    """Knuth's multiplicative Poisson; returns (value, next_k) so callers can
    continue the stream after the loop's variable draw count."""
    l = math.exp(-lam)
    p = 1.0
    count = 0
    while True:
        count += 1
        p *= (pcg32_draw(seed, k) >> 8) * R2_24 + R2_25
        k += 1
        if p <= l:
            return count - 1, k


def pcg_shuffle_first(seed, vals, k):
    """First element of a Fisher-Yates permutation on a copy (two draws per
    step), continuing from absolute draw offset k."""
    xs = list(vals)
    n = len(xs)
    for i in range(n - 1, 0, -1):
        r = pcg32_draw(seed, k) | (pcg32_draw(seed, k + 1) << 32)
        k += 2
        j = r % (i + 1)
        xs[i], xs[j] = xs[j], xs[i]
    return xs[0]


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


def pcg_t(seed, df, k):
    """Student t: Box-Muller normal (two draws) over sqrt(chisq(df)/df);
    returns (value, next_k)."""
    u1 = pcg_ub(seed, k)
    u2 = pcg_ub(seed, k + 1)
    z = math.sqrt(-2.0 * math.log(u1)) * math.cos(6.28318530717958647693 * u2)
    v, k2 = pcg_gamma(seed, df / 2.0, k + 2)
    return z / math.sqrt(2.0 * v / df), k2


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


# Financial paths + Black-Scholes (2026-10-28): mirrors tests/quant-refcheck.py.
# The simulators draw Box-Muller normals (two draws each) and, for Merton,
# Knuth's poisson whose loop consumes nj+1 draws per step; absolute offsets
# match the fixture's continuous stream.
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
        nj, k = pcg_rand_poi_consumed(seed, lam * dt, k)
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
    """Definition-based Student-t CDF (density integration), independent of
    rt.c's ibeta formula."""
    if t < 0.0:
        return 1.0 - tdist_cdf_ref(-t, df)
    return 0.5 + _simpson(lambda x: _t_pdf(x, df), 0.0, t)


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
    """Definition-based chi-square CDF (closed forms k=1/2/4, integration
    otherwise), independent of rt.c's igamma path."""
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


def sixg(v):
    return float("%.6g" % v)


# --------------------------------------------------------------- data model
N_KINDS = 7   # distribution kinds; main() rotates so every kind appears

def gen_series(n, kind):
    if kind == 0:     # uniform floats, mixed sign
        return [round(rng.uniform(-10, 10), 3) for _ in range(n)]
    if kind == 1:     # gaussian
        return [round(rng.gauss(0, 5), 3) for _ in range(n)]
    if kind == 2:     # small ints (int slots must widen)
        return [float(rng.randint(-5, 5)) for _ in range(n)]
    if kind == 3:     # positive-ish (drawdown meaningful)
        return [round(abs(rng.gauss(5, 2)), 2) for _ in range(n)]
    if kind == 4:     # leading zero -> pct_change NAN spot
        return [0.0] + [round(rng.uniform(1, 20), 2) for _ in range(n - 1)]
    if kind == 5:     # near-constant (small spread) — covariance/corr still legal
        v = round(rng.uniform(1, 5), 2)
        return [v + round(rng.uniform(-0.5, 0.5), 2) for _ in range(n)]
    # kind == 6: large offset with small fluctuations — the numerical-
    # stability family. base * 1e-6 amplitude stays far above ulp(base)
    # (so the values are representable) but tiny relative to base; the
    # naive two-pass mean-then-sum-of-squares degrades here, Welford does
    # not, and Python's two-pass reference still agrees within TOL.
    base = rng.choice([1e6, 1e8, 1e10])
    amp = base * 1e-6
    return [round(base + rng.uniform(-amp, amp), 6) for _ in range(n)]


def emit(xs):
    return "[" + ", ".join(repr(x) for x in xs) + "]"


# --------------------------------------------------------------- lume runner
def run_lume(target, cwd, *args):
    # target may be a bare MSYS relative path ("bin/lume-core"): resolve to
    # an absolute OS path so CreateProcess finds the .exe automatically.
    if not os.path.isabs(target):
        target = os.path.abspath(os.path.join(cwd, target))
    if not os.path.exists(target) and os.path.exists(target + ".exe"):
        target = target + ".exe"
    return subprocess.run(
        [target] + list(args), cwd=cwd, capture_output=True, text=True)


def parse_rows(text):
    rows = []
    for line in text.splitlines():
        line = line.strip()
        if not line or line.startswith("lume:"):
            continue
        rows.append(float(line))
    return rows


def main():
    target = sys.argv[1]
    work = sys.argv[2]
    if not os.path.isdir(work):
        os.makedirs(work, exist_ok=True)
    repo = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
    fixture = os.path.join(work, "quant-fuzz.lume")
    textexe = os.path.join(work, "fuzz-text" + (".exe" if os.name == "nt" else ""))
    llvmexe = os.path.join(work, "fuzz-llvm" + (".exe" if os.name == "nt" else ""))

    # one series per distribution kind, rotating so every kind (including
    # the large-offset stability family) is covered on every run
    series = [gen_series(N, i % N_KINDS) for i in range(N_SERIES)]
    # pair each series with the next one (cyclic) so corr/cov/linreg see
    # two independent draws
    pairs = [(series[i], series[(i + 1) % N_SERIES]) for i in range(N_SERIES)]

    lines = []
    expected = []      # Python reference rows, in print order
    for i, (x, y) in enumerate(pairs):
        lines.append("let d%da: list = %s;" % (i, emit(x)))
        lines.append("let d%db: list = %s;" % (i, emit(y)))
    lines.append("")
    for i, (x, y) in enumerate(pairs):
        ref = []
        def row(v):
            ref.append(v)
        da, db = "d%da" % i, "d%db" % i
        row(st.mean(x));                  lines.append("print(mean(%s));" % da)
        row(st.variance(x));              lines.append("print(variance(%s));" % da)
        row(st.stdev(x));                 lines.append("print(stddev(%s));" % da)
        row(st.median(x));                lines.append("print(median(%s));" % da)
        row(percentile(x, 10.0));         lines.append("print(percentile(%s, 10.0));" % da)
        row(percentile(x, 90.0));         lines.append("print(percentile(%s, 90.0));" % da)
        row(sample_cov(x, y));            lines.append("print(covariance(%s, %s));" % (da, db))
        row(sample_cov(y, x));            lines.append("print(covariance(%s, %s));" % (db, da))
        row(sample_corr(x, y));           lines.append("print(correlation(%s, %s));" % (da, db))
        sl, itc, r2 = linreg(x, y)
        row(sl);  lines.append("let fit%da = linreg(%s, %s); print(get(fit%da, \"slope\", 0.0));" % (i, da, db, i))
        row(itc); lines.append("print(get(fit%da, \"intercept\", 0.0));" % i)
        row(r2);  lines.append("print(get(fit%da, \"r2\", 0.0));" % i)
        row(max_drawdown(x));             lines.append("print(max_drawdown(%s));" % da)
        pc = pct_change(x)
        lines.append("let pc%da: list = pct_change(%s);" % (i, da))
        for k, v in enumerate(pc):
            row(v); lines.append("print(pc%da[%d]);" % (i, k))
        # distribution shape + rolling/ewma family (2026-10-24):
        # skew/kurtosis are scalars; rolling_mean/rolling_stddev/ewma emit
        # N rows each (w=3 window, so rows 0-1 are NAN).
        row(skew(x));                     lines.append("print(skew(%s));" % da)
        row(kurtosis(x));                 lines.append("print(kurtosis(%s));" % da)
        lines.append("let rm%da: list = rolling_mean(%s, 3);" % (i, da))
        for k, v in enumerate(rolling_mean(x, 3)):
            row(v); lines.append("print(rm%da[%d]);" % (i, k))
        lines.append("let rs%da: list = rolling_stddev(%s, 3);" % (i, da))
        for k, v in enumerate(rolling_stddev(x, 3)):
            row(v); lines.append("print(rs%da[%d]);" % (i, k))
        lines.append("let ew%da: list = ewma(%s, 0.3);" % (i, da))
        for k, v in enumerate(ewma(x, 0.3)):
            row(v); lines.append("print(ew%da[%d]);" % (i, k))
        # norminv reverse round-trip: normcdf(norminv(p)) == p for p in the
        # open interval — the forward direction (norminv(normcdf(x))) would
        # saturate on the fuzz's large-offset series, so the reverse form
        # keeps p mid-domain while still exercising both Acklam branches.
        p = 0.15 + 0.07 * i
        row(p); lines.append("print(normcdf(norminv(%s)));" % ("%.6f" % p))
        # advanced samplers (2026-10-28): zipf one plain draw against the
        # cumulative i^-s mass (TY_INT); negative-binomial sums r geometric
        # failure counts (r biased draws, TY_INT); erlang sums k
        # exponentials (k biased draws); von Mises runs the Best-Fisher
        # rejection loop (per round: one plain z draw, one biased
        # acceptance draw — round count data-dependent but stream-fixed);
        # multinomial answers an int list, asserted here through [0] (the
        # first category's count), its stream offset following whatever
        # von Mises actually consumed.
        sd = 1000 + i
        lines.append("seed(%d);" % sd)
        row(pcg_zipf(sd, 10, 1.5, 1))
        lines.append("print(rand_zipf(10, 1.5));")
        row(pcg_nb(sd, 3, 0.3, 2))
        lines.append("print(rand_negative_binomial(3, 0.3));")
        row(sixg(pcg_erlang(sd, 2, 0.5, 5)))
        lines.append("print(rand_erlang(2, 0.5));")
        vm0, dr = pcg_vm(sd, 0.0, 2.0, 7)
        row(sixg(vm0))
        lines.append("print(rand_von_mises(0.0, 2.0));")
        row(pcg_multinom(sd, 10, [0.5, 0.3, 0.2], 7 + dr)[0])
        lines.append("print(rand_multinomial(10, [0.5, 0.3, 0.2])[0]);")
        lines.append("seed(%d);" % sd)
        row(pcg_zipf(sd, 5, 2.0, 1))
        lines.append("print(rand_zipf(5, 2.0));")
        vm1, _ = pcg_vm(sd, 1.0, 0.0, 2)
        row(sixg(vm1))
        lines.append("print(rand_von_mises(1.0, 0.0));")
        # mixed samplers (2026-10-27): binomial counts n Bernoulli
        # successes (n plain draws, TY_INT; p=0.5 then p=0.2 after a
        # re-seed); lognormal is exp(Box-Muller) on two biased draws;
        # gumbel / rayleigh one biased draw; triangular the symmetric
        # inverse CDF on one plain draw.
        sd = 1000 + i
        lines.append("seed(%d);" % sd)
        row(pcg_binom(sd, 10, 0.5, 1))
        lines.append("print(rand_binomial(10, 0.5));")
        row(sixg(pcg_logn(sd, 0.0, 1.0, 11)))
        lines.append("print(rand_lognormal(0.0, 1.0));")
        row(sixg(pcg_gum(sd, 0.0, 1.0, 13)))
        lines.append("print(rand_gumbel(0.0, 1.0));")
        row(sixg(pcg_ray(sd, 1.0, 14)))
        lines.append("print(rand_rayleigh(1.0));")
        row(sixg(pcg_tri(sd, 0.0, 1.0, 15)))
        lines.append("print(rand_triangular(0.0, 1.0));")
        lines.append("seed(%d);" % sd)
        row(pcg_binom(sd, 3, 0.2, 1))
        lines.append("print(rand_binomial(3, 0.2));")
        row(sixg(pcg_logn(sd, 1.0, 0.5, 4)))
        lines.append("print(rand_lognormal(1.0, 0.5));")
        # inverse-transform samplers (2026-10-26): geometric is the failure
        # count floor(ln(ub)/ln(1-p)) on one draw (p=0.5 and p=0.1, the
        # second block after a re-seed); weibull/logistic/pareto/laplace
        # are closed-form inverse CDFs on one 2^-25-biased draw each.
        sd = 1000 + i
        lines.append("seed(%d);" % sd)
        row(pcg_geo(sd, 0.5, 1))
        lines.append("print(rand_geometric(0.5));")
        row(sixg(pcg_wei(sd, 2.0, 1.0, 2)))
        lines.append("print(rand_weibull(2.0, 1.0));")
        row(sixg(pcg_logi(sd, 0.0, 1.0, 3)))
        lines.append("print(rand_logistic(0.0, 1.0));")
        row(sixg(pcg_par(sd, 1.0, 2.0, 4)))
        lines.append("print(rand_pareto(1.0, 2.0));")
        row(sixg(pcg_lap(sd, 0.0, 1.0, 5)))
        lines.append("print(rand_laplace(0.0, 1.0));")
        lines.append("seed(%d);" % sd)
        row(pcg_geo(sd, 0.1, 1))
        lines.append("print(rand_geometric(0.1));")
        row(sixg(pcg_wei(sd, 0.5, 3.0, 2)))
        lines.append("print(rand_weibull(0.5, 3.0));")
        # continuous samplers (2026-10-25): gamma (Marsaglia-Tsang 2000)
        # drives beta (two sequential gamma draws), chisq (twice
        # gamma(k/2)), student_t (normal over chisq/df); cauchy is a
        # one-draw inverse-CDF tan. Offsets mirror the consistency
        # fixture's appended section; every value is checked against the
        # independent Python implementation, and the shape<1 boost branch
        # is exercised by gamma(0.5) and chisq(1).
        sd = 1000 + i
        lines.append("seed(%d);" % sd)
        row(sixg(pcg_cauchy(sd, 0.0, 1.0, 1)))
        lines.append("print(rand_cauchy(0.0, 1.0));")
        g21, ka = pcg_gamma(sd, 2.0, 2)
        row(sixg(g21 / 1.0))
        lines.append("print(rand_gamma(2.0, 1.0));")
        ga, kb = pcg_gamma(sd, 2.0, ka)
        gb, kc = pcg_gamma(sd, 3.0, kb)
        row(sixg(ga / (ga + gb)))
        lines.append("print(rand_beta(2.0, 3.0));")
        k5 = pcg_gamma(sd, 2.0, kc)[1]
        row(sixg(2.0 * pcg_gamma(sd, 2.0, kc)[0]))
        lines.append("print(rand_chisq(4.0));")
        tval, kt = pcg_t(sd, 5.0, k5)
        row(sixg(tval))
        lines.append("print(rand_student_t(5.0));")
        g05, kk = pcg_gamma(sd, 0.5, kt)
        row(sixg(g05 / 1.0))
        lines.append("print(rand_gamma(0.5, 1.0));")
        row(sixg(2.0 * pcg_gamma(sd, 0.5, kk)[0]))
        lines.append("print(rand_chisq(1.0));")
        # Monte Carlo RNG rows: deterministic per-dataset seed (1000+i).
        # consume order matches the smoke fixture: r1, r2, rr (draw 3),
        # rand_int x2 (draws 4-5 and 6-7), rand_normal x2 (draws 8-9 and
        # 10-11), then re-seed and draw the first rand again — the last row
        # must equal the first one (reproducibility round-trip).
        sd = 1000 + i
        lines.append("seed(%d);" % sd)
        row(sixg(pcg_rand(sd, 1)))
        lines.append("print(rand());")
        row(sixg(pcg_rand(sd, 2)))
        lines.append("print(rand());")
        row(sixg(0.0 + 10.0 * pcg_rand(sd, 3)))
        lines.append("print(rand_range(0.0, 10.0));")
        row(pcg_rand_int(sd, 0, 100, 4))
        lines.append("print(rand_int(0, 100));")
        row(pcg_rand_int(sd, -50, 50, 6))
        lines.append("print(rand_int(-50, 50));")
        row(sixg(pcg_rand_normal(sd, 0.0, 1.0, 8)))
        lines.append("print(rand_normal(0.0, 1.0));")
        row(sixg(pcg_rand_normal(sd, 5.0, 2.0, 10)))
        lines.append("print(rand_normal(5.0, 2.0));")
        lines.append("seed(%d);" % sd)
        row(sixg(pcg_rand(sd, 1)))
        lines.append("print(rand());")
        # distribution family (2026-10-25): rand_bool is Bernoulli 0/1,
        # rand_exponential inverse-CDF, rand_poisson Knuth's method,
        # shuffle a Fisher-Yates permutation of the series (we read element
        # [0] so each dataset still emits one row per builtin). Stream
        # offsets mirror the consistency fixture's appended section.
        lines.append("seed(%d);" % sd)
        row(pcg_rand_bool(sd, 0.5, 1))
        lines.append("print(rand_bool(0.5));")
        row(sixg(pcg_rand_exp(sd, 1.0, 2)))
        lines.append("print(rand_exponential(1.0));")
        pv, nk = pcg_rand_poi_consumed(sd, 5.0, 3)
        row(pv)
        lines.append("print(rand_poisson(5.0));")
        row(sixg(pcg_shuffle_first(sd, x, nk)))
        lines.append("print(shuffle(%s)[0]);" % da)
        lines.append("seed(%d);" % sd)
        row(pcg_rand_bool(sd, 0.0, 1))
        lines.append("print(rand_bool(0.0));")
        row(pcg_rand_bool(sd, 1.0, 2))
        lines.append("print(rand_bool(1.0));")
        row(sixg(pcg_rand_exp(sd, 0.5, 3)))
        lines.append("print(rand_exponential(0.5));")
        pv2, _ = pcg_rand_poi_consumed(sd, 2.0, 4)
        row(pv2)
        lines.append("print(rand_poisson(2.0));")
        lines.append("seed(%d);" % sd)
        row(sixg(pcg_rand(sd, 1)))
        lines.append("print(rand());")
        # financial paths + Black-Scholes (2026-10-28): sim_* answer
        # n+1-point lists (the fixture reads element n, so one row per
        # builtin), bs_* scalar prices and Greeks; absolute-k refs mirror
        # tests/quant-consistency.lume's appended continuous stream, and
        # the final gbm re-seeds and replays byte-for-byte.
        sd = 1000 + i
        lines.append("seed(%d);" % sd)
        row(sixg(pcg_sim_gbm(sd, 100.0, 0.05, 0.2, 1.0, 5, 1)[5]))
        lines.append("print(sim_gbm(100.0, 0.05, 0.2, 1.0, 5)[5]);")
        row(sixg(pcg_sim_bridge(sd, 0.0, 10.0, 0.5, 1.0, 4, 11)[4]))
        lines.append("print(sim_bridge(0.0, 10.0, 0.5, 1.0, 4)[4]);")
        row(sixg(pcg_sim_ou(sd, 1.0, 2.0, 1.0, 0.3, 1.0, 4, 19)[4]))
        lines.append("print(sim_ou(1.0, 2.0, 1.0, 0.3, 1.0, 4)[4]);")
        row(sixg(pcg_sim_merton(sd, 100.0, 0.05, 0.2, 0.2, 0.0, 0.1, 1.0, 4, 27)[4]))
        lines.append("print(sim_merton(100.0, 0.05, 0.2, 0.2, 0.0, 0.1, 1.0, 4)[4]);")
        row(sixg(bs_call_ref(100.0, 100.0, 0.05, 0.2, 1.0)))
        lines.append("print(bs_call(100.0, 100.0, 0.05, 0.2, 1.0));")
        row(sixg(bs_put_ref(100.0, 100.0, 0.05, 0.2, 1.0)))
        lines.append("print(bs_put(100.0, 100.0, 0.05, 0.2, 1.0));")
        row(sixg(bs_delta_call_ref(100.0, 100.0, 0.05, 0.2, 1.0)))
        lines.append("print(bs_delta_call(100.0, 100.0, 0.05, 0.2, 1.0));")
        row(sixg(bs_delta_put_ref(100.0, 100.0, 0.05, 0.2, 1.0)))
        lines.append("print(bs_delta_put(100.0, 100.0, 0.05, 0.2, 1.0));")
        row(sixg(bs_gamma_ref(100.0, 100.0, 0.05, 0.2, 1.0)))
        lines.append("print(bs_gamma(100.0, 100.0, 0.05, 0.2, 1.0));")
        row(sixg(bs_vega_ref(100.0, 100.0, 0.05, 0.2, 1.0)))
        lines.append("print(bs_vega(100.0, 100.0, 0.05, 0.2, 1.0));")
        lines.append("seed(%d);" % sd)
        row(sixg(pcg_sim_gbm(sd, 100.0, 0.05, 0.2, 1.0, 5, 1)[5]))
        lines.append("print(sim_gbm(100.0, 0.05, 0.2, 1.0, 5)[5]);")
        # hypothesis tests (2026-10-29): z_test / tstat answer statistics on
        # the series (mu=10, sigma=2 keep the value moderate), the p-value /
        # chisq rows are fixed closed-form-friendly pins; deterministic (no
        # RNG draws). The references are definition-based: the t CDF by
        # density integration, the chi-square CDF by closed forms (erf /
        # exponential / gamma by parts) or integration.
        row(sixg(z_test_ref(x, 10.0, 2.0)))
        lines.append("print(z_test(%s, 10, 2));" % da)
        row(sixg(z_pvalue_ref(1.96)))
        lines.append("print(z_pvalue(1.96));")
        row(sixg(tstat_ref(x, 10.0)))
        lines.append("print(tstat(%s, 10));" % da)
        row(sixg(ttest_p_ref(2.32379, 3)))
        lines.append("print(ttest_p(2.32379, 3));")
        row(sixg(chisq_stat_ref([5.0, 15.0], [10.0, 10.0])))
        lines.append("print(chisq_stat([5,15], [10,10]));")
        row(sixg(chisq_p_ref(5.0, 1)))
        lines.append("print(chisq_p(5, 1));")
        # multivariate / copula (2026-10-30): dirichlet answers a (0,1)
        # simplex share, binormal stays finite, the copulas answer
        # probabilities in (0,1); each row re-seeds so the absolute-k
        # reference (1-based, see pcg32_draw) replays from the start of
        # the stream, mirroring the appended consistency section.
        lines.append("seed(%d);" % sd)
        row(sixg(pcg_dirichlet(sd, [1.0, 1.0, 1.0], 1)[0][0]))
        lines.append("print(rand_dirichlet([1.0, 1.0, 1.0])[0]);")
        lines.append("seed(%d);" % sd)
        row(sixg(pcg_binormal(sd, 0.0, 0.0, 1.0, 1.0, 0.5, 1, 1)[0][0]))
        lines.append("print(rand_binormal(0.0, 0.0, 1.0, 1.0, 0.5, 1)[0]);")
        lines.append("seed(%d);" % sd)
        row(sixg(pcg_gauss_copula(sd, 0.5, 1, 1)[0][0]))
        lines.append("print(rand_gauss_copula(0.5, 1)[0]);")
        lines.append("seed(%d);" % sd)
        row(sixg(pcg_t_copula(sd, 0.5, 4.0, 1, 1)[0][0]))
        lines.append("print(rand_t_copula(0.5, 4.0, 1)[0]);")
        # bootstrap / resampling (2026-10-31): percentile-method CIs on the
        # series (n_iter=20, alpha=0.05) and the standard error of the mean;
        # each row re-seeds so the absolute-k reference (1-based) replays
        # from the start of the stream — every replicate consumes exactly
        # n plain uniform draws, mirroring the appended consistency section.
        lines.append("seed(%d);" % sd)
        cim = pcg_bootstrap_ci(sd, x, 20, 0.05, 0, 1)[0]
        lines.append("let bc%da: list = bootstrap_mean_ci(%s, 20, 0.05);" % (i, da))
        row(sixg(cim[0])); lines.append("print(bc%da[0]);" % i)
        row(sixg(cim[1])); lines.append("print(bc%da[1]);" % i)
        lines.append("seed(%d);" % sd)
        row(sixg(pcg_bootstrap_ci(sd, x, 20, 0.05, 1, 1)[0][0]))
        lines.append("print(bootstrap_median_ci(%s, 20, 0.05)[0]);" % da)
        lines.append("seed(%d);" % sd)
        row(sixg(pcg_bootstrap_ci(sd, x, 20, 0.05, 2, 1)[0][1]))
        lines.append("print(bootstrap_stddev_ci(%s, 20, 0.05)[1]);" % da)
        lines.append("seed(%d);" % sd)
        row(sixg(pcg_bootstrap_se(sd, x, 20, 1)[0]))
        lines.append("print(bootstrap_se_mean(%s, 20));" % da)
        # portfolio / risk metrics (2026-10-31, #48): deterministic, no RNG.
        # value_at_risk / expected_shortfall at level 0.05 (negative
        # quantile / negative tail mean), sharpe / sortino at rf 0.0.
        row(sixg(-percentile(x, 5.0)))
        lines.append("print(value_at_risk(%s, 0.05));" % da)
        row(sixg(-es_tail(x, 5.0)))
        lines.append("print(expected_shortfall(%s, 0.05));" % da)
        row(sixg(sharpe_ratio_ref(x, 0.0)))
        lines.append("print(sharpe_ratio(%s, 0.0));" % da)
        row(sixg(sortino_ratio_ref(x, 0.0)))
        lines.append("print(sortino_ratio(%s, 0.0));" % da)
        expected.append(ref)

    with open(fixture, "w", encoding="utf-8", newline="\n") as f:
        f.write("// auto-generated by tests/quant-fuzz.py (seeded, deterministic)\n")
        f.write("\n".join(lines) + "\n")

    # ---- run all three legs
    r_int = run_lume(target, repo, fixture)
    r_txt = run_lume(target, repo, "--compile-text", fixture, "-o", textexe)
    r_llv = run_lume(target, repo, "--compile-llvm", fixture, "-o", llvmexe)
    if r_txt.returncode != 0:
        sys.stderr.write(r_txt.stdout + r_txt.stderr)
        sys.stderr.write("fuzz: text backend failed to compile\n")
        return 1
    if r_llv.returncode != 0:
        sys.stderr.write(r_llv.stdout + r_llv.stderr)
        sys.stderr.write("fuzz: llvm backend failed to compile\n")
        return 1
    r_txt_run = run_lume(os.path.abspath(textexe), repo)
    r_llv_run = run_lume(os.path.abspath(llvmexe), repo)

    legs = {
        "interp": r_int.stdout,
        "text": r_txt_run.stdout,
        "llvm": r_llv_run.stdout,
    }
    rows = {k: parse_rows(v) for k, v in legs.items()}
    n_rows = len(rows["interp"])
    total = 0
    fails = 0

    # ---- 1. leg consistency (byte-level, ignoring the trailing note line)
    base = [l for l in legs["interp"].splitlines() if l.strip() and not l.startswith("lume:")]
    for k in ("text", "llvm"):
        if [l for l in legs[k].splitlines() if l.strip()] != base:
            sys.stderr.write("fuzz: %s output differs from interpreter\n" % k)
            for a, b in zip(base, [l for l in legs[k].splitlines() if l.strip()]):
                if a != b:
                    sys.stderr.write("  interp: %r\n  %-5s: %r\n" % (a, k, b))
                    break
            fails += 1

    # ---- 2 + 3. golden values + properties, row by row
    flat_exp = [v for ref in expected for v in ref]
    if n_rows != len(flat_exp):
        sys.stderr.write("fuzz: row count mismatch (%d lume vs %d python)\n"
                         % (n_rows, len(flat_exp)))
        return 1
    for i in range(n_rows):
        total += 1
        got = rows["interp"][i]
        want = flat_exp[i]
        r = i % RPP
        # zipf is a discrete inverse-CDF: mingw's pow and Python's UCRT
        # pow can differ by one ULP, which flips the returned rank when the
        # draw lands within one ULP of a bucket boundary — exact-value
        # equality is not a stable cross-libm contract here, so those two
        # rows are range-checked by the property assertions below instead.
        if r not in (39, 44) and not close(got, want):
            sys.stderr.write("FAIL row %d: lume=%r python=%r\n" % (i, got, want))
            fails += 1
        # property assertions (independent of the formula used)
        if r in (1, 2) and got < -1e-9:      # variance / stddev >= 0
            sys.stderr.write("FAIL row %d: negative variance/stddev %r\n" % (i, got)); fails += 1
        if r == 8 and abs(got) > 1 + 1e-9:    # correlation in [-1, 1]
            sys.stderr.write("FAIL row %d: correlation out of range %r\n" % (i, got)); fails += 1
        if r == 11 and (got < -1e-9 or got > 1 + 1e-9):  # r2 in [0, 1]
            sys.stderr.write("FAIL row %d: r2 out of range %r\n" % (i, got)); fails += 1
        if r == 12 and got < -1e-9:  # drawdown >= 0 (a price falling below 0
            sys.stderr.write("FAIL row %d: negative drawdown %r\n" % (i, got)); fails += 1
            # vs a positive peak can exceed 100% — the bound is only the sign)
        # Monte Carlo property rows (span RPP-45 .. RPP-1): the advanced
        # samplers at RPP-45..RPP-39 (zipf in [1,n], negative-binomial
        # >= 0, erlang > 0, von Mises finite, multinomial[0] in [0,n],
        # reseeded zipf(5) in [1,5], kappa=0 von Mises finite), the mixed
        # samplers at RPP-38..RPP-32 (binomial in [0,n], lognormal /
        # rayleigh > 0, gumbel finite, triangular in [lo,hi]), the
        # inverse-transform samplers at RPP-31..RPP-25 (geometric >= 0,
        # weibull / pareto > 0, logistic/laplace finite), the continuous
        # samplers at RPP-24..RPP-18 (cauchy/t finite, gamma/chisq > 0,
        # beta in (0,1), gamma<1/chisq<1 exercise the boost branch), then
        # rand() in [0,1), rand_range inside its interval, rand_int inside
        # its inclusive bounds, rand_bool in {0,1}, rand_exponential > 0,
        # rand_poisson >= 0, shuffle[0] a member of the series, and the
        # closing re-seed row (first rand again) equals the opening rand row
        # — a reproducibility contract no identical-implementation bug could
        # fake, since the round-trip forces a full stream restart.
        if r >= 39:
            # row numbers below are absolute positions inside one 95-row
            # dataset (see RPP above); they are kept explicit instead of
            # RPP-relative so inserting a block can never silently re-target
            # an unrelated row.
            if r == 39 or r == 44:                # zipf in [1, n]
                hi_n = 10 if r == 39 else 5
                if got < 1.0 or got > hi_n:
                    sys.stderr.write("FAIL row %d: zipf out of [1,%d] %r\n" % (i, hi_n, got)); fails += 1
            elif r == 40:                         # negative-binomial >= 0
                if got < 0.0:
                    sys.stderr.write("FAIL row %d: negbinom < 0 %r\n" % (i, got)); fails += 1
            elif r == 41:                         # erlang > 0
                if got <= 0.0:
                    sys.stderr.write("FAIL row %d: erlang <= 0 %r\n" % (i, got)); fails += 1
            elif r == 42 or r == 45:              # von Mises finite
                if not math.isfinite(got):
                    sys.stderr.write("FAIL row %d: non-finite vonmises %r\n" % (i, got)); fails += 1
            elif r == 43:                         # multinomial[0] in [0,10]
                if got < 0.0 or got > 10.0:
                    sys.stderr.write("FAIL row %d: multinomial[0] out of [0,10] %r\n" % (i, got)); fails += 1
            elif r == 46 or r == 51:              # binomial in [0, n]
                hi_n = 10 if r == 46 else 3
                if got < 0.0 or got > hi_n:
                    sys.stderr.write("FAIL row %d: binomial out of [0,%d] %r\n" % (i, hi_n, got)); fails += 1
            elif r == 47 or r == 52:              # lognormal > 0
                if got <= 0.0:
                    sys.stderr.write("FAIL row %d: lognormal <= 0 %r\n" % (i, got)); fails += 1
            elif r == 48:                         # gumbel finite
                if not math.isfinite(got):
                    sys.stderr.write("FAIL row %d: non-finite gumbel %r\n" % (i, got)); fails += 1
            elif r == 49:                         # rayleigh > 0
                if got <= 0.0:
                    sys.stderr.write("FAIL row %d: rayleigh <= 0 %r\n" % (i, got)); fails += 1
            elif r == 50:                         # triangular in [0,1]
                if got < -1e-9 or got > 1.0 + 1e-9:
                    sys.stderr.write("FAIL row %d: triangular out of [0,1] %r\n" % (i, got)); fails += 1
            elif r == 53 or r == 58:              # geometric >= 0
                if got < 0.0:
                    sys.stderr.write("FAIL row %d: geometric < 0 %r\n" % (i, got)); fails += 1
            elif r == 54 or r == 59:              # weibull > 0
                if got <= 0.0:
                    sys.stderr.write("FAIL row %d: weibull <= 0 %r\n" % (i, got)); fails += 1
            elif r == 55 or r == 57:              # logistic / laplace
                if not math.isfinite(got):
                    sys.stderr.write("FAIL row %d: non-finite sampler %r\n" % (i, got)); fails += 1
            elif r == 56:                         # pareto >= xm > 0
                if got <= 0.0:
                    sys.stderr.write("FAIL row %d: pareto <= 0 %r\n" % (i, got)); fails += 1
            elif r == 60 or r == 64:              # cauchy / student_t
                if not math.isfinite(got):
                    sys.stderr.write("FAIL row %d: non-finite sampler %r\n" % (i, got)); fails += 1
            elif r == 61 or r == 65:              # gamma / gamma<1 > 0
                if got <= 0.0:
                    sys.stderr.write("FAIL row %d: gamma <= 0 %r\n" % (i, got)); fails += 1
            elif r == 62:                         # beta in (0, 1)
                if got <= 0.0 or got >= 1.0:
                    sys.stderr.write("FAIL row %d: beta out of (0,1) %r\n" % (i, got)); fails += 1
            elif r == 63 or r == 66:              # chisq / chisq<1 > 0
                if got <= 0.0:
                    sys.stderr.write("FAIL row %d: chisq <= 0 %r\n" % (i, got)); fails += 1
            elif r == 67 or r == 68 or r == 74 or r == 83:      # rand()
                if got < -1e-9 or got > 1.0 + 1e-9:
                    sys.stderr.write("FAIL row %d: rand() out of [0,1) %r\n" % (i, got)); fails += 1
            elif r == 69:                    # rand_range(0, 10)
                if got < -1e-9 or got > 10.0 + 1e-9:
                    sys.stderr.write("FAIL row %d: rand_range out of [0,10] %r\n" % (i, got)); fails += 1
            elif r == 70:                    # rand_int(0, 100)
                if got < 0 or got > 100:
                    sys.stderr.write("FAIL row %d: rand_int out of [0,100] %r\n" % (i, got)); fails += 1
            elif r == 71:                    # rand_int(-50, 50)
                if got < -50 or got > 50:
                    sys.stderr.write("FAIL row %d: rand_int out of [-50,50] %r\n" % (i, got)); fails += 1
            elif r == 75 or r == 79 or r == 80:      # rand_bool
                if got not in (0.0, 1.0):
                    sys.stderr.write("FAIL row %d: rand_bool out of {0,1} %r\n" % (i, got)); fails += 1
            elif r == 76 or r == 81:      # rand_exponential > 0
                if got <= 0.0:
                    sys.stderr.write("FAIL row %d: rand_exponential <= 0 %r\n" % (i, got)); fails += 1
            elif r == 77 or r == 82:      # rand_poisson >= 0
                if got < 0.0:
                    sys.stderr.write("FAIL row %d: rand_poisson < 0 %r\n" % (i, got)); fails += 1
            elif r == 78:                      # shuffle[0] in [min, max]
                # compare at the printout's six significant figures: on the
                # large-offset series the raw min/max and the printed value
                # can differ by the %g rounding (e.g. 99999896.7 prints as
                # 9.99999e7), so the bounds are rounded the same way.
                si = i // RPP
                lo, hi = sixg(min(series[si])), sixg(max(series[si]))
                if got < lo - 1e-9 or got > hi + 1e-9:
                    sys.stderr.write("FAIL row %d: shuffle[0] %r outside series [%r, %r]\n"
                                     % (i, got, lo, hi)); fails += 1
            elif r == 83:                      # re-seed round-trip
                if not close(got, rows["interp"][i - 16]):
                    sys.stderr.write("FAIL row %d: re-seed rand %r != first rand %r\n"
                                     % (i, got, rows["interp"][i - 16]))
                    fails += 1
            elif r == 84 or r == 87 or r == 94:  # gbm[5] / merton[4] / replay > 0
                if got <= 0.0:
                    sys.stderr.write("FAIL row %d: path endpoint <= 0 %r\n" % (i, got)); fails += 1
            elif r == 85:                      # bridge endpoint exactly xt
                if not close(got, 10.0):
                    sys.stderr.write("FAIL row %d: bridge endpoint %r != 10\n" % (i, got)); fails += 1
            elif r == 86:                       # ou[4] finite
                if not math.isfinite(got):
                    sys.stderr.write("FAIL row %d: non-finite ou path %r\n" % (i, got)); fails += 1
            elif r == 88 or r == 89 or r == 92 or r == 93:  # bs prices/greeks > 0
                if got <= 0.0:
                    sys.stderr.write("FAIL row %d: bs value <= 0 %r\n" % (i, got)); fails += 1
            elif r == 90 or r == 91:       # bs deltas in [-1, 1]
                if got < -1.0 - 1e-9 or got > 1.0 + 1e-9:
                    sys.stderr.write("FAIL row %d: bs delta out of [-1,1] %r\n" % (i, got)); fails += 1
            elif r == 95 or r == 97:       # z_test / tstat finite
                if not math.isfinite(got):
                    sys.stderr.write("FAIL row %d: non-finite test statistic %r\n" % (i, got)); fails += 1
            elif r == 96 or r == 98 or r == 100:  # p-values in (0, 1]
                if got <= 0.0 or got > 1.0 + 1e-9:
                    sys.stderr.write("FAIL row %d: p-value out of (0,1] %r\n" % (i, got)); fails += 1
            elif r == 99:                  # chisq_stat >= 0
                if got < -1e-9:
                    sys.stderr.write("FAIL row %d: chisq_stat < 0 %r\n" % (i, got)); fails += 1
            elif r == 101 or r == 103 or r == 104:  # dirichlet share / copula
                # probabilities in (0, 1)
                if got <= 0.0 or got >= 1.0:
                    sys.stderr.write("FAIL row %d: probability out of (0,1) %r\n"
                                     % (i, got)); fails += 1
            elif r == 102:                  # binormal[0] finite
                if not math.isfinite(got):
                    sys.stderr.write("FAIL row %d: non-finite binormal %r\n" % (i, got)); fails += 1

    # covariance symmetry is asserted on the pair rows (r==6 vs r==7)
    for i in range(N_SERIES):
        a = RPP * i + 6
        b = RPP * i + 7
        if not close(rows["interp"][a], rows["interp"][b]):
            sys.stderr.write("FAIL series %d: covariance(x,y) %r != covariance(y,x) %r\n"
                             % (i, rows["interp"][a], rows["interp"][b]))
            fails += 1

    print("quant fuzz: %d series x %d rows = %d checks, %d failures"
         % (N_SERIES, n_rows // N_SERIES, total, fails))
    return 1 if fails else 0


if __name__ == "__main__":
    sys.exit(main())

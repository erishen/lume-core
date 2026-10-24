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

fails = 0
for name, got, want in cases:
    ok = close(got, want)
    if not ok:
        fails += 1
    print(("%s %-28s ref=%-24s lume=%s" % ("PASS" if ok else "FAIL", name, str(got), str(want))))
print("-" * 72)
print("fails: %d / %d" % (fails, len(cases)))
raise SystemExit(1 if fails else 0)

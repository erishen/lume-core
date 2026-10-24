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
RPP = 63                              # rows per dataset (39 quant rows + 24
                                      # Monte Carlo rows: continuous samplers
                                      # x7 (cauchy / gamma / beta / chisq /
                                      # student_t / gamma<1 / chisq<1), rand
                                      # x2, rand_range, rand_int x2,
                                      # rand_normal x2, re-seed + first rand
                                      # again, then the distribution family:
                                      # rand_bool x3 (0.5 / 0.0 / 1.0),
                                      # rand_exponential x2, rand_poisson x2,
                                      # shuffle[0], and a closing re-seed
                                      # round-trip)

# ---------------------------------------------------------------- reference
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
        if not close(got, want):
            sys.stderr.write("FAIL row %d: lume=%r python=%r\n" % (i, got, want))
            fails += 1
        # property assertions (independent of the formula used)
        r = i % RPP
        if r in (1, 2) and got < -1e-9:      # variance / stddev >= 0
            sys.stderr.write("FAIL row %d: negative variance/stddev %r\n" % (i, got)); fails += 1
        if r == 8 and abs(got) > 1 + 1e-9:    # correlation in [-1, 1]
            sys.stderr.write("FAIL row %d: correlation out of range %r\n" % (i, got)); fails += 1
        if r == 11 and (got < -1e-9 or got > 1 + 1e-9):  # r2 in [0, 1]
            sys.stderr.write("FAIL row %d: r2 out of range %r\n" % (i, got)); fails += 1
        if r == 12 and got < -1e-9:  # drawdown >= 0 (a price falling below 0
            sys.stderr.write("FAIL row %d: negative drawdown %r\n" % (i, got)); fails += 1
            # vs a positive peak can exceed 100% — the bound is only the sign)
        # Monte Carlo property rows (span RPP-24 .. RPP-1): the continuous
        # samplers at RPP-24..RPP-18 (cauchy/t finite, gamma/chisq > 0,
        # beta in (0,1), gamma<1/chisq<1 exercise the boost branch), then
        # rand() in [0,1), rand_range inside its interval, rand_int inside
        # its inclusive bounds, rand_bool in {0,1}, rand_exponential > 0,
        # rand_poisson >= 0, shuffle[0] a member of the series, and the
        # closing re-seed row (first rand again) equals the opening rand row
        # — a reproducibility contract no identical-implementation bug could
        # fake, since the round-trip forces a full stream restart.
        if r >= RPP - 24:
            if r == RPP - 24 or r == RPP - 20:            # cauchy / student_t
                if not math.isfinite(got):
                    sys.stderr.write("FAIL row %d: non-finite sampler %r\n" % (i, got)); fails += 1
            elif r == RPP - 23 or r == RPP - 19:          # gamma / gamma<1 > 0
                if got <= 0.0:
                    sys.stderr.write("FAIL row %d: gamma <= 0 %r\n" % (i, got)); fails += 1
            elif r == RPP - 22:                           # beta in (0, 1)
                if got <= 0.0 or got >= 1.0:
                    sys.stderr.write("FAIL row %d: beta out of (0,1) %r\n" % (i, got)); fails += 1
            elif r == RPP - 21 or r == RPP - 18:          # chisq / chisq<1 > 0
                if got <= 0.0:
                    sys.stderr.write("FAIL row %d: chisq <= 0 %r\n" % (i, got)); fails += 1
            elif r == RPP - 17 or r == RPP - 16 or r == RPP - 1:      # rand()
                if got < -1e-9 or got > 1.0 + 1e-9:
                    sys.stderr.write("FAIL row %d: rand() out of [0,1) %r\n" % (i, got)); fails += 1
            elif r == RPP - 15:                    # rand_range(0, 10)
                if got < -1e-9 or got > 10.0 + 1e-9:
                    sys.stderr.write("FAIL row %d: rand_range out of [0,10] %r\n" % (i, got)); fails += 1
            elif r == RPP - 14:                    # rand_int(0, 100)
                if got < 0 or got > 100:
                    sys.stderr.write("FAIL row %d: rand_int out of [0,100] %r\n" % (i, got)); fails += 1
            elif r == RPP - 13:                    # rand_int(-50, 50)
                if got < -50 or got > 50:
                    sys.stderr.write("FAIL row %d: rand_int out of [-50,50] %r\n" % (i, got)); fails += 1
            elif r == RPP - 9 or r == RPP - 5 or r == RPP - 4:      # rand_bool
                if got not in (0.0, 1.0):
                    sys.stderr.write("FAIL row %d: rand_bool out of {0,1} %r\n" % (i, got)); fails += 1
            elif r == RPP - 8 or r == RPP - 3:      # rand_exponential > 0
                if got <= 0.0:
                    sys.stderr.write("FAIL row %d: rand_exponential <= 0 %r\n" % (i, got)); fails += 1
            elif r == RPP - 7 or r == RPP - 2:      # rand_poisson >= 0
                if got < 0.0:
                    sys.stderr.write("FAIL row %d: rand_poisson < 0 %r\n" % (i, got)); fails += 1
            elif r == RPP - 6:                      # shuffle[0] in [min, max]
                # compare at the printout's six significant figures: on the
                # large-offset series the raw min/max and the printed value
                # can differ by the %g rounding (e.g. 99999896.7 prints as
                # 9.99999e7), so the bounds are rounded the same way.
                si = i // RPP
                lo, hi = sixg(min(series[si])), sixg(max(series[si]))
                if got < lo - 1e-9 or got > hi + 1e-9:
                    sys.stderr.write("FAIL row %d: shuffle[0] %r outside series [%r, %r]\n"
                                     % (i, got, lo, hi)); fails += 1
            elif r == RPP - 1:                      # re-seed round-trip
                if not close(got, rows["interp"][i - (RPP - 1) + (RPP - 17)]):
                    sys.stderr.write("FAIL row %d: re-seed rand %r != first rand %r\n"
                                     % (i, got, rows["interp"][i - (RPP - 1) + (RPP - 17)]))
                    fails += 1

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

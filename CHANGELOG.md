# Changelog

All notable changes to Lume are documented here. The format follows
[Keep a Changelog](https://keepachangelog.com/), and versions aim for
[SemVer](https://semver.org/).

## [Unreleased]

### Added

- **Statistical builtins infer `float` and emit on the native backends.**
  `normcdf` / `erf` / `gamma` / `lgamma` / `igamma` / `ibeta` /
  `tdist_cdf` / `chisq_cdf` had interpreter support and a text-backend Bi
  table but no inference entry and no libLLVM emit leg, so an unannotated
  `let p = tdist_cdf(...)` refused to compile natively (the P4
  counter-example). All eight now infer `TY_FLOAT` (`codegen_scan.c` +
  `llvm_cg_scan.inc`) and emit through `rt.c`'s `lume_bi_*` helpers
  (`llvm_cg_call.inc`, arity-checked). `tests/native_backends.sh` gains a
  stats cross-backend block; the AI-gate counter-example flipped to the
  type mismatch `let p: string = tdist_cdf(...)`.
- **P6 sql surface joins the AI self-verification loop.** `gen-sql-tx`
  seed (tx open → write → query → commit) joins the produce manifest, and
  `verify-ai-lume.sh` gains an availability probe: a sql-bearing program
  is judged only where the build has libsqlite3 (scratch `SQLITE_DB`,
  same protocol as `native_backends.sh`'s sql block) and skipped with a
  note elsewhere. The ai-verify CI job installs `libsqlite3-dev` so the
  seed is genuinely judged there.
- **Quant-analysis builtins: mean / variance / stddev / median /
  percentile / covariance / correlation / linreg / pct_change /
  max_drawdown.** Ten list-argument statistics and time-series helpers
  with the same three-leg contract as the CDFs: interpreter wrappers
  (`builtins_math.c` native_*, validated with vm_set_error), native
  runtime (`rt.c` lume_bi_*), and type inference + emission on both
  native backends (arity table in `typecheck.c`, inference in
  `codegen_scan.c` / `llvm_cg_scan.inc`, list-arg emit branches in
  `codegen_call.c` / `llvm_cg_call.inc` — sort()'s pattern).
  `variance` / `stddev` are sample (n-1); `percentile(p)` linearly
  interpolates for p∈[0,100]; `linreg` returns a fresh `{slope,
  intercept, r2}` map; `pct_change` returns a fresh percentage list
  (n-1, NAN over a zero previous value); `max_drawdown` answers a
  positive percentage. `tests/quant-consistency.lume` pins byte-identical
  output across the interpreter / text / LLVM legs, and
  `examples/quant.lume` walks a price/benchmark study.
  `tests/quant-refcheck.py` cross-checks the pinned values against Python's
  `statistics` module, and `tests/quant-fuzz.py` runs seeded random series
  through all three legs, verifying every printed number against Python
  reference implementations plus math-property assertions; both are wired
  into `tests/native_backends.sh` and skipped when no python is found.
  `variance`/`stddev`/`covariance`/`correlation`/`linreg` use Welford's
  single-pass recurrences (identical on both the interpreter and native
  sides) so sums of squares never run through a large precomputed mean —
  the naive two-pass form degrades on data riding a large offset, which
  the fuzz's large-offset distribution family now pins down.
- **Quant builtins grow a distribution-shape and rolling/EWMA family.**
  `skew` (adjusted Fisher-Pearson sample skewness, n≥3), `kurtosis`
  (sample excess Fisher kurtosis, n≥4), `rolling_mean` / `rolling_stddev`
  (list + window; window not yet full answers NAN; `rolling_stddev` uses
  ddof=1 like pandas), and `ewma` (list + alpha∈(0,1], recursive
  alpha·x + (1−alpha)·prev, out[0]=x[0]) join the ten existing quant
  helpers with the same three-leg contract: interpreter `native_*` in
  `builtins_math.c`, `lume_bi_*` in `rt.c`, inference + list-arg
  emission in `codegen_scan.c` / `codegen_call.c` and `llvm_cg_scan.inc`
  / `llvm_cg_call.inc`. A float window truncates through the new
  `lume_bi_ftoi` helper on both native backends (matching the
  interpreter's `(int64_t)` cast); the scalar form of `ewma` alpha
  widens through the existing `lume_bi_float`. `tests/quant-refcheck.py`
  now carries 39 fixed cases (Python-verified skew/kurtosis/rolling/ewma
  references) and `tests/quant-fuzz.py` 380 checks (10 series × 38 rows,
  one dataset row per printed value) — all green, byte-identical across
  the interpreter / text / LLVM legs.
- **`norminv(p)` — inverse standard-normal CDF joins the distribution
  family.** Acklam's rational approximation (2011, max |error| ~1.15e-9)
  with dedicated low/high-tail branches, implemented identically in
  `builtins_math.c` (`bi_norminv`) and `rt.c` (`lume_bi_norminv`) and
  registered like the other float-only builtins (BUILTINS table +
  inference in both scan files). `p` outside (0,1) reports an error on
  the interpreter and answers NAN on the native backends. The reverse
  round-trip `normcdf(norminv(p)) == p` is exercised at every fuzz
  dataset (390 checks now) plus fixed values in quant-refcheck (45 cases)
  and quant-consistency.
- **Monte Carlo RNG builtins — `seed`/`rand`/`rand_range`/`rand_int`/
  `rand_normal` (2026-10-24).** Deterministic PCG32 (O'Neill PCG-XSH-RR)
  with a fixed default seed and increment, so the same program produces the
  same stream on every run and byte-identically across the interpreter /
  text / LLVM legs. `seed(n)` re-bases the stream (state=n, default inc) for
  reproducible experiments; `rand()` maps the top 24 bits to [0,1);
  `rand_range(lo,hi)` = lo + (hi−lo)·rand; `rand_int(lo,hi)` draws 64 bits
  (two outputs, low word first) and reduces mod span; `rand_normal(µ,σ)` is
  Box-Muller with a 2⁻²⁵ bias keeping u1/u2 out of log(0). All five carry
  explicit arities (rand 0 args; the rest 1–2, int/float mixed), so they
  live as hand-emitted declares in both native emitters (ints widen via
  `lume_bi_float`, floats truncate via `lume_bi_ftoi`) rather than the
  BUILTINS table; the interpreter mirrors the same constants and consume
  order. `tests/quant-consistency.lume` pins the seed-42 stream (8 rows,
  three legs byte-identical); `tests/quant-refcheck.py` adds 8 fixed PCG32
  cases cross-checked against an independent Python implementation (53
  total); `tests/quant-fuzz.py` grows to 470 checks (10 series × 47 rows)
  with rand/rand_range/rand_int bound assertions and a re-seed round-trip
  (last row must equal the first) under every distribution kind.
- **Monte Carlo distribution family 鈥?`rand_bool`/`rand_exponential`/
  `rand_poisson`/`shuffle` (2026-10-25).** The RNG surface grows a
  Bernoulli draw, an exponential sampler, a Poisson sampler and a
  Fisher-Yates shuffler, all on the same deterministic PCG32 stream:
  `rand_bool(p)` answers 1 with probability p (p outside [0,1] reports
  an error on the interpreter and answers 0 natively);
  `rand_exponential(rate)` is the inverse-CDF `-ln(u)/rate` (rate 鈮?0
  likewise errors / NAN); `rand_poisson(lambda)` runs Knuth's
  multiplicative method with an `exp(-lambda)` underflow guard (errors /
  -1); `shuffle(list)` returns a fresh permutation, leaving its input
  untouched and consuming `rand_int(0,i)`'s two PCG32 draws per step so
  every backend stays byte-identical. `rand_bool`/`rand_poisson` infer
  `TY_INT`, `rand_exponential` `TY_FLOAT` and `shuffle` a float list,
  all with explicit arities like the base five.
  `tests/quant-consistency.lume` pins 9 more seed-42 rows (52 total,
  three legs byte-identical); `tests/quant-refcheck.py` adds 9 fixed
  cases against the independent Python PCG32 (62 total);
  `tests/quant-fuzz.py` grows to 560 checks (10 series 脳 56 rows) with
  bool/exponential/poisson bound assertions and a second re-seed
  round-trip after the distribution rows.
- **Continuous-distribution samplers on the deterministic stream:
  `rand_cauchy` / `rand_gamma` / `rand_beta` / `rand_chisq` /
  `rand_student_t`.** Five more Monte Carlo builtins with the same
  three-leg contract. `rand_gamma` is Marsaglia-Tsang 2000 (ACM TOMS
  algorithm 679): shape<1 boosts via a shape+1 draw raised to 1/shape,
  shape>=1 runs the rejection loop (Box-Muller z, then one u per round)
  — the loop count is data-dependent but the fixed PCG32 stream keeps
  the accepted value and the next consumer's stream position identical
  across backends; `rand_beta(a,b)` = Ga/(Ga+Gb) from two sequential
  gamma draws; `rand_chisq(k)` = twice gamma(k/2); `rand_student_t(df)`
  = a standard normal over sqrt(chisq/df); `rand_cauchy(loc,scale)` is
  the one-draw inverse-CDF tan. All five infer `TY_FLOAT` (both scans)
  and emit through rt.c's `lume_bi_*` (both call emitters, extending the
  2-param and 1-param RNG branches); invalid parameters error in the
  interpreter and answer NAN natively. `tests/quant-consistency.lume`
  pins 7 more rows (59 total, three legs byte-identical);
  `tests/quant-refcheck.py` adds 7 cases against the independent
  absolute-k Python Marsaglia-Tsang (69 total); `tests/quant-fuzz.py`
  grows to 630 checks (10 series × 63 rows) with positivity, range and
  finiteness assertions on the new samplers.
- **Inverse-transform samplers: `rand_geometric` / `rand_weibull` /
  `rand_logistic` / `rand_pareto` / `rand_laplace`.** Five more Monte
  Carlo builtins, each consuming exactly one 2^-25-biased draw (every
  formula has a log pole at the endpoints, so the anti-zero bias that
  rand_normal / rand_exponential already carry is reused). `rand_geometric`
  answers the failure count `floor(ln(ub)/ln(1-p))` for p∈(0,1] (TY_INT);
  the four continuous ones are closed-form inverse CDFs (TY_FLOAT).
  Invalid parameters error on the interpreter / answer -1 or NAN
  natively. Both scans infer, both call emitters extend the 2-param and
  1-param RNG branches. `tests/quant-consistency.lume` pins 7 more rows
  (66 total); `tests/quant-refcheck.py` adds 7 absolute-k cases (76
  total); `tests/quant-fuzz.py` grows to 700 checks (10 series × 70 rows)
  with non-negativity, positivity and finiteness assertions on the new
  samplers.
- **Mixed-arity samplers: `rand_binomial` / `rand_lognormal` /
  `rand_gumbel` / `rand_rayleigh` / `rand_triangular`.** Five more Monte
  Carlo builtins on the deterministic PCG32 stream. `rand_binomial(n, p)`
  is the first builtin with a mixed signature `(int64_t, double)` — n
  truncates through `lume_bi_ftoi`, p widens through `lume_bi_float`, and
  both call emitters gained a dedicated mixed-arity branch — answering the
  success count of n independent Bernoulli draws (n plain draws, TY_INT;
  n<0 or p∉[0,1] errors / answers -1). `rand_lognormal(mu, sigma)` is
  exp(Box-Muller) on two biased draws; `rand_gumbel(mu, beta)` and
  `rand_rayleigh(scale)` are closed-form draws on one biased draw (the
  inner double log has no pole inside the biased domain); `rand_triangular
  (lo, hi)` is the symmetric inverse CDF on one plain draw (no poles, so
  the unbiased draw is reused). `tests/quant-consistency.lume` pins 7 more
  rows (74 total); `tests/quant-refcheck.py` adds 7 absolute-k cases (83
  total); `tests/quant-fuzz.py` grows to 770 checks (10 series × 77 rows)
  with binomial range, positivity and finiteness assertions on the new
  samplers.
- **Advanced samplers: `rand_zipf` / `rand_negative_binomial` /
  `rand_erlang` / `rand_von_mises` / `rand_multinomial`.** Five more
  Monte Carlo builtins on the deterministic PCG32 stream.
  `rand_zipf(n, s)` precomputes the i^-s weights once per call and answers
  one plain draw against the cumulative mass (O(n) setup, TY_INT);
  `rand_negative_binomial(r, p)` sums r geometric failure counts (r biased
  draws, TY_INT); `rand_erlang(k, rate)` sums k exponentials (k biased
  draws); `rand_von_mises(mu, kappa)` runs the Best-Fisher rejection loop
  (each round exactly two draws — one plain for z, one biased for the
  acceptance test — so the data-dependent round count stays byte-identical
  across backends; kappa = 0 and the vanishing-rho case fall back to one
  plain draw on the uniform circle); `rand_multinomial(n, probs)` runs n
  weighted draws against the probs list and answers a fresh int list —
  the first list-returning sampler, emitting through the shuffle-shaped
  i8* branch with n truncated via `lume_bi_ftoi`. All five infer (zipf /
  negative-binomial TY_INT, erlang / von Mises TY_FLOAT, multinomial a
  TY_INT list); the (i64, double) mixed-arity emit block now serves four
  samplers. Invalid parameters error on the interpreter / answer -1, NAN
  or an empty list natively. `rand_zipf`'s exact value is not pinned in
  the Python reference (a 1-ULP libm difference between mingw pow and
  Python's UCRT pow can flip the discrete rank at a bucket boundary), so
  quant-fuzz keeps a [1,n] range property on every dataset instead.
  `tests/quant-consistency.lume` pins 7 more rows (81 total);
  `tests/quant-refcheck.py` adds 5 absolute-k cases (88 total);
  `tests/quant-fuzz.py` grows to 840 checks (10 series × 84 rows).
- **Financial paths + Black-Scholes: `sim_gbm` / `sim_bridge` /
  `sim_ou` / `sim_merton` + `bs_call` / `bs_put` / `bs_delta_call` /
  `bs_delta_put` / `bs_gamma` / `bs_vega`.** Ten builtins that turn the
  Monte Carlo stream into a pricing toolkit. The four path simulators
  answer n+1-point float lists (index 0 = the start value) on the same
  deterministic PCG32 stream: `sim_gbm` = geometric Brownian motion
  (`S·exp((μ−σ²/2)dt + σ√dt·Z)` per step, each Z a Box-Muller pair);
  `sim_bridge` accumulates σ√dt·Z increments and linearly bridges them
  so the endpoint lands exactly on the target (each path step consumes a
  Box-Muller pair, and the final value reproduces the target to the last
  ULP on every backend); `sim_ou` = the exact Ornstein-Uhlenbeck
  recurrence `μ+(x−μ)e^(−θdt)+σ√((1−e^(−2θdt))/(2θ))·Z`;
  `sim_merton` layers Poisson jumps on the GBM diffusion (per step: one
  Box-Muller pair for diffusion, Knuth's Poisson on λ·dt — consuming
  nj+1 draws including the mandatory first — and, when nj > 0, one joint
  Box-Muller pair scaled by √nj·σ_j for the jump size). The six
  Black-Scholes helpers are scalars sharing one `bi_normcdf`
  (0.5(1+erf(x/√2))) so all three backends agree to the last ULP:
  call/put prices (S·N(d1)−K·e^(−rT)·N(d2) and the mirror), delta
  call/put (N(d1), N(d1)−1), gamma (φ(d1)/(S·σ√T)) and vega
  (S·φ(d1)·√T). Invalid parameters return an empty list / NAN natively
  and error on the interpreter. All ten emit through dedicated
  hand-built declare blocks (sim_* return i8* with an int/double-mixed
  tail — the step count truncates via `lume_bi_ftoi`; bs_* are pure
  double×5→double). `tests/quant-consistency.lume` pins 12 more seed-42
  rows (93 total, three legs byte-identical, including a re-seeded GBM
  replay); `tests/quant-refcheck.py` adds 13 absolute-k cases (101
  total); `tests/quant-fuzz.py` grows to 950 checks (10 series × 95
  rows) with endpoint/pricing/delta range and re-seed round-trip
  assertions. This closes the fixture-layout audit started this round:
  the linreg slope row lives inside a `let fit = linreg(...); print(...)`
  compound statement, so per-dataset print counts that scan for
  `print(`-prefixed lines undercount by one — the fuzz rows-per-dataset
  constant and every row-position assertion now use the fixture's true
  95-row layout with absolute positions instead of tail-relative
  offsets.
- **Hypothesis-testing builtins: `z_test` / `z_pvalue` / `tstat` /
  `ttest_p` / `chisq_stat` / `chisq_p`.** Six builtins that close the
  loop between the distribution CDFs and the quant list helpers: the
  one-sample z-statistic `(mean−μ)/(σ/√n)` (n≥1, σ>0), its two-sided
  p-value `2(1−Φ(|z|))` (clamped at 1), the one-sample t-statistic
  `(mean−μ)/(s/√n)` (n≥2, s>0; a zero-variance sample answers NAN),
  the two-sided t p-value `2(1−F_t(|t|, df))` (clamped at 1), the
  chi-square statistic `Σ(o−e)²/e` (equal-length lists, e>0, all
  finite) and its upper-tail p-value `1−F_χ²(x, df)` (clamped at 0).
  All three p-values reuse the interpreter's CDFs (`lume_bi_normcdf`
  / `lume_bi_tdist_cdf` / `lume_bi_chisq_cdf`), so the three legs
  agree to the last ULP; invalid parameters error on the interpreter
  and answer NAN natively. The list-arg statistics emit through the
  percentile-style special-case blocks on both native backends (int
  scalars widen via `lume_bi_float`), and the pure-scalar p-values sit
  in the BUILTINS Bi table / LLVM stats table. The new rt.c block sits
  before the `lume_bi_mean`/`lume_bi_stddev` definitions it reuses, so
  it carries forward declarations to keep C99 implicit-declaration
  warnings out of the text backend.
  `tests/quant-consistency.lume` pins 14 more rows (107 total, three
  legs byte-identical; z_test answers √3, chisq_p(3.84146,1) = 0.05
  and z_pvalue(2.5758) = 0.0100008 on the classic critical values);
  `tests/quant-refcheck.py` adds 14 cases (115 total) whose p-value
  references are definition-based — the Student-t CDF by numerical
  integration of its density, the chi-square CDF by closed forms
  (erf / exponential / gamma by parts) or integration — so agreement
  is independent of rt.c's ibeta/igamma formulas;
  `tests/quant-fuzz.py` grows to 1010 checks (10 series × 101 rows)
  with finiteness and (0,1] p-value assertions on the new rows.
- **Multivariate-distribution / copula samplers: `rand_dirichlet` /
  `rand_binormal` / `rand_gauss_copula` / `rand_t_copula`.** Four
  builtins that move the Monte-Carlo layer from univariate samplers to
  dependent vectors: `rand_dirichlet(alphas)` turns iid Gamma(αᵢ, 1)
  draws (the existing `bi_gamma_sample`, one stream draw per round plus
  the boost draw below shape 1) into a normalized simplex share and
  returns a flat float list; `rand_binormal(mu1,mu2,sd1,sd2,rho,n)`
  samples n pairs `x1 = μ1+σ1·z1`, `x2 = μ2+σ2·(ρ·z1+√(1−ρ²)·z2)` from
  two Box-Muller pairs each; `rand_gauss_copula(rho,n)` feeds the same
  correlated normal pair through `lume_bi_normcdf`, so both marginals
  answer probabilities in (0,1) and the row U(0,1)-marginality is exact;
  `rand_t_copula(rho,df,n)` scales the pair by a shared
  `√(df/chisq(df))` draw (`2·bi_gamma_sample(df/2)`) and pushes the
  result through `lume_bi_tdist_cdf`, adding the common-tail dependence
  the Gaussian copula lacks. Invalid parameters answer an empty list
  (n<1, rho outside (−1,1), NAN shapes); the interp / text / LLVM
  emitters mirror the sim_* list-argument blocks (flat list for
  dirichlet, scalar→list with a trailing count for the rest), and the
  LLVM blocks keep the calloc'd carrier free / reference-free pattern.
  `tests/quant-consistency.lume` pins 13 more rows (120 total, three
  legs byte-identical; every call re-seeds 42 so the rows replay from
  the absolute start of the stream); `tests/quant-refcheck.py` adds 12
  cases (127 total) whose references replay the same absolute stream
  position-by-position — `pcg_dirichlet` via the existing `pcg_gamma`,
  the copulas via `pcg_rand_normal` + `_bs_ncdf` / the definition-based
  `tdist_cdf_ref` integration — so agreement is independent of rt.c's
  gamma loop and ibeta CDF; `tests/quant-fuzz.py` grows to 1050 checks
  (10 series × 105 rows) with (0,1) simplex-share / probability and
  finiteness assertions on the four new rows.
- **Technical indicators — the signal layer: `rsi` / `macd` /
  `bollinger`.** Deterministic (no RNG), built on the #33 rolling /
  #48 risk family. `rsi(prices, window)` is Wilder's RSI: needs
  window+1 prices, so the first window rows answer NAN; after the
  initial simple average, gains/losses smooth with Wilder's alpha
  1/w; avgLoss == 0 answers 100 (no losing periods), an all-flat
  series answers 50. `macd(prices, fast, slow, signal)` answers an
  anonymous struct `{macd, signal, hist}`: the MACD line is
  EMA(fast) − EMA(slow) with alpha 2/(n+1), the signal line is EMA
  over the MACD line, hist = macd − signal — full-length columns,
  no leading NANs. `bollinger(prices, window, k)` answers
  `{mid, upper, lower}`: rolling mean ± k × sample stddev, the same
  window−1 leading NANs as rolling_mean/stddev; zero-spread windows
  collapse the band to mid. Emission: rsi joins the (list, i64) →
  float-list group; macd (i8*, i64, i64, i64) → i8* and bollinger
  (i8*, i64, double) → i8* join the anon-struct group beside linreg,
  on text and LLVM. **Bug fixed on the way**: `get(map, "k", [])`
  — a list default — used to answer the anon struct used for maps,
  so indexing it failed natively (`index read failed`); the list
  flavour now answers a `list<any>` (both emitters), matching the
  interpreter's dynamic read. `tests/r52-smoke.lume` pins preamble
  NAN + last-row values on all three legs (Python recomputes:
  80 / 81.132075 / 0.939072 / 0.652608 / 0.286465 / 103.333333 /
  106.388384 / 100.278283); `tests/quant-consistency.lume` grows to
  154 rows; `tests/quant-refcheck.py` adds rsi/macd/bollinger
  references (152 cases); `tests/quant-fuzz.py` grows to 1260
  checks (10 series × 126 rows). The quant surface now spans the
  full strategy chain: simulation → statistics → risk → portfolio →
  rolling evaluation → signals.
- **Rolling window portfolio evaluation: `rolling_sharpe` / `rolling_sortino`
  / `rolling_var` / `rolling_cvar`.** The backtesting shape called out after
  sim_portfolio: each builtin takes `(returns, window, scalar)` — window
  in [2, len], scalar is rf (Sharpe/Sortino) or level in (0,1)
  (VaR/CVaR) — and answers a fresh float list applying the exact #48
  formula to every window, so the first window−1 slots are NAN (rolling
  preamble) and zero-spread windows answer NAN (a data property, not an
  error; identical to the whole-series metrics). Invalid window /
  non-finite scalar errors on the interpreter and answer an empty list
  natively. Emission shape is a new (list, i64, double) → fresh float
  list group on both backends (window truncates through `lume_bi_ftoi`,
  the scalar widens through `lume_bi_float`); inference joins the float
  list group. `tests/r51-smoke.lume` pins preamble NAN + last-window
  values on all three legs (Python recomputes the last window:
  0.264906 / 0.57735 / 0.005 / 0.02); `tests/quant-consistency.lume`
  grows to 145 rows; `tests/quant-refcheck.py` adds five cases (149
  total) with `rolling_*_ref` window recomputation; `tests/quant-fuzz.py`
  grows to 1230 checks (10 series × 123 rows).
- **Transaction read-your-writes: `sql_tx_query(tx, sql[, params])`.**
  The missing P6 follow-up: `sql_query` opens a fresh read-only
  connection per call, so rows written (but not committed) by
  `sql_tx_exec` on the transaction's private connection are invisible to
  it — `sql_tx_query` prepares the SELECT on the transaction's own
  connection instead, returns the same row-map list shape (200-row cap,
  optional params list), and shares the `sql_query` read whitelist, so a
  write statement is rejected while the transaction stays OPEN exactly
  like `sql_tx_exec`. All three backends: interpreter
  (`native_sql_tx_query` in builtins_sql.c), native runtime
  (`lume_bi_sql_tx_query` in bridge_native.c), and both emitters
  (text/LLVM: two/three `i8*` args → fresh `i8*` row list; registered,
  whitelisted, arity 2–3). `tests/txq-smoke.lume` pins the semantics on
  all three legs (uncommitted read-back with params → visible; rollback
  → 0 rows everywhere; commit → visible to an independent `sql_query`);
  `tests/sqlite.lume` adds six check() cases (read-back, params bind,
  out-of-tx invisibility, write-guard reject, tx stays open, closed
  handle rejects); `tests/native-sql-consistency.lume/.expected` grows
  to 12 rows and passes interp/text/LLVM byte-identical.
- **Portfolio layer: `sim_portfolio(paths, weights)`.** A deterministic
  (no RNG) combination of several equal-length price paths into one
  portfolio-return series: `paths` is a list of strictly-positive finite
  price paths (each a list, length ≥ 2), `weights` one finite number per
  path (the sum is NOT normalised — equal weight means passing
  `[1/n]*n`), and the answer is a fresh float list of
  `r_t = Σᵢ wᵢ·(pᵢ,ₜ/pᵢ,ₜ₋₁ − 1)` for t = 1..len−1 — designed to feed
  directly into the #48 risk metrics and `max_drawdown`. Invalid shapes
  (empty / mismatched lengths / non-positive or non-finite prices /
  non-finite weights) error on the interpreter and answer an empty list
  natively. Nested-list reading on the native side walks
  `LUME_SLOT_OBJ` slots through a new `bi_list_at_list` helper, and the
  emission shape is two `i8*` list arguments returning a fresh `i8*`
  list on both backends (inference joined to the sim_* list group).
  `tests/pf-smoke.lume` pins 6 rows across all three legs;
  `tests/quant-consistency.lume` grows to 137 rows (deterministic, no
  re-seeding); `tests/quant-refcheck.py` adds six cases (144 total) —
  `sim_portfolio_ref` recomputes the weighted returns in Python;
  `tests/quant-fuzz.py` grows to 1190 checks (10 series × 119 rows)
  with two seeded positive price paths per series.
- **Portfolio / risk metrics: `value_at_risk` / `expected_shortfall` /
  `sharpe_ratio` / `sortino_ratio`.** Four deterministic metrics over a
  return series (no RNG — pure functions of the list, so no stream
  bookkeeping): `value_at_risk` answers the negative of the
  level-quantile (pandas linear interpolation, the same percentile rule
  pinned by `lume_bi_percentile`) — a positive loss figure;
  `expected_shortfall` the negative mean of every observation at or below
  that quantile (the tail conditional expectation); `sharpe_ratio` is
  (mean − rf) over the sample stddev (n−1, Welford); `sortino_ratio` is
  (mean − rf) over the downside deviation √(Σ min(0, x−rf)² / n)
  (population n). Zero volatility / zero downside deviation (no
  observation below rf) is a data property, so both ratios answer NAN on
  the interpreter exactly like the native backends instead of aborting
  the run; invalid parameters (level ∉ (0,1), n < 2, non-finite rf) still
  error. The four share the (list, scalar) → double emit shape with
  `bootstrap_se_mean` on both native backends (inference joined to the
  FLOAT group, `fn` dispatch in the shared emit block).
  `tests/va-smoke.lume` pins 6 rows across all three legs;
  `tests/quant-consistency.lume` grows to 131 rows (deterministic, no
  re-seeding); `tests/quant-refcheck.py` adds six cases (138 total) —
  `es_tail` / `sharpe_ratio_ref` / `sortino_ratio_ref` recompute the
  statistics in Python; `tests/quant-fuzz.py` grows to 1140 checks
  (10 series × 114 rows) with finite / NAN agreement on the four new
  rows.
- **Bootstrap / resampling family: `bootstrap_mean_ci` /
  `bootstrap_median_ci` / `bootstrap_stddev_ci` / `bootstrap_se_mean`.**
  The percentile-method bootstrap now sits on the deterministic PCG32
  stream: each replicate resamples the input by drawing exactly n iid
  plain uniform indices `(int64)(u·n)` (u is the top-24 unbiased draw, so
  the index stays in [0, n−1] and every call is a pure function of the
  stream position), evaluates the statistic (mean / median with the even
  count averaged / Welford sample stddev, n−1), sorts the n_iter
  replicate values and answers the α/2 and 1−α/2 quantiles with the
  pandas linear interpolation already pinned by `lume_bi_percentile`
  (`r = p/100·(m−1)`, `v[lo] + frac·(v[lo+1]−v[lo])`); the CIs return a
  fresh two-element float list, `bootstrap_se_mean` returns the sample
  stddev (n−1) of the replicate means (Efron convention) as a double.
  Invalid parameters (n<2, n_iter<1, α∉(0,1), non-finite) answer an
  empty list / NAN. `tests/bs-smoke.lume` pins 7 rows of one continuous
  seed-42 stream across all three legs; `tests/quant-consistency.lume`
  re-seeds 42 before every call so the five new rows (125 total) replay
  from the absolute start of the stream, byte-identical on interp / text
  / LLVM; `tests/quant-refcheck.py` adds five cases (132 total) —
  `pcg_bootstrap_ci` / `pcg_bootstrap_se` replay the same absolute stream
  position-by-position (`idx = int(u·n)`, sorted, linear-interpolated
  quantiles), so agreement is independent of rt.c's qsort and Welford
  loops; `tests/quant-fuzz.py` grows to 1100 checks (10 series × 110
  rows) with positivity / finite assertions on the new rows.
- **`map` type annotations bind correctly on the native backends.** A
  `let m: map = ...` annotation was parsed as a phantom named struct
  ("map" with no definition), so the native emitters bound through the
  named-struct aggregate path and stored the map's length where the
  pointer belonged — `print(m)` answered 2 for a two-key map and
  `get(m, k)` read null. The parser now redirects `map` to
  `type_anon_struct()` (list's existing redirect), and `type_compat`
  accepts any anonymous struct against a bare `map` annotation while
  named structs stay strict.

### Fixed

- **Sanitizer build: the while-shadow scope-mark array leaked on every
  program with a `while` loop.** `wh_saved` (e13e420, while 条件读外层绑定)
  is `xrealloc`'d on the first N_WHILE scan (16 ints, 64 B) and nothing
  owned it afterwards — `cg_free` never released it, so the ASan CI job
  failed `make asan-compile` with `Direct leak of 64 byte(s)` on
  `tests/native-consistency.lume` (which has a while) while
  `examples/native-fact.lume` (no while) stayed clean. Both emitters'
  cleanup now free it (`codegen.c` `cg_free`, `llvm_codegen.c` both error
  and success paths).
- **Native `sql_tx` handles are reclaimed at the serve request boundary
  (P6 close-out, 2026-10-23).** The interpreter's tx handle is an
  `OBJ_SQLTX` with a GC finalizer; the native handle is a plain heap
  struct, so a transaction a handler abandoned (never committed or
  rolled back) pinned its sqlite connection — and the BEGIN lock — for
  the life of the process: a real leak in long-running `server{}`
  deployments. `bridge_native.c` now keeps an open-handle registry
  (`g_open_tx[64]`, register/unregister in `lume_bi_sql_tx_begin` /
  `sql_tx_end`) and `lume_bi_sql_tx_cleanup()` rolls back + closes every
  still-open handle after each request. Regression:
  `tests/native_backends.sh` serve leg (`tests/native-sql-tx-serve.lume`
  — two sequential /open requests both get 200 with a fresh "ok" body;
  without the reclaim the second BEGIN would time out on SQLITE_BUSY).
- **LLVM backend: `(Val){ NULL, NULL, 0 }` initialised the 2-field Val
  with 3 elements** — `-Wexcess-initializers` (13 warnings on the
  clang/mingw build) since the LLVM-side struct shrank. All `llvm_cg_*.inc`
  sites now use `{ NULL, NULL }`; the text backend's 3-field Val is
  unchanged. The LLVM-23 `LLVMAppendBasicBlock` deprecation is also
  resolved (`LLVMAppendBasicBlockInContext` with the CG's context) —
  the mingw/clang build is warning-free.
- **Cast accessors (`int/float/str/bool(m, "k"[, d])`) answer in the cast's
  own type on every backend.** 86cb959's `get()` default fix had routed the
  accessor sugar through `emit_get_call`, whose shape branch refuses a
  default whose type differs from the key's declared type — but for an
  accessor that mismatch is the normal case (`int(acc, "n", 0)` reading the
  string `"42"` is the point of the sugar), and CI went red on
  `examples/lang-basics.lume` for five runs. The desugar moved to its own
  emitter (`emit_acc_call` + `rt.c`'s `lume_map_acc_*` / `lume_slot_acc_*`,
  which carry the interpreter's whole `value_from_map` table), and the
  interpreter now converts a fallback default over the same bridge the
  direct cast builds instead of handing it back verbatim
  (`int(m, "k", true)` used to answer `true`, `int(m, "k", null)` — `null`).
- **`/` on two integers types as float in the checker too** — it always was
  in the interpreter and both emitters. `arith_result` answered INT, so
  `{ ok: a / b }` typed the slot int and `str()` then rendered the slot's
  double bits as an integer (`"21 / 7 = 4613937818241073152"`, 3.0's
  IEEE-754 pattern). Latent behind the accessor compile error above.
- Text backend: double free in `acc_default`'s bool-default widening
  (`coerce()` had already consumed the intermediate value).
- `tests/native-consistency.lume`: +57 pinned lines covering the whole
  accessor table (found string/bool/container, cross-type defaults,
  any-receiver slot path, bool accessor, named-struct boxing).
- **ASan leak in `--compile-text`.** Two emit sites returned the `emit_instrf`
  result through `val_make` (which copies the register name) and never freed
  the original: `cg_index`'s tagged-any subscript (`j["k"]` / `j[i]`) and
  `cg_builtin`'s `json()` lowerer. Each leaked one ~7-byte `%tNNN` name per
  call; LSan reported 28 bytes / 4 allocations when compiling
  `tests/native-consistency.lume`. Both now use `val_take` (transfer
  ownership, no extra copy), matching `rt_call` / the binary-arith loads.
- **`tests/native_backends.sh` ns-chain leg skipped — not failed — without
  libLLVM.** The ns-chain block hardcoded `for leg in text llvm` while every
  other block walks `RAN` (the availability-filtered leg set), so a build
  with no libLLVM still attempted `--compile-llvm` and failed. It now
  iterates `RAN` with the same `text`/`llvm`/`*`-skip `case`, so an
  unavailable llvm leg is skipped like the `propagate` block already did.
- **Named LSan emitter gate + Mac blind spot closed (CI/docs).** The
  `--compile-text` / `--compile-llvm` / `native_backends.sh` LSan checks were
  inlined in `make asan`; they are now a dedicated `make asan-compile` target
  that `make asan` delegates to (single source of truth, no drift). LSan only
  fires on Linux — a Mac local `make asan` runs the emitter paths but its leak
  detection is a silent no-op — so the `test` job now also runs `make
  asan-compile` on Linux as a redundant gate, and `CONTRIBUTING.md` documents
  the rule plus the `scripts/rebuild-linux-build.sh` + Docker `gcc:12` local
  flow for Mac devs.
- **Windows CI: stdout/stderr binary mode for redirected output.** The
  Windows CRT defaults the C runtime's stdout/stderr to text mode, translating
  a written `\n` into `\r\n`. Program output redirected to a file (the pattern
  every `native-text` / `native-consistency` / `ai-verify` / `regex-smoke`
  test uses) therefore differed from the LF-terminated `tests/*.expected`
  files, and `diff` failed every such leg on the `Windows (MSYS2)` job while
  the same binaries passed on macOS/Linux — a hidden, platform-only breakage
  (the job went red before even reaching fuzz/ai-verify). `rt.c` now installs
  a `__attribute__((constructor))` that switches any non-TTY stdout/stderr to
  `_O_BINARY` on `_WIN32` (TTY stays text mode, so interactive display is
  unaffected). This lifts the per-entry `_setmode` calls already in
  `bridge_mcp.c` / `bridge_lsp.c` into the shared runtime, so the interpreter
  and every `--compile-*` native program (both link `rt.o`) are covered from
  one place.
- **AI gate: the gen-sql-tx seed read back inside the transaction.** `sql_query`
  opens its own read-only connection per call, so rows written in an open tx
  are invisible to it until commit; `make ai-produce` judged the seed a verdict
  violation on every platform (reproduced locally: "no such table: t"). The
  seed now commits before the read-back; `make ai-produce` passes 10/10
  verdicts (9 ready, 1 rejected-as-expected). The "read your writes inside a
  transaction" gap is recorded in TODO.md (P6 leftovers).

## [0.10.0] - 2026-10-22

### Added — namespace multi-file modules

- `import` across files now works end to end on all three backends: module
  functions, type declarations and top-level bindings join the build. Every
  referenced alias (entry `as` plus module-side `import`s) is registered per
  module, module member variables resolve to per-module top-level bindings,
  and module function cnames carry the module index so cross-file lambdas
  never collide. Previously `module A import B` only registered B under the
  entry alias — `b.double` failed with `unknown function`.
- Module bodies may contain lambdas and local functions (bare-name calls
  resolve to the module's own `<ns>.<name>` first), and the signature pass
  reads a lambda's real-time return type instead of the AST slot written
  after the loop, so `twice(g(n))` arguments type correctly.
- Pinned by `tests/ns-chain-main.lume` (entry→A→B) and
  `ns-lf.lume`/`ns-lib-lf.lume`; interpreter/text/libLLVM agree, wired into
  `native_backends.sh`.

### Added — server{} and env() on the native backends

- `server {}` compiles through the LLVM backend: route-table emission,
  handler `req.*` member access, `get(req.query_params)`/`int` calls,
  `now()`/`run()`, verb-group route expansion, and a default response when
  no handler matches. Handler parameters are type-checked with the same
  shape on the interpreter and text backends.
- `env()` is three-backend uniform; compiled binaries can read the same
  masked environment the interpreter exposes.

### Added — sql transaction builtins (P6)

- `sql_tx_begin/exec/commit/rollback` on all three backends: a runtime
  transaction object (OBJ_SQLTX with GC mark+release), text/LLVM emitters
  with the TEXT volatile-pointer lifetime fix, and bool parity (TY_BOOL +
  `i1` trunc). The native sqlite runtime links via `-DLUME_HAS_SQLITE=1`;
  a three-backend sql parity leg runs in `native_backends.sh` and CI
  (libsqlite3-dev on Linux, mingw-w64 sqlite3 on Windows).

### Added — statistical distribution builtins

- `normcdf` / `erf` / `tdist_cdf` / `chisq_cdf` / gamma family, with
  `examples/stats.lume` exercising them; a unary-minus literal-element
  inference gap in the signature pass was fixed on the way.

### Added — argv() in compiled binaries

- LLVM-compiled mains take `i32 (i32, ptr)` and initialize through
  `lume_cli_init(argc, argv)`; `argv()` returns trailing arguments as
  `list<string>` with interpreter/text-identical semantics.
  `infer_node_type` gained the argv branch on both `codegen_scan.c` and
  `llvm_cg_scan.inc`, and the 0-argument builtin path needed a special case
  past the table's `argc<1` guard.

### Added — compile-time builtin arity contract

- A `LUME_BUILTIN_SIG` signature table (exact / variadic / min-max) drives
  compile-time call-shape validation: wrong builtin arity is a named error
  at compile time instead of a runtime crash. `type_func` was split into
  `type_func_arity`, and the `str/int/float(map, key)` three-argument
  accessor forms compile three-backend uniform.

### Added — AI self-verification gate (P4)

- `scripts/ai-verify-loop.sh` + `make ai-verify-loop`: must-pass/must-reject
  seed sets are asserted every run, and each violation appends the file +
  stderr counter-example to `out/ai-rejects.log` for LLM retry feedback.
  Wired into `ci.yml` and `windows.yml` (`ai-verify` job);
  `assert_semantic()` converged to a static declaration contract
  (`verify-ai-semantics.sh`). The host-side producer (`ai_verify` exec
  tool) ships in agent-httpd.

### Fixed — for-in over an inline temporary loses the GC root (P9)

- `for x in range(60000)`-style inline-iterable loops crashed under
  allocation pressure: the iterable was `vm_pop`ped before the body ran, so
  the GC only rooted the stack below it, and any body allocation past the
  2 MiB threshold collected the iterable mid-iteration (use-after-free,
  reproduced as 0xC0000005 on a real server.lume workload). The iterable
  now stays on the stack across the whole loop — body stack balance keeps
  the slot valid, and every exit pops it.
- Regression: `tests/for-in-gc.lume` + `make for-in-gc-test`
  (interpreter-only; native backends have no GC) + the CI ASan job re-runs
  the fixture. Containers embedding lume should rebuild to pick this up.

### Fixed — build & portability

- asan/pack/fuzz variant builds drop `-DLUME_HAS_SQLITE=1` for
  `bridge_native.o` like the main build (the flag pulls references into
  rt.c symbols the variants do not link).
- `compile_mods` no longer uses GNU nested functions, so clang builds work.
- Windows/LLVM: the mingw32 triple normalises to windows-gnu/COFF (mingw
  lld rejects ELF), and the LLVM sql emitters no longer free LLVMValueRef
  operands (heap corruption).
- `LUME_CORE_VERSION` bumped `0.7.0` → `0.10.0` (single source for
  `lume --version`; the macro had been stale across the 0.8/0.9 releases).

### Changed — three-backend consistency sweep

- String family (upper/lower/capitalize/trim/contains/split/join/replace/
  substr), `exp`/`log`/`ln`, `bool()`, and int/float casts ported to the
  native backends; `any` dual-track `get`/`index`/`print`/`==` aligned —
  with a default value a literal-struct get resolves to the typed getter,
  without one it takes the boxed-any dynamic read.
- `make test` / `make asan` green (183 tests, 0 failed) with expected files
  regenerated from the interpreter.

### Fixed — sql guardrail: CTE bodies are read-only; top-level RETURNING writes work

- The write-CTE whitelist (0.9.4) opened a "RETURNING DML" head inside WITH
  subqueries on the assumption that SQLite 3.35 had added data-modifying
  CTEs. It had not — 3.35 added RETURNING on *top-level* DML only. Verified
  against the system library (3.51.0): even the documented
  PostgreSQL-style shape `WITH u AS (UPDATE … RETURNING …) SELECT …` fails
  at prepare with `near "UPDATE": syntax error`, on every release. The
  guard now rejects any write verb inside a CTE body outright ("write
  keyword in read-only WITH subquery"), so the impossible shape gets a
  named rejection instead of an engine syntax error at prepare time.
- `sql_write` / `sql_tx_exec` stepped the statement once and required
  `SQLITE_DONE`, so a top-level `UPDATE … RETURNING` / `INSERT …
  RETURNING` (valid since 3.35) failed with sqlite3's "not an error".
  Both runtimes now drain the RETURNING rows before the DONE check; the
  return value stays the affected-row count.
- `tests/sqlite.lume`: the three cases that encoded the impossible
  DML-in-CTE shape (`with update-returning atomic` and its two visibility
  checks — previously misattributed to a host sqlite < 3.35) are rewritten
  to the real thing: top-level `UPDATE … RETURNING` and
  `INSERT … SELECT … RETURNING` single-statement atomic writes, plus a
  negative case pinning the new CTE-body rejection.

### Added — sort() on the native backends

- `sort(l)` is now three-backend uniform: `--compile-text` / `--compile-llvm`
  emit a call to the rt helper `lume_list_sort` (src/rt.c), which mirrors the
  interpreter's `native_sort` by scanning the element slots — all int (exact
  i64 compare), all float (double), or all string (`strcmp` byte order);
  empty and single-element lists return unchanged. Mixed or bool/container
  elements fail at run time with the interpreter's exact error wordings.
- On the native backends the argument must be a statically typed `list`; an
  `any` value (e.g. a `json()` result) needs a `get()`/annotation step first,
  like the `m["k"]` scalar-read boundary. `sort(42)` is a compile-time error.
- `tests/native-consistency.lume` gained a sort probe segment (int, float,
  string, big-int exact, empty, in-place + return, index after sort, `str()`);
  `tests/native_backends.sh` is now line-ending agnostic (CRLF-normalized
  diffs), so the three-way suite runs green on Windows builds too.

### Fixed — native HOF element reads follow the list's real element type

- A `let d = map(...)` binding could be typed `list(any)` when the signature
  pass collected it before the lambda's return was pinned, and every later
  reduce/map/filter element read degraded that to INT — a double list was then
  read by its i64 slots, surfacing as garbage on the native backends
  (`examples/stats.lume` printed `stddev = 2.1474e+09` instead of `2.08713`;
  the interpreter was correct). The signature pass now walks a HOF
  initializer before typing the binding, and a placeholder pinned from an
  any/empty list is upgraded once the real element type resolves.
- A closure consumed by lists of two different element types is now a
  compile-time "one closure shape per lambda" error instead of silently
  reading one of them wrong.

### Added — write-CTE whitelist for sql_write (P6 gray zone)

- `sql_write` now accepts a single `WITH` prefix: `WITH [RECURSIVE] name
  [(cols)] AS (…) [, …]` followed by one whitelisted write statement.
  Subqueries must be SELECT/VALUES (a nested WITH is allowed) and are
  checked read-only at any depth; DML inside a CTE (SQLite 3.35+
  data-modifying CTE) requires a top-level RETURNING, and an UPDATE/DELETE
  inside a WITH must carry a top-level WHERE before that RETURNING — a
  WHERE inside a nested expression never exempts it. The trailing statement
  still passes the exact single-statement whitelist (INSERT/UPDATE/DELETE/
  CREATE TABLE), so the whole CTE write stays one atomic statement with no
  transaction API changes; `;` multi-statement and `WITH`-prefixed
  CREATE/ALTER are rejected.
- `tests/sqlite.lume` gained a write-CTE regression segment (SELECT/VALUES/
  multi-CTE accepts, WHERE-in-CTE does not exempt a WHERE-less body,
  DML-in-CTE RETURNING/WHERE rules, dangerous words, no-trailing-statement
  and multi-statement rejects, atomic UPDATE…RETURNING…INSERT).

## [0.9.3] - 2026-10-19

### Added — lexer scientific notation support

- `lexer.c` now supports scientific notation (`e`/`E` exponent) in numeric
  literals, matching the usual `1e5`, `2.3E-4` style numeric constants.
- Added `tests/sci.lume` (11 assertions) and `tests/sci-bad/incomplete-exp.lume`,
  wired as the `sci-test` Makefile target.

## [0.9.2] - 2026-10-19

### Fixed — CI test-infrastructure stability (no library/runtime change)

- The `route-cap` integration test bound port `18893`, which the GitHub
  macOS runner also occupies — so the server failed to listen and the probe
  failed intermittently. Moved the test server to the cold high port `39184`
  (Makefile target + `tests/route-cap-srv.lume`), and added a 30s readiness
  wait with up to 5 curl retries (1s apart) so transient bind/listen races no
  longer read as a product regression. These are test-harness-only changes;
  the committed `lume-core` binary is byte-identical to `v0.9.1`.

## [0.9.1] - 2026-10-19

### Fixed — two hygiene bugs caught by CI (absent from every prior tag)

- `typecheck.c` leaked the Pass-C parameter scratch (40 bytes per module
  that declares functions): since P2 Stage 1 `type_func()` copies the
  caller's `params`, the `calloc`'d scratch had no owner and was never
  freed — caught by LeakSanitizer on the CI seed replay (`make fuzz-ci`,
  live since `e6896b9`). Now `arena_alloc` + `memset`, abandoned, matching
  the existing `N_FUNC_LIT` path.
- `lexer.c` `lex_istr`'s `EXPR_STR` branch skipped a backslash escape with
  an unguarded `p += 2`; a trailing backslash as the last source byte
  walked `p` past the NUL terminator and read one byte past the allocation
  (ASan heap-buffer-overflow, originally surfaced by the v0.8.0 fuzz
  campaign and minimized to `s!"{"a\`). Guard added (mirroring
  `lex_string`); regression fixture
  `tests/interp2-bad/istr-esc-at-eof.lume` added.

## [0.9.0] - 2026-10-19

### Added — bump-arena allocator for the AST (P2)

- One bump arena per compilation unit. `nalloc` and every parser-owned
  string route into it; the free-path patches (`ecd7afc` / `7e5d2f8`)
  and the orphan journal are deleted — journaling existed solely to
  reclaim mid-parse garbage, which an arena reclaims wholesale at reset.
- The tree death points are unchanged in count: the five former
  `node_free` call sites (compile/run CLI, `--dump`, fuzz cycle per
  input, loader module destroy, LSP per document version) become arena
  resets. The loader's per-request AST read stays reset-free; teardown
  is module-set granularity (watch reload / full unload).
- `type_func` copies its `params` into the arena so the Type graph keeps
  its own teardown regime (`type_release_all` unchanged — out of scope
  for v1, per `docs/ARENA.md`, which records the design and the hazard
  enumeration).
- Gates: `make test`; the ASan suite at 182 tests with zero leaks; the
  native-backend three-leg suite (interp / text IR / libLLVM); fuzz seed
  replay (94 seeds) under the standalone fuzz driver. The P2 Done-when
  audit is closed: the free-path patches were deleted without the suite
  noticing.

### Changed — codegen Asg / arm-scope strings move into the arena

- The `name` / `slot` strings bound per assignment record and switch-arm
  scope are now allocated from the arena (`arena_strdup`). The four
  hand-copied variants (`codegen.c` / `llvm_codegen.c` / `codegen_scan.c`
  / `llvm_cg_stmt.inc`) changed in one commit to preserve text/llvm
  case parity.
- `asg_truncate` / `arm_scope_truncate` degenerate to counter resets.
  The LTys tables (globals / sigs / structs) keep the malloc regime, as
  do the emitter temporaries (`free(r.v)`).

### Verified — Windows CI and Linux llvm legs (P3 / P4)

- P3 closed: the native-backend three-leg suite (interp / text IR /
  libLLVM) is green on Windows CI (run 39216687894).
- P4: `tests/llvm-json` verified on a real Linux machine (OrbStack
  Docker, clang 18), including the gotcha that the tree copied into the
  container must exclude host build artifacts (`bin` / `build` /
  `build-asan` / `out`) or the stale Mach-O mtimes shadow a rebuild.

## [0.8.0] - 2026-10-19

### Added — `sha256()` / `hmac_sha256()` message-digest builtins

- Two general-purpose digest builtins on the interpreter path:
  `sha256(s)` returns the SHA-256 of a string as 64 lowercase hex chars;
  `hmac_sha256(key, msg)` returns HMAC-SHA256 the same way. Strings are
  hashed by explicit length, so embedded NUL bytes are safe.
- Motivation: payment integration (request signing, callback verification)
  needed a message digest; the tree previously had only `crypt_sha512`
  (one-way password hashing via system `crypt(3)`), which cannot sign.
- The SHA-256 implementation is vendored under `src/compat/sha256/`
  (public domain, no external dependency — the tree stays OpenSSL-free
  for this, cf. `src/compat/regex`). HMAC is the standard RFC 2104
  construction over it (64-byte block; keys longer than the block are
  pre-hashed).
- Same scope as `crypt_sha512` / `sql_*` / `http_*`: interpreter-path
  builtins. The compiled native backends (text IR / libLLVM) do not lower
  these calls — `--compile` reports "call to unknown function", exactly as
  it already does for `crypt_sha512`. Server/DSL scripts run interpreted,
  so this does not affect them.
- `tests/test-crypto.lume` (wired as `make crypto-test`, included in
  `make check` and `make test`) covers FIPS 180-4 / RFC 4231 known
  vectors, the multi-block streaming path, the long-key (>64B) HMAC
  branch, empty key/message edge cases, output shape, and four rejection
  cases (non-string args, wrong arity).

### Added — `base64_encode()` / `base64_decode()` builtins

- RFC 4648 base64 on the interpreter path: `base64_encode(s)` and
  `base64_decode(s)`. Binary-safe in both directions — embedded NUL and
  non-UTF-8 bytes survive roundtrips; encode pads with `=` to a multiple
  of 4.
- `base64_decode` validates strictly: length must be a multiple of 4,
  characters outside the alphabet are rejected, and padding is only
  accepted at the end (at most two `=`, never mid-string); failures raise
  a catchable VM error (usable under `try()`).
- Motivation: PayPal integration needs HTTP Basic credentials
  (`base64(client_id:client_secret)`). Same interpreter-path scope as
  `sha256` / `hmac_sha256` — the compiled native backends do not lower
  these calls.
- Covered by `tests/test-crypto.lume` (RFC 4648 known vectors, NUL-byte
  and multibyte roundtrips, three rejection cases).

### Added — `sql_write` accepts additive `ALTER TABLE ... ADD COLUMN`

- `sql_write`'s guardrail allowed INSERT / UPDATE (WHERE) / DELETE (WHERE)
  and CREATE TABLE only. Apps that evolve their schema against an existing
  database had no way to add a column: `ALTER TABLE` was rejected
  wholesale, so a "regenerate against an old database" flow died on
  `no such column`.
- The one ALTER form now allowed is strictly shaped:
  `ALTER TABLE <ident> ADD [COLUMN] <ident> <typedef>` — bare or
  double-quoted identifiers, and a typedef restricted to identifiers,
  numbers, spaces, parens and commas (TEXT / INTEGER / REAL /
  VARCHAR(120) / DECIMAL(10,2)). Every other ALTER (DROP/RENAME COLUMN,
  ALTER COLUMN, table constraints) is still rejected, as are string
  literals in the typedef and multi-statement tricks. SQLite
  independently refuses the shapes that would be unsafe here (ADD COLUMN
  takes no PRIMARY KEY/UNIQUE and no CURRENT_TIMESTAMP default).
- Read-side probing for such migrations rides the existing SELECT-only
  channel: `SELECT name FROM pragma_table_info('<tbl>')` (table-valued
  pragma function) returns the live column list over a read-only
  connection. `PRAGMA table_info(...)` as a statement stays rejected.
- `tests/sqlite.lume` covers the new path (probe, add, old rows read
  NULL, writes to the new column, table still usable afterwards) plus
  five rejection cases.

### Fixed — Windows build no longer needs the libgnurx package

- The MinGW/Windows build linked `-lregex` for the POSIX regex builtins
  (str_match/regex), provided by the MSYS2 package
  `mingw-w64-ucrt-x86_64-libgnurx`. That package is no longer resolvable
  from the MSYS2 repositories (the Windows CI started failing with
  "target not found"), and its LGPL license would not be compatible with
  vendoring into this MIT project.
- The build now vendors a self-contained POSIX regex implementation —
  musl libc's (itself trimmed TRE, BSD-style licensed) — under
  `src/compat/regex/` and compiles it into the binary on Windows. The
  include path makes `<regex.h>` resolve to the vendored header, so no
  external package and no link flag are needed. Three portability edits
  were made to the vendored code, each annotated in place: a
  self-contained `regex.h` (musl's includes `bits/alltypes.h`), a `hidden`
  visibility shim (musl internal macro), and `ALIGN()` casting through
  `uintptr_t` instead of `long` (32-bit `long` on Windows/LLP64 would
  misalign allocations).
- Verified two ways: the vendored implementation matches the system regex
  behavior on a 19-case probe (patterns, capture groups, error codes), and
  a full mingw-w64 cross-build produces a PE binary with
  regcomp/regexec/regfree/regerror defined inside it and no `-lregex` in
  the link line. The Windows CI workflow drops the libgnurx install step.

### Fixed — LeakSanitizer clean under `make asan` (native backends)

- Scope exits in both native emitters reset a locals table by shrinking its
  count only, which stranded the discarded entries' `xstrdup`'d strings
  (match-arm binds, if-let binds, let-expression bindings). The emitters now
  truncate through `asg_truncate` (text: frees name + slot; llvm: frees name,
  the slot being an LLVM-owned `LLVMValueRef`), mirroring `asgs_free`.
- The scan-phase match inference had the same shape of bug: its arm-bind scope
  was reset between arms by count, and the end-of-match `arm_scope_clear` only
  saw the already-truncated table — so every match binding's name leaked.
  It now truncates through `arm_scope_truncate` (both backends).
- `emit_struct_to_map` leaked the throwaway result of each map-put runtime
  call; it is now freed (the map is mutated in place).
- Verified in a Linux container (LLVM 18, matching CI) with
  `make asan`: every leg — examples `--check`, lang-basics, native-fact both
  backends, `native_backends.sh` (interp/text/llvm), smoke — reports zero
  leaked bytes. Follows `ecd7afc`, which fixed the same class of leak on the
  interpreter path (let-expression nodes, local-function synthetic names).

### Fixed — SQLite connections arm a busy timeout (concurrent writers)

- `sql_query` / `sql_write` open a fresh connection per call, and sqlite3's
  default busy timeout is 0 — under prefork workers (`WORKERS > 1`) or any
  second process touching the same file, a write arriving while another
  connection held the write lock failed instantly with
  `database is locked`. Empirically: 8 concurrent processes × 60 guarded
  `UPDATE`s lost 7 of 8 runners and 84% of the writes.
- Both open paths (read-only and read-write) now arm
  `sqlite3_busy_timeout(5000)`: contention becomes a bounded wait instead
  of a spurious failure. Single statements remain atomic either way — the
  timeout changes failure semantics, not isolation.
- Override with `LUME_SQLITE_BUSY_TIMEOUT_MS` (clamped to [0, 60000];
  `0` restores stock fail-fast; unparseable falls back to the default).
  With the timeout armed, the same 8×60 probe lands every write
  (0 failures, no lost updates).
- Does not change the statement guardrails: still single-statement only
  (`BEGIN`/`COMMIT` remain rejected), still parameter-bound values, same
  read/write DDL policy.

## [0.7.0] - 2026-10-14

### Added — let expressions: `let x = e1 in e2` (SPEC §4.15)

- `let` now has an expression form (Lean's let-in): `let x = e1 in e2` evaluates
  `e1`, binds `x` to its value, and evaluates `e2` — whose value is the whole
  expression's. The binding is visible only inside `e2`: it disappears after,
  it shadows an outer same-name binding for the body only (safe even on the
  same source line), and it nests. The body is a full expression and extends
  as far right as possible; `in` is required in expression position (a plain
  statement `let` still ends with `;`, and `let x = e1 in e2;` at statement
  position folds into an expression statement).
- Optional annotation: `let x: T = e1 in e2` constrains `e1` (checked at
  compile time) and types the binding; the expression's own type is the body's.
- Scoping is one mechanism per backend, all agreeing: the checker uses a child
  scope; both native emitters register the binding's slot at emit time and
  truncate the locals table when the body ends (the same mechanism match-arm
  binds use, which also keeps same-name nested let-expressions from aliasing
  one slot); the interpreter runs the body in a child env. The checker stamps
  the body's computed type onto the node so scan-time inference reads the
  stamp instead of descending into the body — where the binding's name would
  resolve against the outer scope and could disagree with what the emitter
  stores.
- `tests/letexpr.lume` (12 assertions, all three backends) +
  `tests/letexpr-bad/` (out-of-scope use, annotation mismatch, missing `in`)
  wired as `make letexpr-test`; a probe section was added to
  `tests/native-consistency.lume` (baseline now 408 lines).

### Added — local functions: `func` inside a function body (SPEC §4.14)

- A `func` declared inside a function body is a **local function**: it compiles
  like any module function (no capture — its body sees only its own parameters)
  but resolves nowhere except inside the function that declares it. Hoisting
  semantics: the local is visible throughout its enclosing function, including
  before its declaration (forward calls and recursion work in all three
  backends), and it shadows a same-named top-level function for the whole
  enclosing body while leaving top-level callers untouched.
- The mechanism is one rewrite run before the checker and re-run as a guard
  before each native emitter: every nested declaration gets a synthetic
  module-level name, and every call inside the enclosing function that spells
  the local's source name is rewritten to it (`lume_resolve_localfns`). Scope
  is therefore decided by one shared rewrite rather than four separate symbol
  tables — a call outside the enclosing function still spells the source name,
  which resolves to nothing, identically in the interpreter, the text backend
  and libLLVM.
- Two locals in one enclosing function may not share a name — diagnosed with a
  dedicated message, since the blanket rename would otherwise route both call
  sets to whichever synthetic name was assigned last. Same-named locals in
  different functions are independent.
- The interpreter pre-binds a frame's locals before the body runs (mirroring
  the native signature tables), which is what makes forward references agree.
- `tests/localfn.lume` (12 assertions, all three backends) +
  `tests/localfn-bad/` (duplicate name, out-of-scope call) wired as
  `make localfn-test`; a probe section was added to
  `tests/native-consistency.lume` (baseline now 398 lines).

### Added — `range()` on the native backends

- The interpreter always had `range(stop)` / `range(start, stop)` /
  `range(start, stop, step)`; both native emitters rejected iterating it
  ("can only iterate a list or a map") because the call had no static type.
  Both now lower it onto a new rt helper `lume_range(double, double, double)`
  that runs the same double arithmetic the interpreter's `native_range` does —
  whole values become INT slots, so `print`, `for-in`, `len`, indexing, `str`
  and `stringify` agree compiled or interpreted. `step == 0` reports the
  interpreter's run-time error.
- Element type is static: all-int arguments infer `int[]`, one float argument
  makes the whole range `float[]` (the interpreter's whole-value rule keeps
  every element readable through either accessor; `lume_list_at_f` now widens
  an INT slot instead of reinterpreting its bits). An argument without a
  static numeric type keeps the usual cannot-infer diagnostics.
- `len(x)` now infers `int` in both emitters' static type read (it previously
  had no branch, so `range(len(xs))` inside a `for-in` could not type).

### Added — tuples `(a, b, ...)` (SPEC §4.13)

- A tuple is an anonymous struct with positional fields `_0`, `_1`, … — zero new
  runtime representation. Three pieces of sugar, all folded at parse or lookup
  time: the literal `(a, b)` builds an `N_MAP_LIT` with auto-numbered keys
  (`(a)` without a comma stays a grouped expression; `(a,)` is a 1-tuple),
  `p.0` reads field `_0` (a member read like any other, so `._N` works too),
  and `let (x, y) = e` is a renaming `let { _0: x, _1: y } = e` reusing the
  §4.12 destructuring machinery (`fields[i]` vs `names[i]`, which also lands
  the `{ field: name }` rename form for plain records).
- **Context-only naming, everywhere** (fixes a latent cross-feature bug): a
  map/struct literal is a named struct *iff* its context says so — a `let`
  annotation, a return type, a parameter, a struct field, or (new) the other
  operand of `==` / `!=`. The old field-name shape scan in the two native
  emitters' `resolve_struct_lit` answered differently from `infer_node_type`
  inside one backend — under `type T2 = { _0: int, _1: int }`, the un-annotated
  `let rec = { u: 8, v: 9 }` stored a `%Rec` aggregate into the `i8*` slot the
  inference had allocated and the next `rec.u` handed the field value to
  `lume_map_get_i` as a pointer (SIGSEGV in both backends, pre-existing); the
  same disagreement also mis-built nested tuples. The scan is gone; call sites
  and the libLLVM `return` now pin `expect` from the parameter / return type,
  exactly where the checker threads it.
- Known lexical edge (documented, not a bug): a chained `p.1.0` fuses into the
  float literal `1.0`; nested positional access uses the underscore form
  `p._1._0`.
- Regression: `tests/tuple.lume` (15 assertions, interpreter + both native
  backends 15/15), `tests/tuple-bad/` (4 compile-time rejects with pinned
  diagnostics), and a tuple probe block in `tests/native-consistency.lume`
  (baseline 356 → 374 lines; interpreter / text / libLLVM all agree).

### Added — record destructuring `let { a, b } = e;`

- Binds every name in the pattern to the like-named field of `e` in the current
  scope, evaluating `e` exactly once — the emitter's spelling of
  `let a = e.a; let b = e.b;` with one `e` (SPEC §4.12). The MVP pattern is a
  plain comma-separated name list; the rename form `let { field: name, ... } = e`
  landed with the tuple work (§4.13), which needed exactly that machinery.
  The rules
  mirror `m.x` exactly: a shaped map (literal or named struct) must declare every
  pattern name, a dynamic `any` map is accepted at type-check but refused by both
  native emitters ("no known shape"), and an enum init is steered to `if let` /
  `match`.
- Named-struct field binding arrives as an *address* (`agg = 0`); the store now
  loads it into the value first. This fixes a latent cross-feature bug where
  `let v = o.inner` (and any struct field bound into a local) read garbage from a
  store of the un-loaded address in both native backends — ordinary `let` of a
  struct field benefits too.
- Regression: `tests/letpat.lume` (12 assertions, interpreter + both native
  backends 12/12), `tests/letpat-bad/` (7 compile-time rejects with pinned
  diagnostics), and a destructuring probe block in `tests/native-consistency.lume`
  (interpreter / text / libLLVM all agree).

### Added — trailing closures `f(args) { |x| body }`

- A closure written after a call's argument list is sugar for passing it as one
  more argument (Rust / Swift / Kotlin's trailing closure). `parse_postfix`
  folds it into an ordinary `N_FUNC_LIT` argument, so the checker and both
  emitters never learn the syntax exists — one grammar, three consumers.
- The body is a statement sequence whose final expression (the one with no
  trailing `;`) is the closure's value, Rust-style; a trailing `;` discards it
  (the closure returns null). `let` / `if` / `for` / `while` statements are all
  allowed in the body. Parameters are bare names with optional `: type`
  annotations, exactly as capable as the arrow form; the zero-parameter form is
  `{ || body }` (the lexer fuses the two pipes into one `TOK_OR` token).
- Recognition is a two-token lookahead: `{` must be followed by `|` (or `||`).
  A map literal's first token is a key, a structure update starts with the base
  name, and no statement or match arm starts with `|` — so the condition braces
  of `if (f()) { ... }`, `while (f()) { ... }` and the scrutinee brace of
  `match (f()) { ... }` can never be swallowed. A `{` not followed by `|` is not
  a trailing closure (pinned by a negative test).
- Covered by `tests/trailing-closure.lume` (16 checks: expression bodies,
  multi-statement bodies, discarded values, zero-parameter, nesting, return
  position, statement bodies, annotated parameters + capture, for-in position,
  and the brace-intactness regressions), `tests/trailing-closure-bad/` (5
  rejection cases wired into `trailing-closure-test`), and a probe block in
  `tests/native-consistency.lume` (`.expected` 337 → 346). SPEC §4.11 documents
  the semantics.

### Fixed — a void call result used as a value emitted the type name as the value

- Both native emitters carried the result of a call to a function with no
  return type as the literal token `void`. Used as a value (e.g. pushed into a
  list), the text emitter produced `i8* void` — not IR, and clang rejected the
  module with "expected value token"; the libLLVM emitter refused at the
  consumer ("push() needs a list and a scalar"). The interpreter's answer is
  `null`, and both emitters now build a null `i8*` (`TY_NULL`), so
  `push(out, f(x))` with a value-less closure prints `[null,null,null]` on all
  three backends. Found by the trailing-closure probe, reachable with the arrow
  form too.

### Fixed — `get(m, k)` read every value through the int getter (native)

- The two-argument form of `get()` dispatched on the *default's* type, and with
  no default it always used `lume_map_get_i`. A string value then read back as
  `0` (`print(get(m, "name"))` printed `0`, the interpreter printed `lume`), and
  a default of a different type than the key's value was silently mis-read.
- Both emitters now dispatch on the key's **static shape** — the key set a map
  literal carries, or a declared struct's fields — so the value at a key the
  shape declares reads through the getter its type needs (`_s` / `_f` / `_i` /
  `_obj`). `infer_node_type`'s `N_MAP_LIT` keeps that shape (both copies), and
  `let x = get(m, "name")` infers without an annotation. A default whose type
  conflicts with the key's declared type is a compile error (it could only ever
  fire for an absent key, and one static call cannot serve both).
- The libLLVM emitter also passed **two** operands to the three-operand runtime
  getters, so an absent key read whatever sat in the default's register/stack
  slot — `str(get(m, "missing"))` printed garbage such as `73896`. The call now
  always passes three operands, the third being the read flavour's zero.
- An absent key with no default still reads as the flavour's zero in the native
  backends (the interpreter says `null`); use `get(m, k, null)` for the null
  semantics, which all three backends share.
- `tests/native-consistency.lume` covers the matrix (string/int/float/bool,
  annotated and bare `let`, typed defaults, absent key, null default, inline
  literal); `tests/map-get-default-bad.lume` pins the conflict rejection, wired
  into `map-index-test`.

### Added — `if` as an expression (`if (cond) expr else expr`)

- The Lean if-then-else in expression position. `parse_primary` folds it onto the
  very same `N_TERNARY` the `? :` operator builds, so every §4.9 rule applies
  unchanged — only the taken branch is evaluated, branch types unify under the
  ternary rules (same type → that type; int/float mix → float; anything else is a
  type error), and else-if chains fold right. `let`, direct arguments, arithmetic
  operands and ternary mixing all work, and all three backends agree because they
  share `cg_ternary`.
- `else` is **required**: without it the false branch has no value, exactly as a
  bare `c ? a` is not a ternary. Missing `else` is a compile error
  (`an if-expression needs an else branch`), pinned in `tests/ternary-bad/`.
- **No braces** in the expression form: `{` already means a map literal /
  structure update in expression position, so `if (c) { 1 } else { 2 }` would be
  ambiguous. The statement form `if (cond) { ... }` keeps its braces and still
  allows a bare `if`.
- Covered by `tests/ternary.lume` (10 new checks), `tests/ternary-bad/` (2
  rejection cases wired into `ternary-test`), and the if-expression block in
  `tests/native-consistency.lume` (`.expected` 318 → 328). SPEC §4.10 documents
  the semantics.

### Added — ternary conditional `cond ? then : else` (SPEC §4.9)

- Right-associative, sitting between `or` and assignment; only the taken branch
  evaluates (lazy in the interpreter, three-block alloca+br+load in both native
  emitters). Branch types unify under the ternary rules: same type → that type,
  int/float mix widens to float, anything else is a type error.
- Coexists with the result-propagation suffix via an adjacency-first rule:
  a tight `f()?` is always propagate; a spaced `?` defers to a paired-colon
  lookahead scan, so `divide(8, 2)? > 3 ? "big" : "small"` stays legal.
- `tests/ternary.lume` (16 checks: precedence, right-assoc, int/float widening,
  map-value colons, call conditions, propagate interplay, multiline, lazy
  branch) wired as `ternary-test`; native-consistency probes with all three
  legs agreeing.

### Fixed — `TOKEN_NAMES` was missing four entries, shifting every diagnostic

- `src/token.c`'s `TOKEN_NAMES[]` predated the `TOK_ISTR`, `TOK_ENUM`, `TOK_MATCH`
  and `TOK_PIPE` tokens, so every token after `TOK_STRING` reported the *next*
  token's name: `if` printed as `else`, `while` as `for`, `(` as `}`, `;` as `?`,
  and so on. Every "unexpected token" / "expected X, got Y" diagnostic was
  misleading. The four names are restored and the table is 1:1 with `TokenType`
  again.

### Fixed — bare `list` type annotation (`let x: list = ...`)

- The type-annotation parser used to route the bare `list` keyword through the
  `TOK_IDENT` branch and build `type_struct("list")` — a *named struct* with no
  definition. The native emitters then read `llvm_type_of` / `ty_of` off the
  annotation and saw "no codegen type" for `let x: list = null`,
  `let x: list = [...]` and friends, while the interpreter (which uses the
  checker-resolved type) tolerated it. `parse_type` now recognizes `list` and
  builds `type_list(any_type())` — the same `TY_LIST` representation list
  literals already carry — so all three backends agree byte-for-byte. (`map`
  stays a struct reference: a map is an anonymous struct at runtime and there
  is no singleton `TY_MAP` to annotate.) Pinned by the `: list` block in
  `tests/native-consistency.lume` (`.expected` 295 → 299). Also added the
  `any_type` declaration to `parser_internal.h` (it lived only as a
  backend-local forward declaration before).

### Fixed — for-in over a list holding nulls (`list<any>`)

- A list literal whose elements include `null` (`[null, 2, null]`) decays to
  `list<any>`; the for-in loop variable then carries a boxed `LumeSlot*`
  (`i8*`). Both native emitters used to reject this — `llvm_type_of` /
  `ty_of` had no `TY_ANY` case (returned NULL → "no codegen type"),
  `list_at_fn(any)` returned NULL (→ "cannot be iterated"), and `cg_print`
  fell through to "print() cannot print this value". The element is now
  spelled as `i8*` in both emitters (`case TY_ANY` in `llvm_type_of` and
  `ty_of`), `cg_for_in` selects the new `lume_list_at_slot` accessor when the
  element type is `any`, `cg_print` dispatches `TY_ANY` to the new
  `lume_print_slot` runtime helper (which reuses `slot_emit` for tag-based
  rendering), and both `declare` blocks name the new runtime functions. Two
  runtime helpers added to `rt.c`: `lume_list_at_slot` (returns a pointer to
  the boxed slot so any tag survives the iteration) and `lume_print_slot`
  (renders it via `slot_emit` + newline). Pinned by the for-in-over-nulls
  block in `tests/native-consistency.lume` (`.expected` 299 → 307).

### Fixed — map field dot access (`m.x`)

- A runtime map (anonymous struct) answering `.x` used to be refused by both
  native emitters with "field access on a non-struct value", while the
  interpreter read it with a plain `map_get`. The type checker already
  requires `x` to be a *declared* key (typecheck_expr.c rejects unknown keys
  at compile time), so the value is always present at run time. `cg_member`
  now lowers `m.x` onto the same `lume_map_get_*` family the `?` unwrap and
  the index read use, dispatched by the field's value type (`map_get_fn`),
  with a zero of the right type as the default. `member_field_type` was
  taught the map case too, so `let v = m.x` infers the value type. A bool
  value travels out of `lume_map_get_i` as an `i64` (0/1) and is narrowed to
  the backend's `i1` bool representation. Pinned by the map-field block in
  `tests/native-consistency.lume` (`.expected` 307 → 313). `m["k"]` indexing
  remains a separate, intentional boundary (native returns an opaque value).

### Fixed — null values inside containers in the native backends

- A map or list literal with a `null` value (`{ "b": null }`, `[1, null]`),
  plus `put`/`push` with a null, `{ base with k: null }` and a null default
  in `get(m, k, null)`, used to be refused at compile time by both native
  emitters while the interpreter accepted them. A null now rides the existing
  obj slot with a null payload: the value-dispatch tables route `TY_NULL` to
  `lume_*_push_obj` / `lume_map_put_obj` / `lume_map_get_obj`, and the
  runtime's obj paths already rendered a null payload as the bare word
  `null` — so print/str/stringify/eq/get are three-backend consistent.
  `lume_map_print` / `lume_list_print` on a null pointer now print `null`
  (not `{}` / `[]`, which remain what a live empty container prints).
- `str(null)` / `stringify(null)` work natively too (the serializer's null
  entry point). The bare `let x: list = null` refusal is fixed separately
  (see the `: list` annotation entry above); the one remaining native-only
  refusal is for-in over a list holding nulls (the element decays to
  `list<any>`, same refusal class as a heterogeneous list). Pinned by the null
  probe block in `tests/native-consistency.lume` (`.expected` 267 → 282) and
  ten interpreter assertions in `tests/interp2.lume` (54 → 64).

### Fixed — named-struct print() / str() / stringify() in the native backends

- A named struct (`type P = { ... }`; a bare GEP aggregate with no runtime
  header) used to be refused at compile time by both native emitters for
  `print()` / `str()` / `stringify()`, while the interpreter serialized it as
  JSON. The emitter now builds a `LumeMap` from the struct's declared fields
  — the `Type` already carries their names and types, so no runtime
  reflection is needed — and serializes that map through the existing writer.
  `print()` routes to `lume_map_print`; `str()` / `stringify()` route to
  `lume_rt_to_json_obj`. Each field is read by GEP+load and stored with the
  same `lume_map_put_*` helpers a map literal uses; a nested named struct is
  converted recursively. Output matches the interpreter byte-for-byte
  (`native-consistency.lume` and `interp2.lume` pin it).
- Known sibling gap, **out of scope here**: constructing a struct literal
  whose *field* is itself a named struct is still refused by the native
  emitters (`cg_struct_lit` cannot yet build a struct value holding a nested
  struct). Once that lands, the serializer above already handles it. The same
  applies to `o.inner` style two-level member access on a struct value, which
  the native backends do not type yet.

### Fixed — bools inside containers in the native backends

- A container bool used to be stored with the int slot tag, so print()/str()
  rendered it as `1`/`0` where the interpreter prints `true`/`false`. The
  native runtime now has a dedicated `LUME_SLOT_BOOL` tag with
  `lume_list_push_b` / `lume_map_put_b`, and both renderers (print and JSON)
  plus the structural-equality slot walker handle it. Literals, `push`/`put`
  builtins and `map`/`filter` with bool results are all three-backend
  consistent now, pinned by the bool-container probe block in
  `tests/native-consistency.lume` (which also revives the bool-valued
  structural-equality probes from the eq round — `.expected` 259 → 267).
- The container-helper dispatch tables (`list_push_fn` / `map_put_fn` /
  `map_get_fn`) are dual copies like `infer_node_type`; they are now covered
  by `tests/case-parity.sh` as well (5 label-set comparisons, negative
  control verified).

### Added — `json()` / `stringify()` serialization in the native backends

- The two native emitters (`--compile-text`, `--compile-llvm`) now support
  `stringify(x)` for every representable type and `json(x)` for serialization
  (non-string argument): scalars via the existing `lume_bi_str_*` flavours,
  a bare string via the new `lume_rt_quote_str` (quotes + escapes, matching the
  interpreter's `json_append_value`), containers via `lume_rt_to_json_obj`.
  `let`-bound results type as `string` in both emitters' `infer_node_type`.
- `jb_escape` now emits `\b`/`\f` short escapes like the interpreter does
  (previously `\u0008`/`\u000c`), so `str()`/`stringify()` over containers
  agree byte-for-byte on those too.
- **Boundary kept honest**: `json(str)` — the parse direction — stays
  interpreter-only. The parse result's type is dynamic and the statically
  typed emitters cannot spell it; both backends now reject it at emit time
  with a clear diagnostic instead of "unknown function". Documented in
  SPEC §6 and pinned by the json/stringify probe block in
  `tests/native-consistency.lume`; interpreter-side round-trip assertions in
  `tests/interp2.lume` (now 54 checks).

### Added — string interpolation `s!"..."` and structure update `{ base with ... }` (SPEC §3.8 / §3.9)

- **String interpolation `s!"..."`** — `s!"text {expr} text"` splices an
  expression into a string; `{` enters expression mode (bracket-depth tracked,
  `}` inside a string literal does not count), so nested interpolation, indexing,
  calls and even `with` updates may appear in a segment. The segment is converted
  with `str()` and concatenated with the literal parts; `{{`/`}}` escape to a
  literal brace. Three-backend consistent (interpreter / `--compile-text` /
  `--compile-llvm`), pinned by the interpolation probe in
  `tests/native-consistency.lume`.
- **Structure update `{ base with f: e, ... }`** — shallow-copies `base` (a map
  or an annotated named struct) and overrides the listed fields, producing a new
  value (the base is untouched). Map bases yield an anonymous map read back with
  `get()`; annotated named-struct bases yield the same struct read back with
  `.f`. Rejected shapes: non-map/struct base (`'with' update requires a map or
  struct base`), unknown field (`unknown field 'f' in struct update (T)`),
  duplicate field (`duplicate field 'f' in with update`), empty update
  (`with update needs at least one field`) and field type mismatch
  (`struct field: cannot use a value of type ...`). Three-backend consistent,
  pinned by the update probe in `tests/native-consistency.lume`; interpreter-only
  assertions (incl. `.f` dynamic read on an unannotated map base) live in
  `tests/interp2.lume` (+ 8 negative cases in `tests/interp2-bad/`).

### Fixed — `str()` over containers in the native backends (2026-10-13)

- **`str(map)` / `str(list)` now work in `--compile-text` and `--compile-llvm`**,
  producing the same JSON the interpreter renders (`{"a":1,"b":2}`, `[1,2,3]`,
  nested containers and escaped strings included). The SPEC §6 table already
  promised `map/list→JSON`; only the interpreter delivered it — both native
  emitters rejected the call with `str() cannot convert this value`, which also
  made `s!"m={m}"` unusable in compiled binaries. Backed by a new runtime
  helper `lume_rt_to_json_obj` (rt.c) that serializes a container into a
  malloc'd JSON string with a private growable buffer (nested strings cannot
  clobber earlier output the way `slot_emit`'s shared static buffer can).
- **Scope boundary**: a *named* struct is a raw GEP struct with no runtime
  header natively, so `str()`/`print()` on one stays native-rejected
  (`str() cannot convert a named struct value`) — the interpreter still
  renders it. Also kept out of the consistency fixture: bool values inside
  containers (the native runtime has no bool slot tag and renders them as `1`,
  `print()` included) and null map values (native emitters reject them at
  compile time). All three boundaries are documented in the
  `tests/native-consistency.lume` header and pinned by its new str-container
  probe block.

### Changed — structural `==` / `!=` (SPEC §1.8)

- **`==` / `!=` on containers are structural comparisons** — never pointer
  luck and never a flat `false`. Before this the three backends each answered
  differently: the interpreter's `value_eq` compared map/list pointers (so
  `[1,2] == [1,2]` was `false`), and both native emitters sent an `icmp` on
  two `i8*`s (pointer identity — and invalid IR for mixed-type operands).
  Semantics, now uniform across all three backends:
  - `list`: same length + element-wise recursion (order sensitive);
  - `map`: same key count + every key present in the other with equal
    values, **key order independent**;
  - `enum`: an ordinary map whose `"$"` tag is just another key — tag and
    payload compare together;
  - `struct`: structure only, **not the type name** — same field set means
    field-by-field;
  - `func`: pointer identity; `null` against an i8\*-lowered value: null
    pointer identity.
- **The type checker now gates equality classes** (it previously checked
  nothing for `==`/`!=`). Cross-class comparisons are rejected at compile
  time with `'==' does not apply to X and Y` (`1 == "x"`, `true == 1`,
  `[1] == 1`, `1 == null`, ...). `null` compares only against i8\*-lowered
  kinds (string / containers / func / any / null) — an `int`, `float` or
  `bool` is never null, which the native backends already enforced by
  refusing to build null scalars. `any` stays the wildcard class.
- **Native lowering**: `string × string` → `lume_str_eq` (byte compare);
  container × container → `lume_value_eq` (rt.c walks the slots — the
  interpreter's `values_equal` mirrored); `any` against an i8\*-lowered
  value → `lume_any_eq` (runtime kind probe: two containers structurally, a
  container vs a non-container never equal, otherwise bytes). A **named
  struct is the one shape that never reaches rt.c** — it lowers to a typed
  aggregate `%P = type {...}`, not the i8\* heap map — so two named structs
  compare field by field (GEP + load, AND over fields, recursive through
  `cg_val_eq`/`cg_struct_eq`, mirrored in both emitters); a field-set
  mismatch is the constant answer `false` (`true` for `!=`), matching the
  interpreter's key-set rule. A named struct against a map is refused by
  the native backends (no runtime bridge between the two representations).
- **Parity fix found on the way**: `bool == bool` was `icmp eq i64` on two
  i1 values in the IR-text backend — clang rejected the module outright
  while the libLLVM backend accepted it. Both now compare as i1.
- **Diagnostic fix (pre-existing)**: `ty_str` spelled every type into one
  static buffer that the *next* call cleared — even one answering from a
  string literal — so any diagnostic naming two types printed an empty
  name for the container side (`'' is not assignable to the declared type
  'int'`). It now uses a ring of buffers. `ty_str` also gained the missing
  `list` / `map` / `func` spellings (`int[]` when the element type is
  known), which the same class of message had been rendering as `<?>`.
- Regression suite: `tests/eq2.lume` (43 assertions) + `tests/eq2-bad/`
  (7 rejection cases with pinned diagnostics), wired into `make test` via
  `eq2-test`; an equality probe section was added to
  `tests/native-consistency.lume` (list / order-free map / struct GEP /
  enum vs map literal / null identity) and the `.expected` regenerated.
  SPEC §1.8 documents the full rule table; §3.7's "enum values do not
  participate in `==`" stance is retired.

### Added — or-patterns, match guards and `if let` (Lean borrowings)

- **Or-patterns**: `match (c) { Red | Green | Blue => "warm", ... }` — several
  variants share one arm. Coverage unions the alternatives' variant sets; with
  payloads every alternative must bind the **same names with the same types**
  (stricter than arm-body unification: no int/float widening — write two arms
  for that). Duplicate variants inside one or-list are rejected.
- **Match guards**: `Circle(r) if r > 1.0 => ...`. Evaluated after the binds,
  before the body; a false guard falls through to the next arm. A guarded arm
  does **not** count toward exhaustiveness (same rule as Lean), and duplicate-arm
  detection only considers unconditional arms. A guarded last arm falling off a
  forged map traps (`unreachable` natively, `no match arm` in the interpreter).
- **`if let P [| P] = e { ... } else { ... }`** — a statement (not an
  expression) with single-arm match semantics and no exhaustiveness
  requirement. Binds are block-scoped (then-branch only); or-patterns allowed,
  guards not; the scrutinee must be an enum value. Assigning an outer local
  from inside the then-block updates the outer variable on all three backends.
- Interpreter fix found on the way: assignment inside a match arm / if-let
  body used to shadow the outer variable instead of updating it (the arm runs
  in a child env and `env_set` never walked the chain). `N_ASSIGN` now reuses
  the nearest visible binding **within the current function/module frame** —
  the walk stops at a new `Env.is_boundary` flag, so "assigning a top-level
  name inside a function defines a local" and "a module assigning a
  global/builtin name stays module-local" both keep working.
- Regression suite: `tests/match2.lume` (20 checks) + `tests/match2-bad/`
  (9 rejection cases with pinned diagnostics), wired into `make test` via
  `match2-test`; match probes extended in `tests/native-consistency.lume`.
  SPEC §3.7 documents the semantics.

### Tests — three-way coverage of `match` as an expression

- `match` already lowers to a value in all three backends: the native
  emitters allocate a result slot, each arm stores through `coerce`, and the
  join loads it (`cg_match` in `codegen_match.c` and `llvm_cg_expr.inc`
  return a `Val`). It was only exercised as a `return` value in
  `tests/match2.lume` and `tests/native-consistency.lume`, so the bare
  expression contexts were unguarded. `tests/native-consistency.lume` now also
  pins `let r = match {..}`, `print(match {..})`, arithmetic on the result,
  and a nested `match`, with the interpreter's stdout regenerated
  (`.expected` 313 → 318). No emitter change was needed — the feature was
  already consistent across backends; this only adds the cross-backend
  assertion.

### Added — collection + string-search builtins

- `sort(list)` — sort a list **in place** and return it. Homogeneous lists
  only: all numbers (ascending; all-int compares exactly as i64, any float
  falls back to double) or all strings (byte/lexicographic via `strcmp`).
  Mixed-kind or container-holding lists raise a VM error.
- `str_match(pattern, text)` / `regex(pattern, text)` — POSIX extended-regex
  findall. Returns a list of match records, each
  `{ text: matched substring, index: 0-based offset, groups: [capture groups] }`.
  No match → `[]`; invalid pattern → VM error (catchable with `try()`).
- These join `json()` as interpreter-path builtins (the compiled native
  backends do not yet lower them), closing the parse → extract → sort loop
  for JSON/CLI tooling. Regression suite: `tests/builtins-sort-regex.lume`
  (17 checks, wired into `make check` / `make test`).

### Added — self-contained SQLite builtins and two-way `json()`

- `sql_query` / `sql_write` now live in the standalone tree (`src/builtins_sql.c`),
  fully self-contained: no db_layer, no agent-httpd. Guardrails are host-identical —
  single statement, SELECT-only reads (opened READONLY), INSERT/UPDATE/DELETE-with-WHERE
  /CREATE TABLE writes, `?` placeholders bound as text (values never enter the SQL
  string), rows capped at 200, empty or relative DSN falls back to env `SQLITE_DB`.
- libsqlite3 is probed with a compile test at build time; without it the build still
  succeeds and the builtins fail clearly at runtime (same pattern as `http_*`
  without `LUME_HAS_HTTP`).
- `json()` now also serializes non-string values (the `stringify()` direction);
  strings keep parsing, and `json(map)` / `stringify(map)` produce identical output.
- Tests: probe-gated `sql-test` (SKIPs on stub builds) and `json()` direction
  assertions in `builtins-test`, both wired into `make test`.

### Added — serve: docroot static files and `:name` route segments

- `server { docroot = "..." }` serves route-miss GETs from disk: URL-decoded
  (encoded `%2e%2e` traversal attempts rejected), `index.html` for directory-shaped
  paths, extension MIME table, 64 MB size cap.
- Dynamic route patterns (`"/api/user/:id"`) capture segments into `req.params` as
  a map; literal matches still win, patterns are tried in registration order, and
  `req.params` is always present (empty map on literal routes).
- Response float serialization moved from `%.17g` to `%g`, matching `value.c`'s
  `json_append_value` (1234.56, not 1234.5599999999999).
- Tests: `docroot-test` (curl probes including traversal attempts and a dynamic
  route) wired into `make test`.

### Added — NO_PROXY bypass for outbound HTTP

- Outbound `http_get/post/put/patch/delete` now honor `NO_PROXY` / `no_proxy`
  (comma/space separated; supports `.domain` suffix matching, `*` wildcard,
  optional `:port`). Hosts listed there connect **directly, bypassing the
  proxy**. `LUME_NO_PROXY` takes highest precedence and wins even when
  `LUME_HTTP_PROXY` is set — intranet hosts must be able to reach directly.
- Regression suite: `tests/builtins-http-noproxy.lume` + `tests/http_noproxy_srv.py`
  (dead-proxy control on `:9` + real server on `:18891`), wired into
  `make test` via `http-noproxy-test`.

### Fixed — accurate error for unannotated scalar `m["k"]` on native backends

- A map value read of an **unannotated variable map** whose value is a scalar
  (int / float / bool) lowers to the `i8*` pointer accessor, which can only
  carry string / container values. It used to fail with the misleading
  `no such key in this map` — the key is present, the value is simply a scalar
  the pointer accessor cannot return. `src/rt.c` now tags the failure reason
  (`g_index_fail_kind`) and `lume_index_error` reports:
  `map value at this key is a scalar; bind m["k"] to a typed variable
  (let v: int = m["k"]) or use get(m, k)`.
- The interpreter is unaffected (it carries runtime types and prints the
  scalar directly); the compliant native forms — `get(m, k)`, a typed binding
  `let v: int = m["k"]`, or a value-type-resolvable literal map — all work.
- Regression suite: `tests/map-index-scalar.lume` (compliant paths, three
  backends agree) + `tests/map-index-scalar-bad.lume` (asserts the accurate
  message on both native backends), wired into `make test` via `map-index-test`.

### Fixed — `install.sh` crash when the release tarball shipped no examples

- The one-line installer no longer fails when the tarball it unpacks carries no
  `examples/` directory.

### Fixed — Windows build / CI follow-ups

- MSYS2 environment migrated MINGW64 → UCRT64 (checkout action bumped to v5);
  parse-time `$(shell cmd /c …)` calls replaced with idempotent recipes and the
  `build/bin` directories created via parse-time `mkdir -p`, so a cold Windows
  tree builds cleanly.

## [0.5.1] - 2026-10-09

### Added — standalone install + version reporting

- Standalone installer `install.sh`: one-line `curl …/install.sh | sh`
  drops the binary in `~/.local/bin/lume-core` and carries `examples/` +
  `docs/` to `~/.local/share/lume-core`. Supports `LUME_CORE_VERSION` /
  `LUME_CORE_PREFIX` / `LUME_CORE_SHA256` / `LUME_CORE_REPO` overrides.
- `.github/workflows/release.yml`: push a `v*` tag to build on
  linux-x64 / linux-arm64 / darwin-arm64 (+ darwin-x64 cross) and attach
  the per-platform tarballs to the GitHub Release; `ci.yml` gained a
  `release-build` guard job.
- `lume-core --version` / `-V` prints the build version; the previously
  hardcoded `0.5.0` in the LSP/MCP `serverInfo` handshake now reads the
  same `LUME_CORE_VERSION` macro (single source of truth).

## [0.5.0] - 2026-10-08

### Added — string tools (codegen-oriented)

- New string builtins in `builtins_str.c`: `upper(s)`, `lower(s)`,
  `capitalize(s)` (ASCII; non-ASCII bytes pass through unchanged),
  `trim(s)` (ASCII whitespace), `contains(s, sub)` (empty sub → true),
  `split(s, sep)` → list (empty sep → `[s]`), `join(list, sep)`
  (16 MiB cap, elements stringified via `str_of_value`), and
  `substr(s, start, len?)` (byte-based; negative start counts from the
  end, len clamps). These cover the string-transform needs of
  code-generation tooling (e.g. `capitalize(controller)` in CRUD
  generators).

### Added — LSP language server (`--lsp`)

- New `--lsp` mode: a stdio Language Server Protocol server
  (Content-Length framing) with the initialize/shutdown/exit lifecycle,
  `didOpen`/`didChange` → parse + typecheck → `publishDiagnostics`
  (the `line N:` error prefix maps onto 0-based LSP lines; no columns
  yet), builtin hover notes and completion over the builtin table.
  Editor wiring: point a client at `lume-core --lsp` (SPEC §12).
- The builtin name table is exported (`LUME_BUILTIN_NAMES` / count) and
  `json_esc` moved into lume.h, shared with bridge_mcp.c.

### Added — MCP tools/call argument validation

- `tools/call` now rejects arguments whose type does not match the tool's
  declared schema (`{"k":"string",...}`): a mismatch returns -32602 with
  the offending field name. Missing keys stay fine (handlers fall back via
  `get(params, k, def)`).
- JSON integers (no `.`, `e`, `E`) now decode as `int` instead of `float`
  (SPEC §6.1), so an `int` schema actually matches a JSON integer and
  64-bit precision is preserved.
- `examples/`: `mcp_fs.lume` (fs_read / fs_list / fs_write) and
  `mcp_math.lume` (math_add / math_fact / math_stats) show scripts
  exposing builtins as MCP tools with zero C-side changes.

### Added — serve() cookie sessions

- Every HTTP request now gets a session: `req.session` is a map shared
  across requests for the same `lume_sid` cookie, and `req.session_id` names
  it. A fresh session is handed to the browser via
  `Set-Cookie: lume_sid=<id>; Path=/; HttpOnly; SameSite=Lax` (no Set-Cookie
  when an existing session is resumed). The session table is VM-owned and
  GC-rooted, so session data lives for the process lifetime (cleared on
  restart). Sessions are created eagerly per request; a future
  `server { sessions = off }` can disable them.

### Added — MCP stdio server (`--mcp`)

- `lume-core --mcp <script.lume>` runs the script (so `tool "name", "desc",
  {schema}, handler;` statements register their handlers), then serves the
  Model Context Protocol over stdio: newline-delimited JSON-RPC 2.0 on
  stdin/stdout. Methods: `initialize` (capabilities), `tools/list`, and
  `tools/call` (runs the handler with one decoded-arguments map; string
  results return as plain text, other values as JSON). JSON-RPC errors use
  the spec codes (-32700/-32600/-32601/-32602); notifications get no reply.
- Windows: stdin/stdout are switched to binary mode so \n framing is exact.
- `tools/list` normalises the shorthand params map (`{"k":"string",...}`)
  into a standard JSON Schema object (`{"type":"object","properties":...}`),
  so mainstream MCP clients (e.g. Claude Desktop) can validate arguments.
  All properties are optional; handlers fall back via `get(params, k, def)`.
- Startup/shutdown diagnostics go to stderr (`lume-mcp: N tool(s)
  registered...` / `stdin EOF, exiting`) — stdout stays a clean protocol
  stream for the client.

### Fixed — write_file() replacing an existing file on Windows

- MSVCRT `rename()` refuses to overwrite, so `write_file()` to an existing
  path silently failed to update it (e.g. regenerating hello-gen output).
  The old file is now removed before the rename, restoring the
  atomic-replace contract on Windows.

### Added — default security headers on serve() responses

- Every HTTP response now carries `X-Content-Type-Options: nosniff`,
  `X-Frame-Options: DENY`, `Referrer-Policy: no-referrer` and
  `Content-Security-Policy: default-src 'self'` by default. Hardcoded for
  now; a `server { csp = ... }` option is the planned escape hatch for apps
  that need inline scripts or external resources.

### Added — CLI script arguments (`argv()`)

- `lume-core script.lume a b` now collects everything after the script name
  into `argv()` — a `string[]` (empty when the run carried none). Previously
  extra non-flag arguments were silently swallowed; the **last** one even
  overwrote the script path. The first non-flag argument is the script now,
  and the rest are script arguments.
- Interpreter only; both native backends report `argv` as not supported yet
  (SPEC §6.5).

### Added — recursive `files(dir, 1)`

- A second truthy argument walks the tree depth-first: entries become
  relative paths (`sub/file.txt`, `sub/dir/`) ready for `read_file()`, with
  directories still carrying the trailing `/`. Single-argument `files(dir)`
  is unchanged (SPEC §6.4).
- Depth capped at 64 and each directory's own entry count at 1024 (the
  single-level listing's cap), so a hostile deep tree cannot blow the stack
  or the list.

### Fixed — `json()` documentation (SPEC §6.1)

- The table said `json(x)` was serialization-only; the builtin has always
  been the reverse — `json()` parses **JSON text into a value**, while
  `stringify()` is the serializer. Both rows now document the real
  directions, including the parse-failure VM error catchable with `try()`.

## [0.4.0] - 2026-10-08

### Added — index syntax `m["k"]` / `l[0]` (规范变更)

- `m["k"]` and `l[0]` now **exist** — SPEC §6.2 replaces the line that said they
  do not. Subscripts live in the postfix loop, so they **chain** (`m["a"][0]`,
  `rows[i]["name"]`), and the subscript is a **full expression**, not just a
  literal — `m[k]` with a variable is the shape worth having.
- ⚠️ **Strict** — that is the only difference from `get()` / `el()` and the
  reason the syntax exists: a missing key or an out-of-range index is an
  **error**, where `get()` silently answers `null` and `el()` answers its
  default. **Negative indices count from the end**: `l[-1]` is the last item.
- Interpreter support landed first (parser, type checker, `src/rt.c`'s strict
  `lume_*_at_strict*` accessors), then **both native backends** (§6.2): `N_INDEX`
  lowers through unified `lume_index_int/float/ptr` runtime entry points, with
  the container kind field dispatching list vs map at runtime (needed for
  opaque chains like `m["p"][1]`). List indexing is fully typed and prints
  directly; map reads return an opaque `i8*`, so the portable native shape is
  binding to a typed variable (`let c: string = m[k]`) — a direct print of an
  opaque result is guarded to print `(opaque value)` rather than a raw pointer.

### Added — native containers nest

- `{ a: { b: 1 } }` and `[[1, 2], [3]]` used to be refused by both emitters
  ("map values must be int, float or string") while the interpreter had always
  accepted them. It was not a missing feature so much as a missing
  representation: every `LumeSlot` held a scalar. A new `LUME_SLOT_OBJ` tag
  carries a nested container's address in the slot's `num` field
  (`lume_list_push_obj` / `lume_map_put_obj`), and both container structs grew
  a `kind` (`LUME_CTR_LIST` / `LUME_CTR_MAP`) at the same offset, so the
  printer reads one field to know what it is holding.
- Printing stopped going through a buffer: `slot_emit` now writes straight out
  and recurses (`container_body()` is the single place elements are walked, so
  the three paths cannot drift). That surfaced a latent bug fixed along the
  way — the string branch wrote the closing quote but no NUL, so a short
  string rendered after a longer one into the same static buffer printed the
  old one's tail. **Maps in maps, lists in lists — four levels deep — now
  agree across all three backends** (regression in
  `tests/native-consistency.lume`, verified by mutation).
- Remaining refusal, unchanged and predating this work: `get()` with no
  default still cannot return a container (its flavour follows the default's
  type and the checker does not type `get()`'s result).

### Added — an in-tree HTTP server, on every path

- `run()` in the standalone interpreter no longer refuses: `bridge_run()` serves
  `vm->routes` through a minimal single-threaded HTTP/1.1 server on POSIX
  sockets. `server{}` config carries host/port; a handler returning a string
  answers `text/html`, a map answers JSON, `null` answers 204, an unmatched
  path answers 404, a handler error answers 500. `req` carries
  `method` / `path` / `query` / `query_params` (URL-decoded) / `body` / `label`.
- The `--compile-text` path gets the same thing **VM-free**: `bridge_native.c`
  (`lume_srv_init` / route / run, query-param lookup, JSON accumulation) is
  compiled per build like `rt.o` and linked into the binary; `server{}` lowers
  to init and route handlers compile as FFI functions reading `req` through
  `SrvReq` fields.
- **Windows**: the winsock port covers both the native and the interpreter
  servers — `winsock2`/`ws2tcpip` glue (`SOCKET` vs `int`, `closesocket`,
  `WSAStartup`), `localtime_s`/`strtok_s` shims, and a `TokenType` shim around
  `windows.h` (winnt.h's enum member clash). `backend.c` gained `LUME_TARGET`
  cross-compilation (`LUME_TARGET=x86_64-windows-gnu` produces a working
  PE32+ exe via zig cc; cross objects go to `build/*_win.o` so a windows COFF
  never poisons the native link), and the mingw branch links `-lws2_32`.
- The route-handler buffer is freed in `cg_free` (not only when it has
  content) — a leak found on the way.

### Build & platform

- `build_core.bat` (Windows build helper) reads its toolchain paths from a
  local `.env` (`LUME_RT_SRC` / `MINGW_BIN`) instead of hardcoding them; the
  file and the build log are gitignored.
- CI: the Windows native-text leg and an asan leak are fixed.

### Added — no-capture closures as first-class values on all three backends (§4.8)

- **无捕获 lambda 三后端通用**：`let f = (x) => x * 2` 在解释器、`--compile-text`、
  `--compile-llvm` 下行为逐字一致——装箱、存变量、调用。§4.8 原「闭包只在解释器可用」
  的口径作废；自由变量捕获与 `try` 仍是解释器独占。
- 运行时新增 `LumeClosure { fn, cap }` 与 `lume_closure_make`（`src/rt.c`），闭包以不透明
  `i8*` 穿过两个发射器。
- 共享签名推断（`codegen_sig.c`）为每个 lambda 定参定返并指派 mangled 名，两个
  `infer_node_type` 副本据此把 `N_FUNC_LIT` 推成带 AST 回指针的 `TY_FUNC`。
- 文本后端（`codegen_*.c`）与 libLLVM 后端（`llvm_codegen.c`）各实现两阶段发射：
  字面量处装箱，lambda 体以 `define @__lume_clo_N(i8* %cap, ...)` 追加在模块尾；
  闭包调用拆记录取 fn/cap 后带 cap 调用。
- **`map` / `filter` / `reduce` 成为原生 HOF**：两个发射器把循环内联展开（迭代长度只
  快照一次，循环体内的 `push` 不延长迭代），filter 有 push/skip 分块，reduce 用累加器
  槽。一阶段拒绝项：自由变量捕获、`try`、filter 返回非 bool/int/float、struct 累加器、
  容器元素列表。
- `tests/native-consistency.lume` 新增闭包 + HOF 差分段（9 行输出），`.expected` 刷新；
  `docs/SPEC.md` §4.8 重写，`docs/LUME.md` 的过时口径同步更正。
- 排查过程修掉两个发射器级缺陷：`Sig.is_lambda` 未在 `sig_push` 清零（realloc 尾巴的
  垃圾把具名函数错走 lambda 回写分支，经 union 偏移踩坏 `param_types`，表现为
  `native-fact.lume` 的 UAF）；libLLVM reduce 把 `Val[]` 误作 `LLVMValueRef[]` 传参
  （第二实参拿到 `Type*`，验证器打印该 call 时段错误）。

### Fixed — top-level bindings visible inside function bodies (SPEC 8.1 #10)

- 每个顶层 `let` / 顶层 for-in 变量提升为一个模块级槽（文本后端 `@lv_<name>` 全局、
  libLLVM `LLVMAddGlobal`），顶层语句与函数体读写同一份存储，两后端镜像同步。语义逐点
  对齐解释器：函数调用时读到顶层最后写入的值（非定义时快照）；函数体内对顶层名赋值新建
  函数级局部（不穿透写全局，赋值行之前的读仍解析到全局——沿 §8.1 #12 的行号规则）；
  参数遮蔽顶层名。
- 驱动在签名推断后新增 pass 1.7 预扫描顶层语句，L_top 复用该预扫描表（不再二次扫描）；
  函数体名字解析顺序 = 位置感知 locals → 顶层表兜底。
- `native-consistency.lume` 新增顶层绑定差分段（调用时读、顶层改值后调用、函数内赋值
  不穿透、参数遮蔽），`.expected` 刷新；SPEC §8.1 #10 开放 → 已修。
- **边缘形态排查后的签名 pass 加固**：① lambda 预注册 pre-pass——`f = (x) => ...` 赋值
  右侧、lambda 体调用另一 lambda 等从未被调用点触及的 lambda 原先到达发射器时没有
  mangled 名，报 `internal:` 级消息；现在推断开始前全 AST 预注册（同时消除 mid-round
  `sig_push` realloc 悬垂 `Sig*` 的隐患）；② 函数体作用域并入顶层 `let` 类型（每 body
  拷贝一份 globals），顶层闭包变量在 lambda 体内可解析；③ 尾部校验消息对 lambda 说
  「the lambda」而非内部名。
- **同轮定界的开放缺口（SPEC §8.1 #9–#12，均有干净拒绝或记录）**：闭包变量重赋值在原生
  路径显式拒绝（运行期盒子与类型回指针的签名可能错配，防静默错签名调用）；顶层/外层绑定
  在函数体内不可见（预先存在，含标量）；空列表字面量直接进 HOF 拒绝（标注变量替代写法
  三后端一致）；locals 表类型查找位置盲（§8.1 #4 的尾巴，后绑定同名可让先前 for-in 拿错
  元素类型）。
- **§8.1 #12 已修（2026-10-08）**：`Asg` 记录声明行号，`asg_find` 位置感知（使用点只见
  不晚于自己行号的声明，取最新一条），与解释器的 set-or-define 重绑语义逐点对齐；
  两后端镜像同步。回归用例：`native-consistency.lume` 的 `sz()`（sibling 块同名异型，
  旧实现首块读到未初始化槽）与 `w2`（for-in 重绑穿透，循环后保持重绑值）。

### Fixed — two §8.1 native-backend consistency gaps (#1 / #2)

- **#1 `float %` 的报错位置**：`--compile-llvm` 原在下游 `cg_print` 处把
  `cg_binary` 先发出的 `'%' does not apply to floats` 覆盖成
  `print() cannot print this value`。`llvm_codegen.c` 的错误宏改为「首错优先」
  （`g->err` 已有内容时不覆盖），根因错误不再被吞；同时 `infer_node_type`
  新增 `N_BINARY` 分支，在类型推断阶段就把 `float %` 直接判为非法，与
  `--compile-text` 及解释器一致。
- **#2 `let x = -7` 在 libLLVM 下不可打印**：预扫描 `infer_node_type` 当时没有
  `N_UNARY` / `N_BINARY` 分支，导致 `let neg = -7` / `let d = 0 - 7` 推不出类型
  （`print() cannot print this value`）。现新增这两类分支（`not`→bool，负号→操作数
  类型；逻辑/比较→bool，算术按浮点/整型推算，`string+string`→string），三后端一致。
  回归用例见 `tests/native-consistency.lume` 的 `neg` / `diff`。
- `tests/native-consistency.expected` 刷新（新增 `-7` / `-7` 两行）。
- §8.1 #3 was **verified already consistent**: its legality check already
  lives in the typechecker (`src/typecheck_expr.c:297/299/303`), and all three
  backends emit the same clear message — the SPEC's "lands in the parser" claim
  was stale, so no code change was needed for it.
- Investigating #3 surfaced a **new, real gap (§8.1 #6)**: a *legal* `call?`
  (error propagation) compiles and runs only in the interpreter; both native
  backends fail to codegen it (`variable 'q' has no codegen type`) because the
  `?` payload type is `any_type()` at check time and there is no native `Result`
  type. Deferred as a large feature (needs runtime-value boxing, like closures).
- **Fixed the native backends rejecting `null` literals** (§8.1 #7): both
  emitters' `infer_node_type` now handles `LIT_NULL` (→ `TY_NULL`); `TY_NULL`
  lowers to an opaque `i8*` null pointer (`llvm_type_of` / `ty_of`); `cg_literal`
  emits `null` / `LLVMConstNull(i8*)`; and a new `lume_print_null` runtime helper
  prints `null`. All three backends now agree on `null` and `print(null)`
  (regression added to `tests/native-consistency.lume`, `.expected` refreshed).
- **Fixed `null` with a scalar annotation silently becoming a wrong value**
  (§8.1 #8) — the worst shape a backend divergence can take, because it is
  silent. `let x: int = null; print(x)` prints `null` under the interpreter,
  **failed to compile** under `--compile-text`, and printed **`0`** under
  `--compile-llvm`, whose `coerce` had an `i8* → i64` `ptrtoint` edge that
  turned the opaque null pointer into an integer. The type checker is right to
  accept it (`type_compat`: "all types nullable" — the language has no
  nullable-type distinction); the emitters disagreed about what to do with it.
  With a `string` annotation both emitters compiled and emitted
  `call i64 @lume_print_str(ptr null)` — a null pointer into `snprintf`'s
  `%s`, which is undefined behaviour (glibc happened to print `(null)`).
  Both `coerce` implementations now reject `TY_NULL` flowing into a scalar,
  with one shared wording (`cannot use 'null' as a int value in the native
  backend`; the type name comes from a new shared `src_type_name()` helper,
  since `ty_str()` lives behind `typecheck_internal.h` and an emitter has no
  business depending on the checker). Pointer-shaped targets stay legal, and
  `rt.c`'s `lume_print_str` now guards a null pointer and prints the bare word
  `null`, matching the interpreter.
- **Hardened a pre-existing weakness of the same shape**: a failing `coerce`
  returns an empty `Val`, but 4 of its 6 call sites used it without checking —
  libLLVM would `LLVMBuildStore(NULL)` and segfault (ASan pinned it in
  `CreateAlignedStore`). Every call site now bails on `g->err[0]` first.
- Regression assertions live in `tests/native_backends.sh` as `null-as-scalar`
  (every emitter must refuse, with identical wording) and `null-as-string`
  (every leg must print `null`). They cannot live in
  `native-consistency.lume`: that fixture only diffs programs all three legs
  *accept*, and what needs asserting here is precisely that the program does
  not run. Verified by mutation — removing the guard makes the suite fail with
  `null-as-scalar: llvm compiled 'let x: int = null'`.
- Docs: dropped the now-stale "known divergence" note on `float %` in §1.6
  (fixed by #1) and recorded #8 in §8.1.
- **Implemented `?` error propagation in both native backends** (SPEC 8.1 #6,
  second half). The recorded diagnosis said the payload type is `any_type()`
  at check time and that fixing it needed generics or runtime-value boxing.
  Neither turned out to be necessary: the payload is knowable statically from
  the callee's own `return { ok: X }` statements.

  The type checker now walks function bodies twice. The first walk records each
  Result annotation's `ok` payload onto that function's *own* Result type object
  (`record_ok_payload` writes `Type.elem`); the second walk reports and lets a
  call site read `callee_t->ret->elem`. Two walks are required, not tidy: in
  `later()?` where `later` is defined further down the file, a single walk sees
  an empty payload. During the collecting walk `ck_fail` only records instead of
  stopping -- a diagnostic must not cut the walk short, since the rest of that
  body may hold the very `return { ok: X }` a `?` elsewhere needs -- and the
  collecting walk gets a top-level scope of its own, or the second walk would
  see the first walk's `let` bindings as duplicate declarations.

  Two `return { ok: X }` in one function whose X disagree is now an error
  rather than first-wins or a silent widening: the function has no single
  payload type, and picking either one mistypes whichever call site read the
  other.

  Both emitters lower `?` to the same thing the interpreter does (interp.c's
  propagate branch): call, test `lume_map_has(res, "err")`, and on a hit return
  that Result unchanged -- `?` moves the failure out, it does not rewrite it --
  otherwise read `ok` through the accessor the payload type picks.

  Three things had to line up that all read as one bug:
    - codegen_scan.c's and llvm_codegen.c's `infer_node_type` both return the
      callee's *declared* return type for an N_CALL, so `let v = f()?` alloca'd
      a pointer for an int payload. The text backend then emitted
      `store i64 %c10, i64* %lv_v` into an `i8*` slot (clang assembles it, the
      program segfaults); libLLVM instead printed the integer through
      lume_map_print. Both infer_node_type copies need the `propagate` case.
    - `cg_string_val` returns a name in a caller-owned buffer, not heap memory.
      Freeing it aborts with "pointer being freed was not allocated".
    - the text backend needs a real basic-block branch, the libLLVM one needs
      three blocks with the payload parked in an alloca (the shape its
      short-circuit and/or lowering already uses).

  native_backends.sh's `propagate` assertion is flipped rather than deleted: it
  used to require both emitters to *refuse* `f()?`, which was the only honest
  answer while `?` was unimplemented. It now requires every leg to run it and
  match the interpreter, on both the ok and the err path, since those fail
  differently (the ok path used to print the whole Result, the err path used to
  run the statements the error was supposed to skip). Verified by mutation:
  dropping the payload-type case fails it with "propagate: text disagrees with
  the interpreter".

  Still refused by both emitters, with the reason stated: a callee that only
  ever returns `err` has no `return { ok: X }` to read a payload type from. The
  interpreter runs it fine.

- **Gave `Result` a native representation** (§8.1 #6, partially fixed). The
  gap's recorded diagnosis was wrong: it blamed the `?` propagation suffix and
  called the fix "a large runtime-value-boxing job". Measuring it showed the
  opposite — **`Result` had no codegen type at all**. `llvm_type_of()` and
  `ty_of()` had no `TY_RESULT` branch, so both returned NULL and even
  `let r = divide(21, 7);` with no `?` anywhere failed with `variable 'r' has
  no codegen type`. The "boxing" framing came from reading a compile failure as
  a missing representation.
  No new representation was needed either: `Result` is `{ ok: ... }` /
  `{ err: ... }` and the interpreter already treats it as an ordinary map
  (`interp.c`'s `propagate` branch looks both keys up with `map_get`), so
  natively it is the same heap map. Both backends map `TY_RESULT → i8*` (the
  same shape as an anonymous map; `LumeSlot` is a tagged union, so one map
  holds an int `ok` and a string `err` side by side) and route `print` through
  the existing `lume_map_print`. Round-tripping `{ok:int}` / `{err:string}`
  now agrees across all three backends (regression added to
  `tests/native-consistency.lume`, `.expected` refreshed).
- **Fixed a pre-existing IR defect that `Result` was the first to trigger**:
  `codegen_stmt.c`'s fallback `ret %s 0` emits `ret i8* 0` for a pointer return
  type, which is not a legal pointer constant — clang rejected it with
  `integer constant must have integer type`. Any pointer-returning function
  that fell out of its body hit this; a `Result` function is simply the first
  one to get there. Now handled like the existing `double` special case:
  pointers emit `ret %s null`.
- `?` propagation is **still unsupported by both native backends** and remains
  tracked as the remaining half of §8.1 #6 — there the recorded diagnosis does
  hold up: the payload type is `any_type()` at check time (`typecheck_expr.c`:
  `/* payload type is unknown without generics */`), so the emitters cannot
  type the unwrapped value. It needs generics or runtime-value boxing.
  Verified by mutation — removing the `TY_RESULT` branch fails the suite with
  `variable 'good' has no codegen type`.
- Also corrected §5, which still described `?`'s legality check as living in
  the parser and reporting `expected ;, got ? ('?')` with a jumping position.
  It lives in the typechecker and reports `'?' used on a call that does not
  return Result` (verified on the current build).
- A **pre-existing** limitation, untouched here and not introduced by this
  work: native map values must be scalar (`map values must be int, float or
  string`), so `{ ok: <Result> }` is rejected by both emitters while the
  interpreter accepts it. Confirmed against the pre-change tree via
  `git stash` before claiming otherwise.

## [0.3.0] - 2026-10-07

### Fixed — `int` is a real 64-bit integer (规范变更)

The single most consequential fix in this tree's history, and the reason
`docs/SPEC.md` now exists. `int` was **carried in a C `double`**
(`val_int()` did `val_num((double)i)`, and the lexer ran *every* numeric
literal through `strtod`). Consequences, all silent:

- Any integer past 2^53 was rounded. `9007199254740993` became
  `9007199254740992`, and the type checker reported `parse OK`.
- Integer overflow **saturated** in the interpreter while both native
  backends wrapped in i64, so the same program meant two different numbers
  depending on how it was run. `m = 9223372036854775807; m + 1` printed
  `9223372036854775807` under the interpreter and `-9223372036854775808`
  compiled.
- Unary minus widened to float, so `let neg = -7; neg % 3` was refused with
  `'%' does not apply to floats` even though the type checker types `-7` as
  an int.

`int` is now `VAL_INT` carrying an i64, distinct from `VAL_FLOAT`, mirroring
the `TY_INT` / `TY_FLOAT` the type checker already had. Integers are lexed
with `strtoll`, wrapped in arithmetic like the emitters' unadorned `add i64`,
printed from their i64, and serialized to JSON without a detour through
`double`. An integer literal too large for i64 is now a **lexical error**
rather than a silent rounding; `2^63` is accepted only as the operand of a
unary minus, which makes `-9223372036854775808` the minimum i64.

`%` on a float is now refused by the interpreter too (it used to answer with
`fmod`), so all three backends agree.

### Added — a language spec, and a three-way invariant to keep it honest

- **`docs/SPEC.md`**, the first normative document for the language: numeric
  types (int is i64, overflow wraps, `/` is float division, `%` is integer
  with C signs, unary minus preserves the type), left-to-right evaluation
  order, the type system, `Result` / `?`, module resolution and circular
  imports, and the three-backend invariant. It documents what the language
  **is**, not how the code is organised.
- **The int cases are now pinned in CI.** `tests/native-consistency.lume`
  gained literals past 2^53, both i64 extremes, wrap behaviour and signed
  modulo, and `tests/smoke.c` gained 11 interpreter-level assertions
  (146 tests total). The three-way target is the thing that would have caught
  the divergence in the first place.
- **`docs/SPEC.md` gained a complete builtin reference (§5), verified by execution.**
  The previous version documented the *language* but not its 55 builtins, so
  the only written record was `LUME.md`'s name list — no signatures, no return
  types, no failure modes. Writing §5 meant running every builtin, and that
  turned up behaviour no document had recorded, several of which is a trap:

  - `map` / `filter` / `reduce` take **`(fn, list)`** — the reverse of nearly
    every other language.
  - `put` / `push` return **the collection**, not `null`, so they chain.
  - `min` / `max` are **variadic** (not two-argument) and, like `abs`, always
    answer `float` — they go through `double` internally.
  - `round` is **half-to-even**: `round(2.5) = 3`, `round(3.5) = 4`.
  - `int("abc")`, `float("x")`, `len(5)`, `read_file(<missing>)` and
    `env(<unset>)` all answer **silently** (`0` / `0.0` / `0` / `null` / `null`)
    instead of failing.
  - `strftime` takes `(format, timestamp)`, not the reverse.
  - `html`'s slots are **`{N}`** with N from 0; `{{` / `}}` are literal-brace
    escapes. Its result is flagged **trusted**, so `html(html(...))` injects
    raw and skips escaping — an injection channel, now documented as such.
  - `.len` reads a **declared struct field** when the struct has one, but the
    **key count** on an anonymous map; `.length` works on string/list/map only.

- **`docs/SPEC.md` gained control flow, closures and struct semantics (§4, §3.6),
  and two of the gaps found are severe enough to call out separately.**
  Documenting these meant running every construct, which turned up behaviour
  nothing had recorded:

  - **The loop variable is not block-scoped.** `for (let x in [1, 2]) { }` leaves
    `x` bound to `2` afterwards, and a nested `for` reusing the name **overwrites
    the outer variable** — the outer read after the inner loop sees `9`, not `1`.
  - **Closure capture is asymmetric: reads follow the outer variable, writes do
    not.** `let f = () => x; x = 99; f()` answers `99`, but `let c = () => { x =
    x + 1; return x; }` never writes `x` back — each call restarts from the
    captured value. Mutable state has to live in a map or list, which are
    reference values. The same rule applies to named `func`.
  - Every closure made inside a loop shares the one loop variable, so they all
    answer the last value — the opposite of JavaScript's per-iteration `let`.
  - `for` walks lists and maps only; strings and ints are refused. Map iteration
    is in insertion order.
  - A struct is a named map, not a record: `len`/`keys` still count keys, but
    **assignment is by reference** (mutating the copy mutates the original), and
    there is no method syntax at all.

Two **pre-existing backend bugs** were found while pinning this down, both now
in §8.1. Neither is introduced by this work, and both are worse than the
diagnostic-only gaps already listed there:

  - **Any two same-named bindings in one function break the IR text backend.**
    Locals are named `%lv_<name>` per *name* rather than per *binding*
    (`asg_push` in codegen.c), so they collide and it refuses to assemble. This
    covers nested *and* sequential same-name loops — which the loop-variable
    leak makes easy to write.
  - **`push` to the list being iterated OOM-kills both native backends**
    (exit 137). Their for-in never snapshots the iteration length, so a growing
    list means the loop never ends. The interpreter is correct. This is the only
    gap in the spec that does not merely refuse but takes the process down.

16 new assertions in `tests/smoke.c` (179 total), plus a new `reject_syntax`
helper for constructs that must not parse at all (struct methods). The loop
cases that both backends *can* run went into `tests/native-consistency.lume`;
the two broken ones are excluded with a comment saying why.

  17 assertions in `tests/smoke.c` pin those (163 total at that point). Two
  claims written from memory were wrong and are now corrected against the
  implementation: `put`/`push` do not return `null`, and `abs` does not
  preserve `int`.

- `docs/SPEC.md` §7.1 records three **known** backend gaps that are not
  errors — most notably that the libLLVM backend cannot type a variable
  initialised from a unary minus (`let neg = -7; print(neg)`), which is why
  that particular case lives in the smoke suite rather than the three-way
  target.

### Fixed — two native-backend codegen bugs (the §8.1 #4 / #5 gaps)

Both were found while pinning the spec and excluded from the three-way target
at the time; both are now fixed in this commit.

- **Same-named bindings broke the IR text backend.** Locals were named
  `%lv_<name>` per *name*, so two bindings sharing one name (nested *or*
  sequential same-name `for` loops — easy to write because the loop variable
  leaks past the loop) collided and the module failed to assemble. `asg_push`
  now stamps a process-wide binding counter onto the name (`%lv_a_0`,
  `%lv_a_1`), and `codegen_stmt.c` / `codegen_expr.c` emit `store`s through the
  `a->slot` returned by `asg_find` instead of re-spelling `%lv_<name>`. The
  three backends now agree on `seq_same` / `nest_same` in
  `tests/native-consistency.lume`.
- **`push` inside `for ... in` OOM-killed both native backends** (exit 137).
  Their for-in re-read `len(list)` every iteration, so a `push` in the body
  grew the bound forever. Both `cg_for_in` emitters now snapshot the length
  into a dedicated alloca before the loop and load that snapshot in the
  condition, matching the interpreter (which captures `list.count` at loop
  entry). `tests/native-consistency.lume`'s `pt` case now visits only the
  original three elements instead of spinning.

### Fixed — build: guard OpenSSL includes and SSL calls behind HAVE_OPENSSL

`<openssl/err.h>` was included unconditionally, so a machine without
`libssl-dev` (e.g. a bare Debian) failed to compile. It now lives inside the
`HAVE_OPENSSL` guard, an opaque `SSL` typedef is provided for the no-OpenSSL
path so `Conn.ssl` still compiles, and the `SSL_write` / `SSL_read` /
`SSL_free` calls are wrapped in the same guard.

### Changed — the tree itself is named lume-core

- **The directory is now `lume-core`** (previously `lume-lang`).
  The artifact, the PATH entry and the package name had already become
  `lume-core`, so the tree, its binary and its tarball went by two different
  names and every doc had to spell out which was which. `core` also reads
  truer than `lang` while there is no spec, no tag and a thin standard
  library: this is the language's reference core, not a finished language.
  It also settles the submodule name in advance, should the tree ever be
  nested under the host.
- **Everything was rebuilt with `make -B`.** `Makefile` bakes `$(CURDIR)` into
  `-DLUME_RT_SRC="$(CURDIR)/src/rt.c"`, so a plain `make` leaves binaries
  pointing at a `.../lume-lang/src/rt.c` that no longer exists — the paths sit
  in the DWARF of all 33 `build-asan/*.o` and in both `smoke-bin` targets.
  This also caught a stale `bin/lume-asan` surviving from the previous rename,
  and the PATH binary was reinstalled for the same reason.

### Added — a real release path

- **The artifact has its own name.** `make` now links **`bin/lume-core`**
  (previously `bin/lume`, as in the host tree). Two trees that both emit
  `bin/lume` on one machine could not be told apart — that is how a stale PATH
  binary ended up answering for a fresh tree once already. The sanitizer build
  follows suit: `bin/lume-core-asan`. Only the link line changed, so a plain
  `make` suffices (`make -B` is for directory moves / macro changes, not this).
- **`make pack`.** There was previously *no* packaging target and `.gitignore`
  ignored `/bin/`, so "ship a language build" meant a hand `cp` with no manifest
  and no way to reproduce the bytes. `make pack` now writes
  `dist/lume-core-<os>-<arch>.tar.gz` with the binary plus `README.md` /
  `README.zh.md` / `CHANGELOG.md`. Override the name with `make pack PKG_NAME=…`.

### Added — Windows / mingw-w64 native port

- **lume-core now builds as a native Windows executable** under MSYS2 /
  mingw-w64 (a `.github/workflows/windows.yml` CI job compiles and tests it on
  `windows-latest`). The five platform-specific spots are guarded with
  `#ifdef _WIN32` shims rather than dropping the platform:
  - `--watch` (dev hot reload) is compiled out — it depends on `fork` / `exec`
    / `kqueue`, which Windows lacks; passing `--watch` prints an error and
    exits instead of crashing.
  - `mkdir` / `flock` map to `_mkdir` / `LockFileEx`; `realpath` maps to
    `_fullpath`.
  - Outbound HTTP (`http_get` / `http_post` / …) is **opted out** on Windows:
    `src/builtins_http.c` is excluded from the build and `LUME_HAS_HTTP=0` is
    compiled in, so the `b_http_*` wrappers bind to `native_http_unavailable`
    stubs in `builtins.c` (they still type-check, but fail at runtime with a
    clear message). Winsock wiring is not wired up in this first port.
  - `Makefile` detects Windows via `uname -s` (`MINGW*` / `MSYS*` / `CYGWIN*`)
    and sets `PKG_OS := windows`; a `make pack` on Windows therefore names the
    tarball `lume-core-windows-<arch>`.
- **Fixed a latent break exposed by the rename.** `tests/native_backends.sh`
  defaulted its compiler to `bin/lume` and `make native-consistency` never
  passed `LUME_BIN`, so the leg died on `bin/lume missing — run make first`
  while looking like "you forgot to run make". The Makefile now injects
  `LUME_BIN=./$(TARGET)` (what the `asan` leg already did), and the script's
  default follows the artifact name.
- **`make packcheck`.** The tarball was leaking the build machine, in two
  copies: `CFLAGS` keeps `-g`, so DWARF records the working directory, *and*
  the Makefile passes `-DLUME_RT_SRC="$(CURDIR)/src/rt.c"`, which bakes the
  same path into `.rodata` as a string literal. `strip` only removes the
  first. `make pack` now removes both (relink with a package-relative
  runtime path, then strip) and `packcheck` greps the finished tarball, so a
  future change cannot quietly re-introduce either one.
- **`make treecheck`** (also part of `make test`). `packcheck` only ever
  looked at the tarball, and a packed binary is stripped and relinked first,
  so it structurally cannot see an *unstripped* build product — and two
  `backups/lume-core.bak-*` blobs (each holding the compile path 65 times,
  as DWARF) were committed while every guard stayed green, because
  `.gitignore` never covered them. `scripts/check-tree-privacy.sh` closes
  that gap by scanning the blobs reachable from HEAD, which is the question
  the pack guard cannot ask. Those two blobs are now untracked (moved to
  `~/.lume-core-backups/`, outside any repo) and `/backups/` is ignored.
  `--history` additionally walks the object database; it stays red until the
  repo is pushed and rewritten, which is deliberate. Note that the guard
  flags prose too, so comments must write `/Users/<user>/...` in placeholder
  form — it failed on its own header the first time it ran.
- **`LUME_HAS_HTTP` gates the `http_get` registration.** The builtin lives in
  `builtins_http.c`, which not every downstream compiles. A downstream that
  lacks it can now pass `-DLUME_HAS_HTTP=0` and take this `interp.c` as-is
  instead of pinning its own copy.

### Changed — this tree is the host-independent standalone compiler

`lume-core` starts as a two-file research sketch and now carries
the language itself (moved over **without** the host's history, on top of the
existing `594648b` initial commit). The host tree `lume` keeps
the `agent-httpd` embedding and is untouched; this fork is the tree that owns
the language from here on, and it links **libc + optional libLLVM only** — no
HTTP server, no agent runtime, no Docker, no submodules:

- **agent-httpd is gone.** `src/bridge.c`, `src/iquest.{c,h}` and
  `src/builtins_sql.c` (the one builtin file that talked to the host's
  `db_layer.h`) are not carried over. The `AH_LIB` / `AH_INC` lines, the
  `agent-httpd` gitlink and `-lsqlite3` are out of the `Makefile`; so are every
  demo/server/container target (`dev`, `invest`, `demo-sqlite`, `query-demo`,
  `modules`, `abac`, `react-ssr`, `hub`, `run`, `ui`, `vsix`, `image`, …) and
  the `docker`/compose frontend layers. `bin/lume-core` is ~270 KB here against
  ~1.7–2 MB in the host tree — the difference is `libagenthttpd.a`.
- **`src/sbuf.h` is a fork-local stand-in for the `sbuf` half of the host's
  `minijson.h`.** Same contract on the append side (`p` is NULL before the
  first append, the buffer is always NUL-terminated, `oom` latches): the fork
  cannot include a JSON library it does not have. `lume.h` includes it, so
  `vdom.c` / `value.c` / `interp.c` keep working through `lume.h`.
- **`src/bridge_stub.c` replaces the bridge.** `bridge_init` is a no-op,
  `bridge_define_route` / `bridge_define_tool` record the DSL's route/tool into
  the VM's own tables (the same GC roots the host bridge filled), and
  `bridge_run` prints that this tree has no agent-httpd service and
  `exit(2)` — it must fail loudly, not return, or a script that calls `run()`
  would look like it had served something.
- **Tools/skills discovery is a fork-local, empty registry** in
  `src/builtins_catalog.c` (`tools_count`/`tools_get`/`skills_count`/
  `skills_get`, same names and semantics as the host's, plus
  `tool_register`/`skill_register` for later use). Discovery builtins
  `tools()` / `skills()` / `catalog()` therefore return an empty list instead
  of the host's table — kept on purpose so the language surface has no holes,
  the functions exist and behave, they just have nothing to enumerate.
- **`tests/smoke.c` drops the `sql_query`/`sql_write` block** and the
  `tools: registry enumerates …` / `skills: index enumeration includes the
  scratch skill` cases: both seeded from the host runtime
  (`skills_init()` + `SQLITE_DB`), which this tree cannot provide. 106 checks,
  0 failures.
- **`docs/` was rewritten to the fork's shape** (`ARCHITECTURE.md`,
  `DEVELOPMENT.md`, `NATIVE.md` no longer describe the embedded host runtime;
  the server/frontend/iquest chapters are gone).

Everything the guards protect still works here, unchanged: `make test` runs
backend parity (28 `N_*` labels, read from both sides), `tests/native_backends.sh`
(interp / text / llvm three-way), `native-consistency`, `native`,
`native-text`; `make asan` is clean.

### Added

- **The other four HTTP verbs.** `http_post` / `http_put` / `http_patch` /
  `http_delete` now sit beside `http_get`, all five with the same shape —
  `verb(url, opts) -> { ok, status, body, err }` — so a script that writes
  needs a different call and not a different error handling. They share one
  implementation (`http_call()` in `src/builtins_http.c`); what differs is the
  verb and whether the call may carry a body: `post` / `put` / `patch` read
  `opts.body` (a string, capped at 1 MiB) and emit `Content-Length`, the
  others do not take a body at all. Every gate the first verb introduced
  covers all five — the SSRF check, the proxy chain, `--no-net`, the per-hop
  redirect re-check, the timeout.
  `typecheck.c`'s name table lists them, which is what makes the type checker
  accept them; a verb added without that entry compiles and then fails on the
  first call.
- **Eight offline cases pin the malformed-URL branch** all five verbs share.
  `"not a url"`, `""`, `"http://"` and a non-HTTP scheme each answer with
  `ok: false` and a message naming the URL, on every verb — reachable without
  a socket, so the suite holds on a host with no network. Three of the eight
  assert the exact message; the rest assert the `ok:false` shape only, so a
  wording change reports a failure instead of a red build.

### Fixed

- `resolve_redirect()` dropped a **root-relative `Location`**. The
  "same scheme and host, new path" branch took the first `/` of the base as
  the separator, but that is the *second* slash of `http://`, so it rebuilt
  `http:/final` and threw it away — a `Location: /anything` hop never
  followed, with no diagnostic, on `http_get` and on every verb since. The
  host boundary is now located after `//`. Covered: a `POST` that answers
  `301` with a root-relative `Location` now reaches the target and comes back
  `200`.
- A request whose caller supplied `Content-Length` in `headers` sent it
  **twice** — once by the caller, once by the automatic emitter — which a
  server may read as request smuggling. The automatic header now only appears
  when the caller has not set it.
- `obj_string()` was declared `const char *` although the bytes it returns are
  inline in the object and writable, which forced every caller that releases
  or reinterprets them to cast the const away first. One of those casts was
  `memcpy()` from `as.str.data`, which is always `NULL` — the string body
  lives at `obj_string(o)`, so `http_call()` dereferenced null on its first
  `http_post` and died with SIGSEGV before ASan was brought in. The
  declaration is `char *` now, matching the object layout, and the two casts
  it had been provoking are gone.
- `files()` collected its `strdup()`ed names into a `const char *[]`, so
  releasing them needed `free((void *)…)`. They are `char *[]` now.

### Changed

- **`tests/smoke.c`: 120 → 128 checks**, and the three counts in
  `README.md` / `docs/DEVELOPMENT.md` follow (they said 110/120 while the
  binary printed 128 — a stale number in a green build is a worse signal than
  no number). `make test` and `make asan` are clean; the sanitizer build
  still reports `SUMMARY: 0 byte`.

### Added

- **`make lint`.** The project's own `CFLAGS` stop at `-Wall -Wextra`, and
  `src/rt.c` is not even compiled with warnings — `src/backend.c` shells out
  to a bare `cc -O2 -c` to build it, so that file's signatures have never
  been checked against anything. `make lint` now runs `-fsyntax-only` over
  `rt.c` plus every source but the optional `llvm_codegen.c` with a stricter
  set (the project flags plus `-Wshadow -Wstrict-prototypes`): 32 files,
  clean today. The two exclusions are deliberate — `rt.c` sits outside the
  normal build, `llvm_codegen.c` needs the LLVM headers.
- The flags carry **`-Werror`**, which is the point and not a matter of
  taste: `cc` exits 0 when it has only printed warnings (`cc -fsyntax-only
  -Wall` on a file with one unused variable returns 0), so a lint target
  without `-Werror` stays green no matter what accumulates. Checked both
  ways — a clean tree exits 0, and injecting one warning makes the target
  fail with `Error 1`.

### Fixed

- **Two more field declarations were lying about ownership.** `Node`'s
  `lit.text` was `const char *` with a comment saying it points into the
  source buffer and is never owned; the only place it is ever assigned is
  `parser_expr.c`, from a `strdup`, and `parser.c` frees it — so the type
  forced `free((char *)…)` and the comment was simply wrong. `VM.load_stack`
  was `char **` although it only ever holds a `const char *` path, which
  needed a cast at its one assignment. Both are honest types now and the
  casts that depended on them are gone; `node_free_own()` and the import
  cycle check behave the same.

### Added

- **`http_get(url, opts) -> { ok, status, body, err }`** (`src/builtins_http.c`):
  the outbound HTTP the host tree could only do from inside `agent-httpd`. Raw
  sockets, no libcurl, so the builder stays at "libc + optional libLLVM +
  optional libssl":
  - **SSRF gate is on by default** — loopback, `localhost`, link-local
    (`169.254/16`), RFC1918, CGNAT and friends are refused *before* any socket
    is opened; `http://ok@127.0.0.1/` (real target hidden in userinfo) is
    refused too; names are resolved and every returned IP is checked, so a
    Round-Robin answer gets no second chance. `allow_private: true` opts out.
  - **Proxy follows the environment**: `LUME_HTTP_PROXY` → `https_proxy` /
    `http_proxy` → `HTTPS_PROXY` / `HTTP_PROXY`; `https://` targets get a
    `CONNECT` tunnel and then a verified handshake (SNI set, system CAs).
  - Redirects are followed up to 5 hops, re-checked at every hop; body is
    capped (1 MiB default, `max_bytes`), everything is bounded by a `timeout`
    **in seconds** (default 10, cap 60) that covers the whole request.
  - The request always says `Accept-Encoding: identity` — this build does not
    decompress, so anything else turns the body into binary.
  - `--no-net` / `LUME_NO_NET=1` is the master switch: no packet leaves.
  Example: `examples/http-get.lume`.
- OpenSSL is now a *detected* optional dependency (`HAVE_OPENSSL`): the
  `Makefile` tries `pkg-config` and falls back to `brew --prefix openssl`
  (then `openssl@3`), and only links `-lssl -lcrypto` when
  `<prefix>/include/openssl/ssl.h` exists. Without it, `https://` targets
  return `needs libssl` instead of failing to compile.

### Fixed

- `host_resolves_private()` returned "private" whenever `getaddrinfo` produced
  *any* address — every public host was rejected as an SSRF attempt. Only a
  genuinely private answer refuses, now.
- `getaddrinfo(host, NULL, …)` never resolved an IP-literal host (the proxy
  address), and when patched to `"0"` it returned a `sin_port == 0` sockaddr,
  so `connect()` failed with `EADDRNOTAVAIL`. The service argument is now the
  real port.
- `poll()` returning `EINTR` aborted the connection wait, which looked like
  "proxy sent no CONNECT reply". `EINTR` now retries; so does the handshake.
- The CONNECT response status was read at a hardcoded offset; it now parses the
  three digits after `HTTP/x.y `, which also holds for `HTTP/1.0`.
- `body` was `free()`d twice on the success path (`exit 134`, SIGABRT). All
  releases now go through `body_done()`, which frees *and* resets, and the
  buffer is only released after the result string has copied it.
- `tls_attach` handed a relative 15000 ms to waiters that expect an absolute
  deadline, so every TLS handshake timed out immediately.
- `tests/smoke-bin` had **no build rule** after the agent-httpd detach:
  `make test` referenced it but nothing ever compiled it, so the interpreter
  unit tests ran whatever stale binary was on disk. Restored
  (`CORE_OBJS` + `build/tests`) and added four offline assertions for the
  `http_get` gates (110 tests, 0 failed).
- `examples/hello.lume` was still the host tree's server example — `server {}`
  with a `docroot`, routes, a `tool` and `run()`. `make check` only ran
  `--check` on it, so it stayed green while `lume examples/hello.lume` exited
  **2** on `run() needs the agent-httpd host build`. Rewritten as a plain
  script (function + `print` + `get()` map reads), which is also what
  `docs/LUME.md` always promised it was.

### Changed

- The text backend's 2782-line `src/codegen.c` is cut along the section
  boundaries it already had, into one tu per stage:

  | file | what it owns |
  |---|---|
  | `src/codegen.c` (+ `codegen_internal.h`) | the entry, the CG context, the container helpers, `codegen_emit_ir` |
  | `src/codegen_types.c` | `Type` -> LLVM spelling (the struct interning table) |
  | `src/codegen_expr.c` | every expression form, the `Val` helpers, the builtin table |
  | `src/codegen_scan.c` | static types of expressions; block scanning |
  | `src/codegen_sig.c` | signature inference (`codegen_infer_signatures`) |
  | `src/codegen_stmt.c` | statements, loops, function bodies |

  Pure move: no logic changed, and the verdict is that `--compile-text` on all
  14 example/test scripts produces byte-identical `.ll` (and the same messages
  on the paths that are rejected today). A function is visible through
  `codegen_internal.h` exactly when another tu calls it; everything else stays
  `static`, so the text backend still exports only `codegen_emit_ir` and
  `codegen_infer_signatures`. `scripts/check-backend-parity.sh` now reads
  `src/codegen*.c` as one side — without that a `case N_...` living in
  `codegen_stmt.c` would read as a divergence instead of a hit.

### Added

- Opt-in `--no-fs` runtime lock (also `LUME_NO_FS`, any value but `0`/`off`/
  `false`). `read_file` / `write_file` / `files` / `mkdir` / `lock_file` were
  completely unguarded on the path, so an untrusted script could exfiltrate
  exactly the credentials `env()` masks (`read_file(".env")`) — masking the
  variable and then leaving a door next to it is not a boundary. With the
  switch on those builtins fail loudly at run time instead of touching disk;
  type checking is unaffected. Off by default: the product toolchain writes
  `frameworks/autogen-pse/.env` and `.data/` from outside the cwd. See
  `docs/LUME.md` ("安全边界:解释器不是沙箱"). Four cases in `tests/smoke.c`
  plus both directions of `tests/fixtures/no-fs.lume` in `tests/run_all.sh`
  (off => it runs, on => it aborts with the guard's message) pin it down.

- `make asan` now drives `tests/run_all.sh` as its last leg, so the end-to-end
  suite runs against the sanitized binaries (see the entry further down).

- Route handlers now see `req.query` (raw query string without the `?`;
  null when absent) and `req.query_params` (URL-decoded map; a segment
  without `=` gets an empty value, repeated keys last-wins, `+` decodes
  to space).

- Tests for the 15 builtins that had none: the whole math family
  (`abs`/`sqrt`/`exp`/`log`/`ln`/`pow`/`floor`/`ceil`/`round`/`min`/`max`) plus
  `now` / `push` / `catalog` / `discovery_endpoints`. Twelve cases in
  `tests/smoke.c`, among them a new `check_err()` helper for the paths the type
  checker cannot reject — the domain guards (`sqrt(-1)`, `log(0)`,
  `abs("3")`, `min()`) only fire at run time, so they were unreachable from the
  existing `check`/`reject` pair.

- The end-to-end suite runs against the sanitized binaries: `tests/run_all.sh`
  takes `LUME_BIN` / `SMOKE_BIN` / `TOOLS_BIN`, and `LUME_SKIP_MAKE` stops it
  from rebuilding the plain ones. `make asan` never started a server, so live
  HTTP, the forked request workers and the fork/exec `--watch` reload were all
  running unsanitized. It used to be a separate opt-in target because it
  failed (below); the fix landed in the submodule, so the leg now runs inside
  `asan` and the extra target is gone — sanitized coverage of the request path
  is worth more than the minute it costs, and an opt-in that can fail is a
  place for regressions to hide.

- `editor/lume-vscode/scripts/gen-builtins.py` now covers every registry
  builtin. Entries without a `SIG` key were skipped silently, so
  `builtins.lume` stopped at 39 declarations and the math builtins could not be
  jumped to from the editor; the script now emits all 51 and still refuses to
  run when its tables disagree with the registry.

- `examples/react-ssr.lume` (:8085, `make react-ssr`): a React SSR content
  page driven by a resident node backend — the `server` block sets
  `react_socket` (`./.data/react-ssr.sock`) and the built-in HTTP engine
  FastCGI-relays `/react/*` to `bin/react-ssr-server` (built from
  `frontend/react-ssr/` TSX by `pnpm install` + `scripts/build-react-ssr.sh`;
  `make react-ssr` builds, starts the backend, then runs lume). React components render with `react-dom/server`
  `renderToString` + `StaticRouter` — `/react`, `/react/about`,
  `/react/counter` all server-render; query strings (`?name=Ada&message=hi`)
  are relayed and echoed. No CGI fork, no Lume vdom. Without the backend,
  `/react` degrades to the static `www/react/home.html` shell.

- Experimental native backend: `--compile` turns a `.lume` script into a real
  executable (typechecked AST → LLVM IR → native binary), now with a
  **libLLVM default**: when `bin/lume` was built with libLLVM, the IR is built
  through the C API (`src/llvm_codegen.c`) so LLVM verifies its shape where it
  is produced; `--compile-text` opts out and goes back to handwritten IR text
  (`src/codegen.c`) with `clang -O2`. libLLVM stays optional — the Makefile
  probes `llvm-config`, and a host without it gets a `bin/lume` whose default
  path is the text one. New targets `make native` / `make native-text` /
  `make native-llvm` / `make native-bench`, plus `examples/native-fact.lume`
  and `examples/native-bench.lume` with their expected outputs. Covered: the
  core subset — scalars, structs, functions, `if`/`while`/`for`, arithmetic and
  comparisons, and `print`/`abs`/`min`/`max`/`sqrt`/`pow`/`floor`/`ceil`/
  `round`/`str`/`int`/`float`/string `+`. `server {}`/`route`/`tool`/`run()`
  remain interpreter-only for now.

- Documented cost of the libLLVM default: `LLVMRunPasses` cannot be used from a
  pure-C TU on this libLLVM build (a minimal probe reproduces the abort for
  every pass string and every code model), so the libLLVM path runs no
  optimization pass and is one to two orders of magnitude slower than
  `--compile-text` on a loop-heavy target. Run `make native-bench` for the
  numbers.

- Cross-backend consistency test (`make native-consistency`, part of
  `make test`): `tests/native_backends.sh` compiles
  `tests/native-consistency.lume` through both IR emitters (`--compile-text`
  and `--compile-llvm`), runs each binary, diffs both against
  `tests/native-consistency.expected` and against each other. Any compiler
  output on stderr counts as a failure, so IR that clang or LLVM's verifier
  rejects does not count as "it compiled". Missing clang or a libLLVM-free
  build skips that leg instead of failing. The fixture now also covers struct
  fields (pass, return, read, write, literal and call-result) and a top-level
  statement, which is what pins the struct and entry-point fixes below.

- Static backend-parity guard (`make backend-parity`, part of `make test`):
  `scripts/check-backend-parity.sh` diffs the `case N_*` labels handled by
  `src/codegen.c` and `src/llvm_codegen.c`, so a new AST node implemented in
  only one emitter now fails the suite instead of silently falling into that
  emitter's `default:` branch. A node deliberately absent from one side goes
  in the script's `KNOWN_DIFF` list. The script also refuses to "pass" when a
  side matched no case at all, which is how a silently-empty grep used to
  look.

- `asan` leaks one knob instead of eight: the sanitizer runtime options now
  come from `ASAN_BASE_OPTS`, shared by all eight legs, so the leak policy is
  a single documented default and no future leg can quietly reintroduce the
  hardcoded flag. LeakSanitizer is on by default (`ASAN_BASE_OPTS` ships
  empty); `make asan ASAN_BASE_OPTS="detect_leaks=0"` is the way to exempt a
  single investigation. Why it used to be off, and what it cost, is recorded
  under Fixed — that reason no longer holds.

### Added

- The native backend gained the container types: list literals (`[1, 2, 3]`),
  string-keyed map literals (`{"a": 1}`), `len` / `push` / `get` / `put` /
  `keys`, `.len` / `.length` on a list, a map or a string, and `for (x in xs)`
  over a list (elements) or a map (keys, insertion order). In native code
  there is no GC and no `Value` layout, so both emitters represent a list or a
  map as a **heap object behind an opaque pointer** (`i8*`) and store tagged
  slots (`int` / `float` / `string`), picking one of the `lume_list_*` /
  `lume_map_*` helpers from the *static* type at the call site.

- `and` / `or` short-circuit in the native backends, and the result is the
  constant on the short path: `d == 0 or 10 / d > 1` is `true` even though the
  division is never evaluated. Both emitters use three blocks (short / long /
  join) because two labels make the join block be emitted twice, which clang
  rejects as a terminator in the middle of a basic block.

### Added

- The native backends now infer what the source leaves out: a parameter or a
  return type that is not annotated is resolved from the call sites and from
  the body, and the result is written back onto the AST, so both emitters read
  the same signature. `codegen_infer_signatures()` is run once by the driver
  *before* it decides which emitter to use, and both backends run it again as a
  safety net — implementing the fixed-point pass twice is how two emitters
  drift. A name gets exactly one signature: `f(1)` then `f("s")` is an error
  reported at the offending line ("argument 1 of 'f' is a string, but the
  parameter was already resolved as a int"), not a first-call-wins guess. The
  struct comparison used by the pass treats an anonymous struct as matching any
  named one, so `sq_dist({ x: 3, y: 4 })` against a `Point` parameter holds.

- `main()`'s return value is now the process exit status, on all three paths.
  Only a `main()` the *top level* calls counts: the synthesised `L_top`
  becomes int-typed and forwards that call, the C entry does
  `call i64 @L_top()` → `trunc i64 %r to i32` → `ret i32 %t`, and the
  interpreter records the same value for the entry module only (an imported
  module that happens to call `main()` is an ordinary call). A program whose
  top level never calls `main()` still exits 0. Pinned by the new exit-status
  leg of `tests/native_backends.sh`, which asks all three legs for the status
  of a `main()` returning 3 and of a program with no top-level `main()`.

- An opt-out for the optimisation pipeline itself: `LUME_NO_PASS=1` (any value)
  skips the `LLVMRunPasses` call before lowering, so the effect of the pipeline
  is measurable instead of assumed. `make native-bench` gained a `nopass-ms`
  column built on it — the llvm leg runs 9ms with the pipeline and ~215ms
  without, which is what keeps "mem2reg is really running" an observable fact
  rather than a note in a comment.
- The same opt-out as a flag rather than an environment variable: `lume
  --compile --no-pass` and `--compile-llvm --no-pass` skip the pipeline, with
  `LUME_NO_PASS=1` kept as the equivalent spelling for scripted runs (the
  `nopass-ms` column of `make native-bench` uses the flag). The switch is
  probed once per process, so it cannot leak between compiles.

### Fixed

- `--no-pass` is accepted rather than rejected by a build without libLLVM. It
  prints a note saying there is no pipeline to skip, instead of falling through
  to the usage message with status 2 — a switch that means "no pipeline here"
  should still compile. (While checking the no-libLLVM build also compiled, the
  flag pushed its way into that path, and `use_llvm` had to move inside the
  HAVE_LIBLLVM guard to stop the build warning about an unused variable.)

- A for-in loop over a list whose element type cannot be inferred — a `let xs =
  [];` that is only filled inside a loop, say — used to SEGV the compiler on the
  libLLVM leg (exit 139, no message at all). The loop variable is registered
  with whatever `for_in_elem_type` found, and when it finds nothing that NULL
  reached `LLVMBuildAlloca` in the entry block. The alloca pass now refuses to
  build a slot it has no type for and says so; `llvm_codegen_module` already
  turns `g->err` into a clean exit 1, so both emitters now print the same kind
  of diagnostic for the same source instead of one of them dying.

- Once the pipeline landed, `make native-bench`'s `run-ms` column stopped
  meaning what its name says: the target is a pure arithmetic loop, and O2 folds
  it into vector arithmetic (`<2 x i64>`, `mul i64 %n, 1000`), so 1e8 iterations
  disappear before codegen and the column only reports whether the optimiser ran
  at all — a number the two backends now agree on for the wrong reason. The new
  `nopass-ms` column is the honest signal (see above).

- An integer value handed back through a float slot used to reach clang as
  `ret double 1` — LLVM reads that as an integer constant, and the error points
  nowhere near the statement. The return statement now widens through `coerce`,
  which materialises the `sitofp` as its own instruction (`ret double sitofp
  i64 %t0 to double` is rejected outright: constexpr operands are gone), so
  `func f(v: int): float { return v; }` compiles on both emitters.

- The libLLVM backend now runs the optimisation pipeline (`default<O2>`) before
  lowering, which is the one thing the default path used to be unable to do.
  The alloca-heavy IR was emitted without mem2reg, so `make native-bench`
  reported 220ms against the text backend's ~7ms on the same loop; it is now
  5ms against 9ms, i.e. the two are in the same band again. The runner is
  `LLVMRunPasses` from `llvm-c/Transforms/PassBuilder.h`. The long note in
  `backend_llvm.c` concluding that it "aborts when used from a C-only TU" was
  wrong twice over, and both halves are recorded where the code now stands:
  the header was already included by that file (the standalone probe did not
  pull it in, so `LLVMRunPasses` compiled as an *implicit declaration*
  returning `int` and was stored into `LLVMErrorRef`, a pointer — that was the
  SIGSEGV), and it is opt level `None`, not `Default`, that dies. The stderr
  note that used to warn about the cost is gone with its premise.

- The default backend was put to the question and left alone: `--compile` still
  uses libLLVM when it is linked in. The option was to make text the default
  (the binary would come out an order of magnitude faster — the cost is all on
  the run side, compiling is even slightly cheaper) and to fall back to
  heuristics per script, both rejected: clang is already a dependency of both
  paths, so switching the default buys speed but costs the visibility of the
  optional dependency, and a per-script choice would make two machines pick
  different backends for one file. `docs/NATIVE.md` records the decision with
  the alternatives, so a later session reopens it with `make native-bench` in
  hand rather than the table quoted above.

### Unresolved

- The two SSR-relay assertions in agent-httpd's own `make test` fail on this
  machine, before and after this change: "SSR relay document carries
  Cache-Control: no-store" and "SSR relay document asks for the
  fingerprinted bundle". Both are cache/bundle-relay behaviour and unrelated
  to `status_text`, so lume's suites do not touch them.

### Changed

- The interpreter's two dispatch tables were the widest functions in the
  tree (`eval_expr` and `exec_statement`, ~300 lines each); the heavy cases
  now live in their own functions (`eval_expr_literal` / `_member` /
  `_binary` / `_call`, `exec_statement_for` / `_route` / `_verbs` / `_tool`)
  and the dispatchers are a switch of one-line calls. Behaviour is unchanged;
  the end-to-end suite, the tool registry test and the three-way native
  consistency check all still pass.

- `src/typecheck.c`: `is_builtin_name()` and the "builtins are loose" scope
  seeding each carried their own copy of the builtin name list (same names,
  different order and comments). They now share one `LUME_BUILTIN_NAMES`, so a
  new builtin only has to be added once instead of twice.

- Docs catch up with the code: `docs/ARCHITECTURE.md` now lists the five
  flags its CLI table was missing (`--compile` / `--compile-text` /
  `--compile-llvm` / `-o` / `--help`) and the gitlink (`fcb80e07`, recorded
  three different ways before), `docs/DEVELOPMENT.md` drops a duplicated
  section and its stale test counts, and `docs/LUME.md` gives the editor
  builtin file the right number of declarations.

- `make ui` / `make ui-items` chained the dependency install and the build in
  one shell line (`{ [ -d node_modules ] || pnpm install; } && pnpm run
  build`). A failed install did not stop it: pnpm ran anyway, failed on the
  missing binary, and what a reader got was a wall of module-not-found rather
  than the real cause. Both targets now give the install its own recipe line
  and exit on failure. `make test-all`'s comment also called the five legs
  `test` pulls in "the three legs" with no list; it names them now.
  `scripts/build-react-ssr.sh` likewise stopped hardcoding `:8085` in its
  header: that port is `SSR_CONTENT_PORT` overridable from the Makefile.

### Fixed

- A failing parse aborted the compiler instead of reporting itself. Any script
  that had parsed at least one statement before the error died with SIGABRT and
  no diagnostic at all — the message `lume: line N: ...` never printed, because
  the process died on the way out of `parse_program()`. Two were double frees
  against the orphan journal:

  * the failure path called `node_free(prog)`, which walks the whole partial
    tree, and then let `orphan_release_all()` free the very same nodes — every
    node the parser allocated is in that batch; `prog` is not, it is the one
    `orphan_remove()`d up front. Now the sweep is the only thing that releases
    the statements and `prog` only gives up its own shell.
  * `parse_postfix()` freed the argument node it had just built (`free(args)`)
    after moving its items array to the call, and `parse_expression()` did the
    same with the left-hand side of an assignment. Those nodes stay in the
    batch, so the sweep reached them again. They now go through the new
    `node_unjournal()` first — the matching release is the *shell* only, since
    whatever the node owned already moved to a node that outlives it
    (`node_free()` on `args` would have freed the items array a second time).

  `tests/fixtures/parse-error.lume` pins the whole contract: non-zero exit, a
  message naming line 10, and no abort.

- The libLLVM emitter did not implement `.len` on a string, so
  `print(s.len)` compiled under the interpreter and under `--compile-text` and
  failed on `--compile-llvm` with `print() cannot print this value` — a
  diagnostic on line 3 about something line 3 never did. The `.len` branch in
  `cg_member()` now covers `TY_STRING` (`lume_bi_len_s`) next to the list and
  map cases.

- Leak detection is real now instead of excused. `ASAN_BASE_OPTS` ships
  empty, so LeakSanitizer runs on every `asan` leg by default; it used to be
  pinned to `detect_leaks=0`. On CI's platform (gcc:12, Ubuntu bookworm
  arm64, 2026-10-03) every leg is clean — `SUMMARY: 0 byte(s) leaked` —
  after the compiler-side object chart was actually freed rather than
  suppressed. The baseline that did not move was
  `6091427 byte(s) leaked in 42939 allocation(s)` in the smoke binary: the
  old flag made a red CI a certainty.

  * `parser.c` keeps an orphan journal — `nalloc()` records every node and
    `parse_program()` releases the batch when it gives up — which is where a
    rejected script used to strand a half-built tree. That only works
    because `node_free()` is two passes now (`node_free_kids` /
    `node_free_own`): a sweep may already have freed the child of the node
    being swept, and walking into it is a use-after-free. Hence
    `node_free_orphan()` for the batch, which never descends.
  * `value.c`'s `gc_collect()` called `free(o)` without `obj_release(o)`,
    so every collection stranded the `gc_cstr` names and the map/env keys
    the object owned. Only the `--compile-*` legs grow a heap big enough to
    reach a collection; `--check` and both unit tests read as clean
    throughout, which is why this one hid for so long.
  * `main.c`'s `--compile-llvm` refusal returned 1 *after* `vm_init()`,
    skipping `vm_teardown` and leaking an entire VM — every builtin native,
    the globals env, the loaded module table — on a path that never
    produced a binary.
  * `codegen.c`: `val_make()` already copies its string, so the outer
    `xstrdup()` at ten call sites was a double allocation nothing could
    reach; `val_take()` takes ownership instead. `cg_free()` releases the
    signature and struct tables from one place on both exits.

  A suppression file was the alternative and is not needed: it would
  swallow every new leak and rot with the code it names.

  The platform asymmetry is unchanged and still worth knowing: on macOS
  LeakSanitizer does not exist, so `make asan` cannot prove leaks clean
  locally — it only proves UB and out-of-bounds clean. CI (ubuntu-latest,
  `make -j4 asan`) is where a new leak will surface.

- `codegen.c` freed the synthetic `top` node along with the pass-2 arrays it
  shares (`fns` / `tops`), which is before the entry point reads it: `if
  (top_fn)` then tested freed memory, and `main()` stopped emitting
  `call void @L_top()`. The result was a program that compiled, linked,
  exited 0 and printed nothing — `make test` caught it, because the
  cross-backend consistency check diffs every leg against
  `tests/native-consistency.expected`; `make asan` did not, because its
  `--compile-*` legs only looked at the exit code. The node is now released
  after the entry point has read it, and the consistency check itself runs
  inside `make asan` as well (through `LUME_BIN`, the same env knob the
  e2e leg already takes), so a backend that is leak-clean and wrong fails
  there too instead of only on the plain build.

- `set_error_response` (agent-httpd) no longer copies into
  `response->status_text` from that same field: three call sites — the
  keep-alive fill in `http.c`, `fast_serve` in `event.c` and the FastCGI path
  — hand the field in as the `status_text` argument, so the `strncpy` had
  src == dst, UB the plain build gets away with and ASan rejects as
  `strncpy-param-overlap` (this ASan build has no flag for that check). The
  new `copy_status_text` memmoves, which tolerates the overlap, and writes the
  terminator itself — the old `strncpy(..., sizeof - 1)` never did, so a phrase
  long enough to fill the 64-byte field left it unterminated. That is what
  let the sanitized e2e leg abort on the first 404; with the gitlink bumped,
  `make asan` drives the live suite and passes.

- `/api/reports/<name>` read the target path with `stat()`, which follows
  symlinks: a link planted in the reports directory (pointing at `/etc/passwd`
  or a dotfile) satisfied the `S_ISREG` check and was returned through the API.
  It is now `lstat()`, and `S_ISLNK` is refused. `tests/run_all.sh` plants such
  a link and asserts a 404, so the hole cannot come back unnoticed.

- Makefile: `make test-all` silently ran a weaker suite than `make test` (the
  `backend-parity` / `crypt-test` / `native-consistency` legs were missing),
  `make asan` never exercised the two IR emitters under a sanitizer (no
  `--compile-*` leg), `tests/native_backends.sh` could only ever be driven by
  the plain binary (`TARGET` was hardcoded, so the sanitized legs could not
  reuse it), and `make native-bench` used BSD-only `stat -f%z`, which fails on
  Linux.

- `tests/native_backends.sh`: `curl … | grep -q` in the e2e leg of
  `tests/run_all.sh` closed the pipe early and the shell reported the
  `curl` as failing with SIGPIPE (exit 141) under `pipefail`, which read as
  "React bundle marker missing"; the body is now written to a file first.
  Its process check matched `bin/lume` only, so the sanitized build — which
  runs `bin/lume-asan` — never found "any server processes".

- `src/loader.c`: silenced the last `-Wextra` warning in the tree.

- `--compile-llvm` on a build without libLLVM used to fall through to the text
  backend silently, behind a note on stderr: the caller got a binary from an
  emitter it did not ask for. It now exits 1 with a message pointing at
  `--compile-text` / `LLVM_CONFIG=`.

- The `llvm-config` probe no longer reports success for a path that does not
  work. `HAVE_LLVM` used to be "the path is non-empty", so a stale
  `/opt/homebrew/opt/llvm/...` symlink (or a hand-passed `LLVM_CONFIG`)
  produced a hard `fatal error: 'llvm-c/Core.h' file not found` instead of the
  documented text-backend fallback. The probe now asks for `--version`.

- A build without libLLVM did not build at all: `main.c` called
  `backend_llvm_native()` without a declaration, which the compiler rejects.
  The whole libLLVM branch is now behind `#ifdef HAVE_LIBLLVM`, which is what
  makes "optional libLLVM" true rather than aspirational.

- The libLLVM backend gave a `void`-returning call a name (`%t = call void
  @L_top()`), which the verifier rejects with "Instruction has a name, but
  provides a void value". A void call now gets an *empty* name instead: `NULL`
  is not an option on this libLLVM build (the C API turns the name into a
  `Twine` and dereferences it, so a script with any top-level statement
  segfaulted the compiler), and `"c"` is exactly the malformed one the verifier
  complained about.

- Struct field access. It was known-broken on the libLLVM backend and now
  works on both: (1) the field `getelementptr` was spelled with a struct type
  recovered from the pointer, which on LLVM 23's opaque pointers yields `half`
  and is rejected outright — the struct type now comes from the frontend;
  (2) a struct *value* (a literal, a call that returns one) had no address, so
  `f().x` indexed the aggregate as if it held a pointer — a `struct_addr()`
  helper now slots such a value before indexing it; (3) `p.x = e` as a bare
  statement had no `N_ASSIGN_MEMBER` case in either backend's `cg_expr`, and
  both read `n->as.member.type` off a union member that does not exist there.

- The libLLVM backend segfaulted on any script with a top-level statement
  (`run();`, `let x: int = f();`): the C entry point's `call void @L_top()`
  passed `NULL` as the call's name to `LLVMBuildCall2`, which crashes on this
  libLLVM build. See the previous entry.

- Route handler lookup now matches the path with the query string stripped
  (the framework already dispatched on the stripped path, but the DSL shim
  compared the full URI, so any `/echo?a=1` request fell through to a 404).

- Call-argument lists now accept a trailing comma (`f(a, b,)`), matching
  list and map literals; previously the extra comma made the parser try to
  parse the closing paren as another expression.

- Fix locals declared inside a nested block in both native backends:
  `scan_stmt` fell through to `default` for block nodes, so a `let` in any
  `if`/`while`/`for` body was never registered in the codegen locals table —
  `--check` passed while `--compile` failed with `bad operands` /
  `unknown variable` for a variable that is plainly in scope.

- Fix a dropped call result in the libLLVM backend: the return value of
  `LLVMBuildCall2` was not captured and codegen fell back to `LLVMGetUndef`,
  emitting `mul i64 %v1, undef`. `LLVMVerifyModule` accepted it, so the wrong
  answer only showed up at runtime.

- `tests/native_backends.sh` advertised three legs but only compared the two IR
  emitters: the interpreter never ran, so a backend that agreed with the text
  backend while disagreeing with the language itself still passed. The
  interpreter is now the first leg of the comparison and every pair of legs is
  diffed against each other, so "three-way consistency" no longer reduces to a
  two-way one. The interpreter's cosmetic `note: script completed without
  run()` line is stripped before the diff; anything else it writes to stderr is
  a failure like any other leg's.

- String concatenation in `eval_expr_binary` never checked its `malloc`, and
  the very next lines write `buf[total] = '\0'` — on a NULL return that is a
  null write. It now reports out of memory instead.

- Two guards in `tests/native_backends.sh` were decoration, not guards. The
  `[ ${#RAN[@]:-0} -ge 2 ]` leg-count check is not an expansion bash accepts:
  the shell rejected it with `bad substitution`, so the check never ran and the
  one-leg case passed having compared nothing (missing clang and no libLLVM was
  enough to get there). The length takes no `:-` default, so it is
  `${#RAN[@]}` now. The libLLVM skip branch matched the refusal text with an
  exact phrase as its only build-time signal, so rewording the refusal in
  `main.c` would have failed every build *without* libLLVM for a reason that
  has nothing to do with the code under test; the wording change is now
  reported as a skip with a note instead of a failure.

- The `asan` job in `.github/workflows/ci.yml` now uploads `lume/logs/` as an
  artifact on failure, like the `test` job already did. That matters more than
  it sounds now that LeakSanitizer is on by default and the report is one long
  block at the tail of a very long log.

- The libLLVM backend was the one leg nobody had ever run under a leak check,
  so its leaks had no reason to show: the first time LSan saw it, it reported
  `18524 byte(s) in 85 allocation(s)`. Two roots. `llvm_codegen_module` made a
  fresh `LLVMContext` per call and handed back the module alone — the context
  is not the module's to own, and the failure path disposed it while the
  success path did not, so every `--compile-llvm` stranded it together with
  everything that context holds. It is handed back through an explicit
  out-parameter now and disposed by the caller, *after* the module, which is
  the only order that is safe. The remaining `66 byte(s) in 11 allocation(s)`
  were lume's own: the signature and struct tables xstrdup the names they are
  given and only their array shells were freed, and `cg_function` never
  released its parameter-type and parameter-slot vectors. Both are zero, on
  LLVM 14 and 16.

- `HAVE_LIBLLVM` is a compile-time macro baked into `main.o`, so a build that
  had just found `llvm-config` had to rebuild *everything*: linking a fresh
  `llvm_codegen.o` against the old `main.o` yields a half-fresh binary whose
  `--compile-llvm` still answers "needs a build with libLLVM". The probe
  result is a prerequisite of every object, sanitized ones included.

- `/` was integer division in both native backends and floating-point division
  in the interpreter: `10 / 4` answered `3`-and-a-bit-wrong (`3` from `sdiv`,
  `2.5` from the interpreter) with no error to point at it. The operands are
  now widened with `sitofp` before the `fdiv`, and the result type is float in
  both emitters — the language's `/` is float division regardless of operand
  types. Pinned by three lines in `tests/native-consistency.lume`, which is
  where the drift could only ever have been caught: the fixture compares the
  interpreter against both emitters, and a wrong answer matches nothing.

- `m.len` on a map was accepted by both native backends and refused by the
  type checker, so the same script failed under `--compile` and ran under the
  interpreter. `.len` / `.length` is now a property of the container in all
  three legs: string, list, and anonymous struct (a runtime map).

- The libLLVM emitter had no notion of a forward call. IR text declares a
  symbol the moment it sees a call, but through the C API a `main` that calls
  a helper defined below it needed the symbol to exist already, and
  `LLVMAddFunction` answers by uniquing the name (`L_g.1`) — so a forward call
  either failed codegen with `print() cannot print this value` or linked with
  `Undefined symbols`. `cg_call` now creates the symbol on demand and
  `cg_function` reuses it instead of adding a second one.

- `tests/native_backends.sh` accepted the sanitizer's own stderr as the
  libLLVM refusal text. On a sanitized binary the leak summary says
  `libLLVM-14.so`, the skip branch matched it, and the script reported
  `libLLVM backend skipped` on runs that *did* have libLLVM — the libLLVM leg
  was never run under `make asan`, the one place it was added for. Sanitizer
  output is stripped from the probe now, and the suppression table ASan
  always echoes has to go as well (`print_suppressions=0`); it was being read
  as a compiler diagnostic and failing the leg for not being one.

- What the probe is a prerequisite of was a path, not a resolution. Passing a
  bare name (`make LLVM_CONFIG=llvm-config-18`, on PATH but not in the working
  directory) or one that no package has installed yet made make stop with
  "No rule to make target 'llvm-config-18'" — a harder failure than the
  half-fresh binary the prerequisite exists to prevent. The prerequisite is a
  resolved path now (an existing file first, then `command -v`), so the
  rebuild still happens and the bare name still builds.

### Changed

- `--compile` now follows the libLLVM backend when the binary has one instead of
  always taking the text path; the old behaviour is one flag away with
  `--compile-text` (and `make native-text`). `--check`/`--dump`/the interpreter
  are untouched.

- `make test` no longer requires the frontend. It used to depend on `make ui`
  (pnpm), so on a host without pnpm the whole regression died within a second,
  before the suite had run at all — the end-to-end script itself was fine. The
  React bundle is now opt-in behind `make test-all`. `tests/run_all.sh` also
  probes `/sum` after the GC-stress loop instead of counting `ps` output; the
  old count reported zero and failed the run on hosts where `ps` is not
  permitted, even with the server alive and serving.

- The native entry point no longer calls `main()` for the program. It runs the
  top level and nothing else, so `main` is an ordinary function under
  `--compile` exactly as it has always been under the interpreter: a script
  that wants `main()` to run calls it from the top level, in source order.
  The entry used to run `main` *after* the top level, which ran it twice for
  any program whose top level called it (the interpreter ran it once) and ran
  it where the interpreter printed "script completed without run()".
  `examples/native-fact.lume` and `examples/native-bench.lume` now end with
  `main();`. Consequence: `main()`'s return value is no longer the process
  exit code; the native binary always exits 0.

- `tests/run_all.sh` asserts on the two frontend build products (`/app.css`
  and `/invest/app.js`) that `git` does not track — `.gitignore` ignores
  `/www/**/*.css` and `/www/**/*.js` and only `make ui` produces them. On a
  cold checkout, which is what CI gets, they are simply absent and the static
  handler answers 404, so the suite's first red on a fresh clone was about a
  file no one had built. Both assertions now run when the bundle is present and
  report `skip` when it is not, so CI says what it did not exercise instead of
  claiming green.

- `tests/run_all.sh` grew a case for the iquest cross-origin guard
  (`src/iquest.c`, `origin_ok`). `/api/reports` and `/api/settings` are
  registered in C, not from any `.lume` source, so nothing ever called them
  and nothing asserted that a cross-origin request is rejected. The probe was
  run before the guard assertions on purpose: if even a bare request were
  refused, every 403 below would prove nothing.

### Fixed

- `tests/run_all.sh` had a `pkill` to reap the `--watch` service that could
  never match anything: the process is started as `./bin/lume --watch <script>`,
  and the pattern it searched for put the script path immediately after the
  binary, with `--watch` in between. Only the port-based cleanup trap had ever
  done the work; the `pkill` line pretended otherwise.

- `tests/run_all.sh` used one fixed `/tmp` path per service and fixed ports
  8996/8997/8999, so a second suite (or a stale instance from an interrupted
  run) could collide with it. The scripts now come from `mktemp` and each port
  is picked by probing for a free loopback port, falling back to the old fixed
  numbers where `lsof` is unavailable.

- `examples/native-fact.lume` and `examples/native-bench.lume` were not in
  `EXAMPLES`, so `make check` and `make asan` type-checked every other script
  but not the two that exercise the `--compile-*` emitters. They are in now.
  `Makefile`'s `.PHONY` was also missing 18 targets, including the whole
  `native-*`/`crypt-test`/`modules-*` set.

### Changed

- Documentation corrections, all found by reading the docs against the code:
  `docs/DEVELOPMENT.md` counted the smoke tests as "53 check + 11 reject"
  (they are 78 + 15, plus 7 module cases) and pointed at
  `scripts/gen-builtins.py`, which lives at
  `editor/lume-vscode/scripts/gen-builtins.py`; `docs/ARCHITECTURE.md` said
  `make invest` allows 11 tools when `INVEST_TOOLS` lists 14 (the three
  `sql_*` read channels were missing); `docs/LUME.md` still documented
  `mcps()` as returning `null` for a missing catalog, but `tests/smoke.c`
  pins it returning an empty list — `null` is what made the hub pages throw
  on `data.mcps.length`. The same file's builtin table was missing the whole
  numeric family (`abs`/`sqrt`/`exp`/`log`/`ln`/`pow`/`floor`/`ceil`/`round`/
  `min`/`max`) along with `push`/`put`/`replace`/`strftime`/`try`; they are
  documented now, with the shapes the implementations actually have —
  numeric builtins accept only real numbers and always return float,
  `push`/`put` return the container itself, `strftime` takes the format
  first, and `try` takes a function and hands back `{ ok, err }`.

### Added — the privacy guard can now ask the remote

- **`scripts/check-tree-privacy.sh --remote <sha> [<sha>...]`.** A force-push
  only makes a leaked blob *unreachable*; GitHub keeps it readable in its
  object database until its own GC runs, and there is no probe that separates
  "gone" from "private repo" — an unauthenticated request answers 404 for both,
  which is a false negative, not a clean verdict. The new mode asks with a
  token and looks at `has("content")`: every probe gone is rc=0 ("may flip
  back to public"), any probe still present is rc=1 ("stay private"), and a
  probe that cannot be read downgrades to a warning **and still fails**, so
  "could not tell" is never read as "clean". It pre-flights
  `gh api repos/<owner>/<repo>` first, so a dead `gh` fails loudly instead of
  being mistaken for a passing check.

### Fixed — the filesystem unit test depended on the working directory

- `tests/smoke.c` probed `files("docs")` and `read_file("docs/LUME.md")`,
  which pinned the case to the repository root: started from anywhere else the
  listing is empty and the suite reported "128 tests, 1 failed" even though
  the builtins were fine. CI stayed green only because it runs from the
  checkout root. The fixture is now built under `/tmp/lume-smoke-files` (the
  way the other filesystem cases already do) and addressed by absolute path,
  so the verdict holds from any cwd — verified from the repo root, from
  `tests/`, and from `/tmp`.
- `.gitignore` ignored `/.data/` but not `tests/.data/`, which `smoke.c`
  leaves behind whenever it runs from anywhere but the root, so `git status`
  grew a stray untracked directory right after a test run.

### Changed — the README no longer claims the host tree's capabilities

- `## SQLite support` and `## Text2SQL` read as features of this tree while
  both in fact describe the host `lume` tree. The warnings were already there,
  but the headings themselves carried the wrong claim. They are now
  `## SQLite support (host tree only — not in this one)` and
  `## Text2SQL (host tree only — not in this one)`.
- `README.zh.md` gained the Chinese versions of `## Containers`,
  `## SQLite support` and `## Text2SQL`, so the two READMEs carry the same
  sections again.

### Fixed — the Linux CI legs, which had never passed

- **`--compile-llvm` asked for a large code model on a PIE link.** On x86-64 a
  large code model cannot be combined with PIC, so the backend fell back to
  absolute addressing and the relocations landed in the read-only text section.
  GNU ld says so —
  `warning: relocation in read-only section '.ltext'` plus
  `warning: creating DT_TEXTREL in a PIE` — and `tests/native_backends.sh`
  reads *any* stderr from an emitter as a failed emit, so `test (ubuntu-latest)`
  and `asan (ubuntu)` went red on objects that linked and ran correctly. ld64
  is silent, which is why `test (macos-latest)` and every local run stayed
  green on the same source. `make_target_machine` now asks for
  `LLVMRelocPIC` + `LLVMCodeModelDefault`: PIC because clang links that object
  into a PIE by default on Linux, so a static relocation model would be the
  wrong answer even without the warning.
- **`make pack` shelled out to `strip -u -r`, which is an ld64 spelling.**
  GNU binutils has no `-u`; it printed its usage and exited 1, so
  `pack (ubuntu)` died at the strip step and `packcheck` never ran. The flags
  are now chosen per platform — `-u -r` on darwin, `--strip-debug` elsewhere.
  Both mean "drop the DWARF that carries the build directory, keep a loadable
  binary", and `packcheck` is what proves it either way.
- The smoke-suite size quoted in `README.md` (128), `README.zh.md` (113) and
  `docs/DEVELOPMENT.md` (128) had drifted from the suite's actual 134. All
  three say 134 now, and `docs/DEVELOPMENT.md` records where the number comes
  from, since nothing tests it.

## [0.2.0] - 2026-09-25

### Security

- `env()` masks credential-named environment variables (`*API_KEY`, `*TOKEN`,
  `*SECRET`, `*PASSWORD`, `*CREDENTIAL`, case-insensitive, word-boundary
  match) from `.lume` scripts — they read as `null`. The runtime still reads
  them itself; config-like names (`HTPASSWD_FILE`, ...) are unaffected.
- Startup prints a one-time stderr WARNING when the server binds a
  non-loopback address with Basic Auth off, noting that /chat, /dsl and the
  SQL data behind them are reachable by any host that can reach the port.
- Session retention is configurable via `SESSION_TTL_DAYS` (default 30,
  `0` disables the sweep; memory.json is never pruned).
- README/README.zh.md document the privacy posture: chat + SQLite schema ride
  along to the configured `LLM_API_URL` endpoint; `.lume` files are trusted
  code (only run authored/audited scripts); data backup/deletion is
  user-managed.

### Changed

- /dsl page UI overhaul: sections now render as cards, a status badge
  shows data readiness / row count / errors, tables get styled headers,
  zebra striping, right-aligned numeric columns and monospace symbol
  cells, and the code sample sits in a proper code panel. The page's
  Tailwind utilities were previously never emitted because `dsl.tsx`
  was missing from `app.css` `@source` — tables and the bare `<pre>`
  had no styles at all; the source list now includes it.

## [0.1.2] - 2026-09-25

### Added

- `for` loops in both forms: C-style `for (init; cond; incr)` (all three
  optional) and iteration `for (x in xs)` / `for (let x in xs)` over a
  list's items or a map's keys (sorted). `for (;;)` is an infinite loop.
- `break` / `continue` in `for` and `while`; both are statically rejected
  outside a loop (typechecker tracks loop nesting, function-local).
- Collection builtins: `range(stop)` / `range(start, stop, step)`,
  `map(fn, list)`, `filter(fn, list)`, `reduce(fn, list, init)` — the fn
  forms accept named functions and lambdas and run through the shared call
  machinery (GC-rooted, `return`/`?` unwinding intact).
- Adjacent string literals merge JS-style: `"a" "b"` is `"ab"` (merged at
  parse time pre-escape, so `\n` / `\"` inside either part keep meaning
  across the join). Non-string neighbours still error as before.
- SQL builtins: `sql_query(sql[, params])` / `sql_query(path, sql[, params])`
  (read-only SELECT returning a list of row maps) and `sql_write(sql[,
  params])` / `sql_write(path, sql[, params])` (guarded INSERT / UPDATE /
  DELETE with mandatory WHERE, or CREATE TABLE for a new table; returns
  affected rows, 0 for DDL). `params` binds `?` placeholders via
  sqlite3_bind_* — values never enter the SQL text, so the guardrails see
  only the statement skeleton and injection through a parameter value is
  impossible (quotes, `;`, `--`, DDL keywords in a value are inert). The
  single-statement checks, SELECT-only read and write allow-list are shared
  with the agent chat tools via agent-httpd's exported `sqlite_query_json`
  / `sqlite_write_exec` — one guardrail implementation, no duplication. The
  default database comes from env `SQLITE_DB`; an explicit path opens any
  SQLite file (read-only for queries).
- Typecheck: un-annotated list literals may mix element types (element
  type falls back to `any`), needed for heterogeneous `?` parameter lists
  like `[3, "c"]`; annotated lists stay homogeneous.
- `sqlite-write` example grows a /dsl demo page (chat UI gains a DSL entry
  in the shared nav) and the page migrated from Lume SSR to a React
  client that fetches the `/dsl/data` JSON API.

### Changed

- Release assets are now tarballs (`lume-<os>-<arch>.tar.gz`) containing
  `bin/lume` plus the web UI (`www`, including the esbuild bundles that
  are gitignored in the repo), `examples/`, `docs/` and the READMEs —
  a bare binary alone cannot serve /chat /dsl because the docroot
  resolves `./www` from the working directory. Each tarball ships with a
  `.sha256` sidecar; `install.sh` downloads the tarball, puts the binary
  in `~/.local/bin` and the web UI/examples/docs in `~/.local/share/lume`.
- `install.sh`: installs from the tarball (needs `tar`), still honors
  `LUME_VERSION` / `LUME_PREFIX` / `LUME_SHA256` (checksum now applies to
  the tarball).

## [0.1.1] - 2026-09-25

### Added

- `--help` / `-h` CLI flag: prints usage and exits 0 (previously fell into
  the unknown-flag error path and exited 2).
- One-command installer `install.sh`
  (`curl -sSfL https://raw.githubusercontent.com/erishen/lume/main/install.sh | sh`)
  — downloads the platform binary from GitHub Releases to `~/.local/bin/lume`;
  `LUME_VERSION` / `LUME_PREFIX` / `LUME_SHA256` overrides.
- Prebuilt binaries for all four platforms (linux-x64/arm64, darwin-x64/arm64)
  as `lume-<os>-<arch>`, built by a GitHub Actions matrix and attached to the
  release on tag push; the same matrix runs on push/PR to catch cross-platform
  build failures before a tag is cut. (darwin-x64 was delayed on 0.1.1 by a
  starved macos-13 runner and shipped with 0.1.2 via cross-compilation.)

## [0.1.0] - 2026-09-24

Initial release — a self-contained agent DSL server (one C11 binary serving
static sites, JSON APIs, SSE chat, agent tools and SSR pages).

### Added

- Lume DSL: lexer/parser/type-checker/tree-walking interpreter with `el()`
  and `html()` page builders, agent-httpd bridge (HTTP, chat SSE, MCP, tools).
- Product API (`iquest`): weekly report archive + approval settings endpoint
  with whitelisted keys, same-origin CSRF guard, atomic .env write-back.
- Native SQLite: `sql_query` (read-only, physically enforced via
  `SQLITE_OPEN_READONLY`), opt-in guarded `sql_write`, `sql_tables`,
  `sql_schema` — no Python, no MCP stdio process.
- Text2SQL: live schema + data discipline injected into the chat system
  prompt (DataPulse-style describe), cached by db mtime.
- `lock_file(path, wait_ms?)` / `unlock_file()` built-ins: flock-based
  advisory lock (auto-released on process death), serializing invest ledger
  read-modify-write across workers.
- `write_file` is atomic: payload goes to a sibling `.tmp.<pid>` file renamed
  over the target; created files are `fchmod`'d 0600.
- invest weekly reports get a `YYYYMMDD_HHMM` timestamp — same-day
  regenerations no longer overwrite each other.
- Kubernetes manifests (`docker/k8s/`): invest / hub deployments with Basic
  Auth from a Kubernetes Secret.
- Six examples: demo / hello / invest / hub / lang-basics / sqlite-write.
- Tooling: `make dev|invest|hub|demo-sqlite|test|check|clean`, `make asan`
  (ASan/UBSan), `make vsix` (VS Code extension pack), `make ui` (esbuild /
  Tailwind frontend bundles).
- CI: GitHub Actions (test + asan) with agent-httpd vendored as a git
  submodule pinned by gitlink.
- Docker: two-stage image rebuilding the C binary inside a Linux container;
  compose stack for invest / hub behind Basic Auth.

### Changed

- `examples/invest.lume` and the frontend report/date handling accept the new
  timestamped report names.
- invest default tool whitelist is read-only; `sql_write` is opt-in via the
  whitelist (`HARNESS_TOOLS_ALLOW`).

### Security

- Same-origin guard covers **GET and POST** product endpoints
  (`/api/reports`, `/api/reports/*`, `/api/settings`): cross-origin browser
  reads are rejected 403 (DNS-rebinding style theft), `Origin: null`
  rejected, origin-less curl / local scripts still work.
- invest server binds `127.0.0.1` by default (loopback only); dead
  `write_file` entry removed from the invest tool allow-list; cross-border
  data disclosure banner + privacy policy added to the settings page.
- Settings endpoint redacts `env_file` and upstream URLs (`configured`/`null`),
  provider control characters rejected (400), write-back refused when
  `IQUEST_ENV_FILE` is unset.
- Portfolio ledger write path: per-process flock + atomic rename; load
  failure prints an explicit corruption warning instead of silently
  continuing with an empty ledger.
- Private data permissions: `.data` 0700 via `mkdir`, mcp config written with
  `fchmod 0600`, session files 0600, access logs never record query strings.
- Frontend has zero third-party calls; `read_file` jailed to web root; MCP
  children get LLM keys unset; sessions TTL 30 days.

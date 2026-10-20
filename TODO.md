# TODO

Priorities reflect the 2026-10-15 project review, after the v0.7.0 CI
closure (asan zero-leak on Linux, Windows CI green without libgnurx).
Order: highest value per unit of risk first.

## P1 — Parser fuzzing (libFuzzer)

The lexer/parser edge cases are currently covered only by 74 hand-written
negative fixtures. ASan already ships in this repo's toolchain, so the
incremental cost of fuzzing is small.

- [x] Add a fuzz entry point: `LLVMFuzzerTestOneInput` feeding arbitrary
  bytes to the lexer → `parse_program` → orphan release path, plus the
  type checker; no backend emission (keep the corpus small and the
  target fast). — `src/fuzz_target.c`, with a standalone driver shape
  (`-DLUME_FUZZ_DRIVER`) so any compiler can smoke-run the corpus.
- [x] Wire a `make fuzz` target (`-fsanitize=fuzzer,address,undefined`),
  seeded from `tests/*.lume` and the `-bad/` fixtures. — `make fuzz`
  (Linux clang; `-fsanitize=fuzzer` rejects the Windows GNU targets,
  so Windows runs the driver shape instead — landed 2026-10-20 as
  `make fuzz-driver`: the standalone `-DLUME_FUZZ_DRIVER` shape (any
  `$(CC)`, mingw gcc included) replays all 92 tests/ seeds through
  lexer → parser → checker in one process, wired into windows.yml).
- [x] Decide CI policy: a short 60s fuzz leg in the `asan` job, or
  local-only. — Promoted 2026-10-18 as its own `fuzz` job running
  `make fuzz-ci` (deterministic -runs=1 seed replay + 60s budgeted
  campaign, artifacts uploaded on failure); first run green. The old
  "30-minute run still needs a Linux host" note is moot: campaign 10
  (see Done-when) completed the budget on an arm64 Linux container.
- [x] Any crash found lands in `tests/*-bad/` so the regression stays
  covered. — The first Linux campaigns found three, all fixed with
  fixtures under `tests/fuzz-bad/`: (1) tuple-literal error paths
  `node_free`'d journaled nodes — double-free at the parse sweep
  (crash input `(http_,`); (2) `parse_params` failure paths dropped
  strdup'd parameter names — 6-byte leak behind the same input;
  (3) `ck_put_pat_binds` walked to a caller-supplied bindc that the
  record-only validators could leave longer than `binds[0]`/
  `vfield_types[0]` — heap-buffer-overflow, reproducible today from
  the seed corpus via `match2-bad/iflet-binds.lume` and
  `enum-bad/wrong-binds.lume`. Campaigns 6-9 raised the tally to
  eight: (4) match/if-let mid-construction `MatchPat` arrays stranded
  (free_pat_array); (5) enum variant slots not published before their
  payload parse (in-flight arrays outlived the sweep's vcount walk);
  (6) parse_patterns' own fail exit skipped the in-flight slot; (7)
  that fix's realloc-failure edge (claimed-slot bound, `1ff919a`);
  (8) unbounded descent nesting — ~500 parens ran the fuzz thread's
  stack out, fixed with PARSER_MAX_DEPTH 200 + the expression-chain
  and parse_block depth guards (`c4d1906`). All carry fuzz-bad
  fixtures; the 96-seed corpus replays clean on the instrumented
  build.
- [x] The `make fuzz` target built the core objects WITHOUT coverage
  instrumentation (`$(CFLAGS)` has no `-fsanitize=fuzzer`): libFuzzer
  ran blind — a 54M-input "clean" 30-minute report was `cov: 5`, i.e.
  no fuzzing at all. Fixed 2026-10-18 with `build-fuzz/` objects on
  `-fsanitize=fuzzer-no-link`; any pre-fix fuzz number in this repo's
  history is void.

**Done**: 2026-10-18, campaign 10 — 1,226,047 inputs in the full
1801s budget, zero crashes/leaks/timeouts, 12,279 new corpus units,
peak RSS 504 MB. The discover-fix-campaign loop stops here (by
consensus): patrol moves to the CI `fuzz` job, follow-ups are filed as
**P8** below.

## P2 — Arena allocator for the AST

The 2026-10-15 leak fixes (N_LET_EXPR subtrees, truncated scope tables,
throwaway map-put results) are all instances of one structural problem:
manual per-node freeing that every new node type and scope rule must
remember. An arena removes the whole class instead of patching points.

- [x] Design first: arena lifetime = one compilation unit
  (`parse_program` .. end of backend emission). `nalloc` redirects to
  the arena; `node_free` / `node_free_kids` / `node_free_own` and the
  orphan journal collapse into a single arena reset. — Design recorded
  2026-10-19 in `docs/ARENA.md` (lifetime map, in/out of scope, staged
  rollout; Type graph stays on `type_release_all` in v1).
- [x] Enumerate the lifetime hazards before writing code: AST nodes are
  shared across phases (checker, codegen scan, both emitters read the
  same tree) — the arena must outlive all of them. Cross-check every
  phase that today calls `node_free` on a subtree it claims to own.
  — Done 2026-10-19: ten hazards in `docs/ARENA.md` (source-buffer
  borrows, unjournal dances, parse-error abandonment, the mixed Type
  vectors, checker scope borrows, allocation past parse via the
  signature pass, loader's many-trees model, request-time interp reads,
  host sync debt, the four-copy Asgs trap).
- [x] Stage 1 (2026-10-19): `src/arena.c` chunked bump allocator; `nalloc`
  and all parser strings redirected; orphan journal + node_free family
  deleted; the five tree-death sites reset the arena. `type_func` copies
  the caller's param vector; the loader's import node borrows the canon
  path and `loader_free` frees `import_paths` itself. Gates green:
  `make test`, `make asan` (182 tests, zero leaks), 94-seed fuzz replay
  via the standalone `-DLUME_FUZZ_DRIVER` driver; host synced (synced
  six + arena files copied, four pinned files hand-merged).
- [x] Stage 2 — checker coupling (2026-10-19): verified, folded into Stage 1.
  The mixed-vector frees/NULL-stamps died with the node_free family and
  `type_release_all`'s skip degenerated to the unconditional
  `free(t->types)` in the same commit. Hazard 5 verified by inspection:
  `scope_put`/`ck_free`/EnumDef shells own their malloc'd copies and
  teardown touches no arena memory (`make asan` agrees).
- [x] Stage 3 — codegen strings (2026-10-19): Asgs/arm_scope name+slot
  strings moved to the arena in all four copies in one commit
  (codegen.c, llvm_codegen.c, codegen_scan.c, llvm_cg_stmt.inc);
  `asg_truncate`/`arm_scope_truncate`/`asgs_free`/`arm_scope_clear`
  degenerated to count/vector-shell frees. The LTys tables (globals,
  sigs, structs) keep their malloc regime by design. Gates green:
  `tests/native_backends.sh` three legs, `make asan` (182 tests, zero
  leaks), `make test`, fuzz seed replay.
- [x] Gate on the existing suite: `make asan` stays at zero leaks (now
  proving the arena, not the frees), native-consistency unchanged, all
  `*-test` targets green.

**Done when**: the free-path patches from `ecd7afc`/`7e5d2f8` can be
deleted without the suite noticing. — **Satisfied 2026-10-19**: every
free those patches introduced is either gone with the node_free family
(ecd7afc: N_LET_EXPR subtrees, local-fn cname, renamed callee names) or
degenerated to a count reset (7e5d2f8: the four truncate helpers). The
only survivors are emitter-owned malloc temporaries (`free(r.v)` for
throwaway map-put results and friends), which were never arena memory
and stay correct. P2 is complete.

## P3 — Native-backend test leg on Windows CI

Windows CI currently only builds and runs `check` + Run-hello; the
native backends have zero Windows coverage, so the vendored regex's
wchar assumptions and the mingw link path are untested on the real
platform.

- [x] Add a smoke leg to `windows.yml`: interpreter + `--compile-text` +
  `--compile-llvm` on a Run-hello-level script (the MSYS2 clang
  dependency already exists in the toolchain group). — The `--compile-text`
  (`make native-text`) and default-backend (`make native`) legs were already
  in place from the Windows native port; `check` + hello cover the
  interpreter side.
- [x] Include one `str_match`/regex positive case to exercise the
  vendored `src/compat/regex` on Windows for real. — `examples/regex-smoke.lume`
  (2026-10-16) + a Windows CI interpreter leg. Note: `str_match`/`regex` are
  interpreter-only builtins (SPEC §6.2 intentional boundary — the match-list
  shape has no native emission path), so the leg exercises the vendored regex
  through `native_str_match` at runtime; the mingw link path is covered because
  the regex is compiled into `lume-core.exe` itself.
- [x] If LLVM emission proves heavy on the runner, land the interpreter
  + text legs first and track the llvm leg separately. — Moot: all three
  legs were in place and run green on every push since (2026-10-19:
  run 39216687894 success @ 9fd6d7b).

**Done when**: the Windows workflow runs the three legs green. ✅ 2026-10-19.
2026-10-20: windows.yml additionally runs `make native-consistency`
(tests/native_backends.sh — interp + text + libLLVM legs each diffed
against the full tests/native-consistency.expected, the 18 statistical
distribution prints included, plus the exit-status/null/`?` assertions)
and `make fuzz-driver` (the P1 Windows seed replay).

## P4 — Later / larger

- [x] Cross-backend `json(str)` parsing: needs a tagged-any value (2026-10-16: rt.c `lume_json_parse` + slot helpers; codegen json/get/str/len/for-in on any, verified interp = IR-text native on nested containers; `--compile-llvm` (llvm_cg_*.inc) wired 2026-10-16 too (json parse / str / len / get / for-in / infer), pending real-machine check on the Linux copy; try() failure path fixed 2026-10-16 on the text backend: rt.c `lume_map_put_slot` (tag-dispatched {ok} writer) + codegen_call.c try TY_ANY branch + json() failure routed through `lume_error_set` (try() fills {err: "json parse error..."}, outside try() prints and exits like the interpreter) — verified `try(() => json("{bad"))` => `{"ok":null,"err":"json parse error (at offset 1)"}` and `a={"ok":3,"err":null}`; LLVM side still pending: try() itself is unimplemented on the LLVM backend (no error_begin/end/branch emission; json failure there serializes as null — noted at llvm_cg_call.inc, land together with branch emission); index / dflt-less get verified identical interp vs IR-text 2026-10-16 — and closed 2026-10-16: `j["k"]` subscript on an any value now lowers on the text backend (rt.c `lume_slot_at_key` / `lume_slot_at` + `lume_index_error` failure branch in codegen_expr.c cg_index) and dflt-less `get(j,k)` reads the raw slot (`lume_slot_get_any` — absent key is a null slot, not an error); `str()` of a string slot is now the bare string (`lume_slot_str_scalar`) while `stringify()` keeps JSON quotes (`lume_slot_str`), matching the interpreter. LLVM backend mirrors: any-subscript branch (llvm_cg_expr.inc), dflt-less get (llvm_cg_call.inc — merged with 2ebe3cf's three-arg `lume_slot_get_any`, both backends share one call shape), str scalar (llvm_cg_call.inc); try() itself landed 2026-10-16 (llvm_cg_call.inc: error_begin/end bracket + branch + slot-ized ok, llvm_cg_scan.inc: {ok,err} inference, json() failure routed through `lume_error_set` like the text emitter); interp json error message unified to "json parse error (at offset N)" (value.c) matching both native emitters; cross-backend regression suite added at tests/llvm-json/ (trymin/jedge/jt2b/jfail + .expect + run.sh) — the whole LLVM side (json three-piece + try) verified on a real Linux machine 2026-10-19 (OrbStack Docker, clang 18: clean-source `make` + `--compile-llvm` + `tests/llvm-json/run.sh` all green — trymin/jedge/jt2b three-backend identical, jfail exit-1 leg included; the libLLVM leg genuinely ran, not skipped))
  representation in the native backends — a real project, not a patch.
  SPEC §"intentional boundaries" documents the current interp-only rule.
- [x] Ecosystem: README first-screen example pass, one non-toy example
  program, then exposure. No marketing before P1-P3 close. — README/README.zh.md
  first-screen example (2026-10-16, verified on all three backends) +
  `examples/stats.lume` (statistics toolkit: typed functions, HOFs, closures,
  struct values — `make check` covers it via EXAMPLES).
- **Design boundary (2026-10-19): `server{}`/listen is already a lume-core
  builtin, not a host-only capability — do NOT file "add listen to core".**
  `bridge_serve.c` is unconditionally compiled into the core main Makefile;
  the `lume-core` binary ships `server{}` (bind/accept/route/docroot/sql)
  since 0.7.0, and **lume-nsgm is the live proof**: its generated
  `gen/app.lume` runs as plain `lume-core app.lume` listening on :18081, no
  1.7M agent-httpd host binary involved. So the family's run-modes are four:
  host `lume` (core + LLM tools/SSE/MCP) · **core-only** (nsgm, a bare
  server runtime, few-MB) · dual-binary (host routes + core as authoritative
  engine, e.g. medical-stats) · pure-interpret (fuzz/selfcheck/generators).
  Consequence for cross-agent comms: any repo can be a comms endpoint as a
  **core-only serve node** (`http_post(peer, allow_private:1)` out, `server{}`
  in) without inheriting the 1.7M host. Nothing to build; this note exists so
  we stop re-deriving "listen needs to be added" and instead pick the lightest
  endpoint shape.
- [ ] **P4 · AI self-verification loop (filed 2026-10-20, user goal: "turn Lume into an AI-self-verifying language").**
  Goal: any LLM-generated `.lume` program must pass an automated gate before it enters
  the regression corpus — "AI writes, machine verifies."
  Existing base (do NOT rebuild):
  - `make native-consistency` — three-backend diff (interpreter / IR-text / libLLVM)
    against `.expected`; clang-rejected IR counts as fail. **This is the acceptance
    oracle.**
  - `make fuzz` (libFuzzer, seeded from `tests/*.lume` + 8 fixture classes in
    `tests/fuzz-bad/`) — regression safety net. Currently human-written seeds.
  - `make backend-parity` — static AST-node diff between `codegen.c` and
    `llvm_codegen.c` case labels.
  Gap (what "AI self-verification" adds):
  1. **LLM-generated `.lume` as fuzz/consistency seed**: wire a CI/CD leg that
     takes LLM output (e.g. from a ReAct/planner agent, or a scripted
     `prompt → .lume` generator) and runs it through `native-consistency`
     + `fuzz`. Pass → commit to `tests/`; fail → reject + log the counter-example
     back to the LLM for retry. This closes "AI writes, machine verifies."
  2. **Optional / open: an `assert_semantic()` DSL builtin** — let `.lume` code
     declare invariants (`assert_semantic(x => x >= 0 && x <= 1)`) that a
     *host-side* validator can check against LLM output without running it,
     turning the program into its own spec. Keep this OUT of the core (same
     host/interpreter split as above). Open question, not committed.
  Scope guard: do NOT add a "LLM builtin" into the core interpreter (it stays
  host-independent / no-network). The LLM step lives in the **host** (agenthttpd's
  tool dispatch) or in an external CI script; core only does the deterministic
  verify (native-consistency + fuzz). Same boundary as `server{}`: LLM in, verify out.
  — Core gate landed 2026-10-20: `scripts/verify-ai-lume.sh` + `make ai-verify`
    (AI_VERIFY_FILE=...). An AI program has no `.expected`, so the gate's oracle
    is backend equivalence: type check, then the interpreter / IR-text / libLLVM
    legs must agree byte-for-byte on stdout (CRLF-normalized; any compiler
    diagnostic rejects), at least two legs required to judge, plus the optional
    `out/fuzz-driver` crash leg. Exit 0 = pass, 1 = reject (counter-example on
    stderr, ready to log back to the LLM), 2 = gate cannot run. Verified on
    Windows (interp + text legs, libLLVM skips without llvm-config): 4 example
    programs PASS, all 8 enum-bad fixtures rejected. Still open: the host-side
    LLM → `.lume` step and the CI/CD wiring that feeds its output into
    `make ai-verify`; `assert_semantic()` remains an uncommitted open question.

- [ ] **P4 gap · `server{}` + `env()` is interpreter-backend-only; LLVM leg
  not aligned (blocks three-backend equivalence for serve programs).** nsgm's
  `gen/app.lume` reads `env("PORT"/"HOST"/"WORKERS")` inside `server{}` and
  its Dockerfile forces `HAVE_LLVM=0` because `env()`-in-`server{}` only takes
  effect on the default (interpreter) backend today. When P3 (three-backend
  equivalence) hardens, add a serve+env regression leg (interp / text / LLVM
  must all honor `env()` inside `server{}`); until then, serve programs that
  read env at startup are interpreter-pinned. Filed 2026-10-19 from nsgm's
  `HAVE_LLVM=0` workaround.
- [x] **P4 · Statistical distribution builtins (filed 2026-10-19, driven by
  medical-stats p-values; closed 2026-10-20).** medical-stats' next step is
  descriptive stats → inference: p-values for the correlation matrix,
  group-baseline t/χ², and regression coefficient t/Wald tests. The current
  math builtins (`abs/sqrt/exp/log/ln/pow/floor/ceil/round/min/max` +
  `pi`/`e`, see `builtins_math.c`) provided none of the needed CDFs. Added,
  via C `math.h` erf/lgamma + self-authored series / continued fractions
  (Temme / modified Lentz — the same house style as the engine's hand-written
  `normal_quantile`), registered as three-backend-consistent builtins:
  - `normcdf(x)` / `erf(x)` — normal CDF (correlation r→t tail, z-test)
  - `tdist_cdf(t, df)` (or `ibeta`) — t tail (correlation significance,
    linear-regression t, two-sample t)
  - `chisq_cdf(x, df)` (or `igamma`) — χ² (categorical group comparison,
    logistic Wald/LR)
  - `gamma` / `lgamma` / `igamma` / `ibeta` — shared implementation base
    (regularized incomplete Gamma/Beta)
  Implementation: rt.c `lume_bi_*` (native twins for text/LLVM emitters,
  `emit_builtin_declares` / the LLVM `BUILTINS` table spell their
  signatures), builtins_math.c interpreter wrappers (numerical core
  duplicated byte-for-byte from rt.c — the interpreter binary does not link
  rt.c, same split as abs/sqrt/...), interp.c registration, typecheck.c
  `LUME_BUILTIN_NAMES`. Regression: tests/native-consistency.lume +
  .expected (18 distribution prints) run by tests/native_backends.sh across
  interp/text/llvm; the numbers were cross-validated against known critical
  values (z 0.975, t10 0.95/0.975, χ²₁/₂/₃ 0.95) and the binomial identity
  I_x(5,5) = Σ C(9,k) x^k (1-x)^(9-k) to ~1e-7.
  Consumption side (pure `engine/*.lume` functions) is specced in
  `work/knowledge/medical-stats/TODO.md`; now that core ships these, that
  project can wire the p-values without hand-rolling unstable distribution
  code in the DSL.

## P5 — Found 2026-10-17 (both closed same day)

- [x] Route table caps raised 32 -> 128 (`556cbf3`): interpreter
  `MAX_AL_ROUTES` (lume.h) and native `NATIVE_ROUTES_MAX`
  (bridge_native.c) in lockstep — one-sided raises would mean "registers
  fine interpreted, silently loses routes once compiled". A full table now
  reports `route table full (N routes, max M)` (interpreter; -2 return
  distinct from bad-arguments -1) or warns on stderr (native; previously a
  silent drop, surfacing only as a 404 on a handler that exists in source).
  Regression: `tests/route-cap-srv.lume` + `make route-cap-test` (40 routes,
  wired into `make test`). Found via lume-nsgm's PayPal variant (31 routes:
  product 10 + order 10 + pay 4 + shop 6 + `/` 1; a third module would hit 43).
- [x] Text backend emitted untyped bool constants in server map
  serialization: a handler returning `{ok: true, ...}` made codegen_stmt.c's
  literal fast path emit `call void @lume_srv_json_bool(i8** %p1, i64* %p2, 1)`
  — a bare `1` is invalid IR, clang rejects the .ll ("expected type"), so the
  text backend could never serve a map with a bool field (the generic path
  below already zext'd to i32; only the fast path forgot). Fixed by typing the
  constant `i32 1` / `i32 0`; covered by route-cap-srv.lume's `{ok: true, n: N}`
  returns, verified through `--compile-text` end to end. Emission dated to
  `00204ae` (first server{} leg) — latent since then. LLVM side has its own
  emitter and was not affected.

## P6 — Cross-statement transactions (design recorded, deliberately not started)

Motivation: lume-nsgm's PayPal variant inserts the order and decrements
stock as two statements; a crash in between leaves a pending orphan
order (their S2). Already mitigated by the CAS-guarded UPDATE
(`stock = stock - 1 WHERE id=? AND stock>=1`, atomic as a single
statement), `busy_timeout` (`178501c`), and insert-first + compensating
DELETE — sufficient at shop scale, which is why this stays filed
instead of started.

Architecture-level, not a builtin addition — it conflicts with three
foundation designs at once:

- **Stateless builtin contract**: `sql_write` opens → prepares → steps →
  closes within one call; the signature carries no handle, so
  transaction state cannot survive a call boundary. Even a whitelisted
  `BEGIN` would be a silent no-op — `sqlite3_close()` rolls back an
  open transaction, so BEGIN/INSERT/COMMIT written as three calls would
  auto-commit each and quietly lose atomicity (fail-loud rejection is
  the honest behavior).
- **Fork safety**: modules are parsed before workers fork; a
  module-level cached connection would be inherited by every worker —
  SQLite forbids cross-fork connection use (corruption class, not a
  perf question). Per-call open sidesteps the whole problem class.
- **Whitelist guardrail**: `sql_validate_write` guarantees "exactly one
  known-shape statement per call"; `BEGIN` is session state, so
  admitting it anywhere in that path breaks the invariant.

The four-layer surface if started:

- [ ] Value system: `sql_tx_begin()` returns a first-class handle —
  new OBJ type, GC finalizer (handle collected = rollback + close),
  value-semantics questions (does copying a handle copy the
  transaction? map round-trip?); closure capture of a handle would
  hit the known llvm capture gap.
- [ ] Connection management: per-worker cache keyed by path (never
  module-level — see fork safety); refcounts, cache bound, close
  timing (request end vs worker exit).
- [ ] Guardrails: dedicated builtin entry, never the `sql_write`
  whitelist; error semantics when `vm_set_error` fires mid-transaction
  (auto-rollback or leave open?).
- [ ] Three-backend parity: interpreter + text + llvm emitters,
  inference/boxing, `case-parity` + `native-consistency` gates — the
  repo's classic double-copy sync trap.

Gray zone — **done 2026-10-20**: write CTEs are whitelisted in
`sql_validate_write` (`WITH ... INSERT/UPDATE/DELETE`) — still one
statement, still stateless, guardrail-only; subqueries must be
SELECT/VALUES (read-only at any depth) or a RETURNING DML (top-level
WHERE required for UPDATE/DELETE), and the trailing statement passes the
exact bare-write whitelist. Collapses some multi-step units (not general
cross-call sequences). `0c44b90` (+ 17 CTE cases in tests/sqlite.lume).

Host-tree debt — **merged 2026-10-20**: `builtins_sql.c` is HOST_PINNED
(ownership fork, not visibility — the host version carries
portfolio-mirror policy on a `db_layer` abstraction), so `make
sync-lang` will never overwrite it; the write-CTE whitelist was merged
manually into the host (`agent-httpd src/agent/sqlite_tool.c
db_validate_write`, `cd94be1`), not copied by sync.

**Done when**: a lume-nsgm-scale workload demonstrates a real crash or
correctness need for cross-statement atomicity (not just purity), and
the value-system design clears the llvm closure-capture gap. (The gap
this gated on — closures capturing function locals on the llvm backend
— landed 2026-10-18, see P7; a transaction handle behind a closure
still hits the general capture machinery, and the top-level half of P7
would apply to it too.)

## P7 — LLVM closure capture (ported 2026-10-18) + top-level env write path (closed 2026-10-18)

- [x] The llvm backend emits phase-2 captures (2026-10-18): `cg_def`
      registers the signature pass's `cap_names`/`cap_types` before
      `scan_block` (at line 0, like parameters — a lambda's source line can
      be later than uses inside its own body) and materializes each into a
      typed slot pointer off the closure's `i8*` context parameter in the
      entry block (bitcast → GEP index → load → bitcast; the text backend's
      `%cap<i>` string convention becomes an `Asg.cap` flag in the typed
      IR-value table, and the entry alloca pass must skip flagged
      bindings); `cg_funclit` builds the i8* array of outer slot addresses
      at the creation site and hands it to `lume_closure_make`, rejecting
      capture-of-capture like the text emitter. The llvm leg of
      `native-consistency.lume` (which failed `cap_hof` at line 503 since
      the capture tests landed) is green; the three-way suite
      (`tests/native_backends.sh`) passes on macOS, Linux CI pending.
- [x] Top-level env access on the native backends (2026-10-18): the
      original diagnosis in this entry ("reads hit an uninitialized slot")
      was wrong on the read half — a body's reads of a top-level name
      already resolve through `gvar_find` to the module global and see
      rebinds. What was broken is the write path: scan's N_ASSIGN arm
      registers a frame-local shadow for a body that assigns a top-level
      name (intentional, matching the interpreter's fresh-frame-per-call
      rebinding) but the entry block never seeded that alloca from the
      lifted global — so the shadow's first read was uninitialized stack
      (`f2(1)` printed garbage where the interpreter says 2). Fixed by
      marking those registrations (text `Asg.shadow` / llvm `Asg.shadow`,
      `asg_push_shadow`) and seeding them from the `@lv_` global in both
      emitters' entry blocks plus the route-handler alloca pass (named
      structs skipped: reference-shaped globals). Pinned by the new tl8_*
      series in `native-consistency.lume` (+ its expected baseline):
      fresh-frame seeds, frame-local writes, shared-cell top level.
  - [x] Semantics decided: seed-from-global (capture-like freshness per
        call), not typecheck rejection — the reject branch would break
        scripts the interpreter runs today, host invest.lume among them.
  - [x] Landed in both native emitters (shared scan marks); the third
        backend needed no change.

**Done when**: the top-level shape is either three-way consistent or
loudly rejected. (The Linux real-machine half is closed 2026-10-18: the
llvm leg of `tests/native_backends.sh` runs green on ubuntu-latest CI
since `e817823`, and `tests/llvm-json/run.sh` passed 5/5 on
ubuntu:24.04 + LLVM 18 — clearing P4's pending note as well.)

## P8 — Fuzz follow-ups (filed 2026-10-18 at P1 closure, priority order)

The ten instrumented campaigns found eight memory/stack bugs, all
fixed with `tests/fuzz-bad/` fixtures. They cluster into two shapes —
an in-flight slot claimed before it is filled (enum variant,
parse_patterns variant, parse_params name vector) whose failure exit
walks only the completed count, and a manual `node_free` crossing the
orphan journal's sweep. CI (`make fuzz-ci`: deterministic seed replay
+ 60s campaign, artifacts uploaded) now patrols for regressions; the
items below close the loop the campaign loop left open.

- [x] **Host sync — the one item with a deadline.** All eight fixes
      live in the host `lume/lang/` copies of the parser trio
      (`parser.c`, `parser_expr.c`, `parser_stmt.c` — sync-lang carries
      them), and the host product runs production scripts on that
      parser today; the ninth touch-point is `typecheck.c`'s
      bind-overflow clamp, which is host_owned and needs a hand-merge.
      Sequence: CHANGELOG
      promote (Unreleased already carries 90+ entries incl. tonight's
      eight) → `git tag v0.8.0` → host `make sync-lang && make
      check-sync` → hand-merge `typecheck.c` (the
      `ck_put_pat_binds` triple bound) → host `make check`/`test` →
      push agent-httpd BEFORE the host repo (a dangling gitlink reds
      every CI job, learned the hard way 2026-10-18).
      — Closed 2026-10-19: v0.8.0 tagged at `436b744`; host pin bumped
      to `v0.8.0 @ 436b744` (`bcdccd0`); the parser trio was already
      byte-identical (the 09:11 sync carried all eight), the
      ck_put_pat_binds clamp was already hand-merged, and the real
      residual gap was the `ty_str` ring-slot size fix (`98ceb48`,
      hand-merged as `4232dd0` — `sizeof buf` = pointer size, 8, vs the
      ring slot's 128: type names truncated at 7 chars in diagnostics).
      Host `make check`/`test` green; agent-httpd gitlink `4060a068`
      verified = its `origin/main` before the host push.
- [x] Same-class audit (2026-10-18, clean): every claim structure in
      the parser proper was checked against its cleanup bound —
      `type_decl` field vectors and the `server{}`/`tool{}`/route
      builders all realloc-write-`count++` in one straight-line block
      after the last failure point, so they are structurally immune;
      `lf_*`/`sdef_*` arrays are checker-side and belong to
      `type_release_all`. The one live hazard in the family —
      `node_free_own` walking journaled children on a failed parse —
      is already pinned fatal-by-comment in `parse_program`; nothing
      tonight walks it.
- [x] fuzz-ci hang patrol: `-hang_threshold=10` on the random phase
      (2026-10-18) — `-timeout=10` catches a single slow run, the
      threshold catches inputs slow on every repeat; crash/leak/hang
      are now all CI-visible.
- [x] SPEC: `PARSER_MAX_DEPTH 200` recorded as a deliberate hostile-input
      boundary in the preamble's resource-limits note (2026-10-18), so
      the next review does not re-file crash-eb4ae2 as a gap.

## P9 — for-in over an inline temp value: heap corruption under agent-httpd (filed 2026-10-20)

Writing `for (x in get(load_xxx(), "key", [])) { ... }` — iterating an
inline expression chain whose only owner is the expression itself,
instead of binding it to a `let` first — crashes the host `lume` HTTP
stack after a handful of repeated requests. First observed in the
chinese knowledge project (`work/knowledge/chinese`, fix commit
`c10f23d`); the bug is presumably in shared code, not that project.

Evidence (all on host `lume` v0.7.3, agent-httpd :8095, sandboxed
macOS arm64):

- `GET /api/recite` whose handler contained two such loops returned
  200 on a single hit but **segfaulted the whole server silently on
  the 5th consecutive hit** (lsof empty, workers orphaned). No panic
  message: the master's stdout is block-buffered and lost on death,
  stderr carries only the startup WARNING.
- A "killer sequence" reproduced 3/3: any 10 data-loading GETs
  (poems/guwen/xuci/.../quiz/wrong/sick/synonym) followed by the
  recite hit → dead; replacing the 12th request with any other
  endpoint (health/synonym/xuci) → alive. `health`×11 + recite →
  alive. So the trigger needs both loop-toting handlers AND
  accumulated per-request state.
- **Bare interpreter does NOT reproduce**: a standalone script doing
  300 rounds of the same inline-temp for-in over 60 items
  (`lume-core script.lume`, this repo's binary) completes clean.
  Whatever dies is therefore an interaction between the interpreter's
  temp-value lifetime and agent-httpd's per-request lifecycle (arena
  reset / prefork worker reuse), not the plain eval path.

Workaround (applied, verified): bind the iteration target to a `let`
before the loop — `let items = get(load_xxx(), "k", []); for (x in
items)`. All 9 inline-temp loops in chinese/server.lume were converted
(fix `c10f23d`); after that, recite ×11 and the full 14-endpoint smoke
are green. The pattern in other handlers (`for (p in get(ok,
"results", []))` where `ok` is already a `let`) appears safe.

Steps:

- [ ] Build host `lume` with ASan (Linux container if needed; core's
      `build-asan` recipe shows the flag set) and replay the chinese
      `c10f23d^` server.lume: recite ×5 must crash under ASan with a
      readable report (use-after-free / heap-buffer-overflow expected).
- [ ] Shrink to a minimal server.lume (one route, one inline-temp
      loop, one loader) — keep it as a regression fixture if it fits
      the agent-httpd test layout.
- [ ] Locate the lifetime bug: likely candidates are (a) the eval of
      the for-in iterable expression not being retained across
      statements while agent-httpd resets a per-request arena, or
      (b) the temp value being allocated in a pool that a later
      request's setup overwrites (explains "crashes on a LATER
      request, not the first"). Check how interp holds the iterable
      vs. how the HTTP layer brackets evaluation.
- [ ] Fix + regression: if the fix lands in core (interpreter value
      lifetime), it flows to the host via sync-lang semantics — but
      note interp.c is host-owned (minijson include block), so a
      core-side fix must be hand-merged like typecheck_expr was; if
      it lands in agent-httpd (arena bracketing), the standalone
      agent-httpd repo is the canonical place.
- [ ] Done-when: under ASan, the killer sequence replays clean on the
      unfixed script shape; chinese's original handler works without
      the workaround; CI (three legs + fuzz) stays green.

## Intentional boundaries — do not file as gaps

These are deliberate design decisions; re-proposing them is noise
(kept here so future reviews don't re-list them):

- `m["k"]` scalar reads on typed bindings: use `get(m, "k")` on native
  backends (opaque anon struct).
- Local functions don't capture; same-name locals in one function are a
  compile error (§4.14 rules).
- Arrow lambdas accept `: T` on parameters but have no return-type
  annotation.
- `json(str)` parse direction was interpreter-only until tagged-any; 2026-10-16 native backends parse it through a `LumeSlot*` tagged-any (SPEC 865).
- `str_match`/`regex` are interpreter-only: the match-list element is an
  anonymous `{text,index,groups}` map with no native emission path (SPEC
  §6.2). Windows CI exercises the vendored regex through the interpreter
  (`examples/regex-smoke.lume`).
- Third-party package managers are not build dependencies: vendor small
  deps instead (musl regex precedent after libgnurx vanished from MSYS2).
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
distribution prints included, plus the exit-status/null/`?` assertions).
**Windows/MSYS .exe hazard — closed 2026-10-20**: Makefile's `TARGET` and
the fuzz-driver object used to default to extensionless names
(`bin/lume-core`, `out/fuzz-driver`), which MSYS bash refused to execute
(127 — Windows PE needs the .exe extension for CreateProcess). Both now
carry `.exe` on the Windows branch (IS_WINDOWS=1): `TARGET :=
bin/lume-core.exe`, `FUZZ_DRIVER := out/fuzz-driver.exe`, and
windows.yml's `./bin/lume-core` invocations were updated to
`./bin/lume-core.exe`. Verified locally in MSYS bash without any TARGET
override: `make all` produces `bin/lume-core.exe` and `make fuzz-driver`
replays all 100 seeds. (Diagnosis also surfaced a local-only PATH hazard:
D:\Software\mingw64\bin holds a libLLVM-23.dll whose dependency chain
(libffi-8/libxml2-16/libzstd) lives in the MSYS2 tree, so running LLVM-
linked exes requires /mingw64/bin first in PATH; CI's MSYS2 setup does not
have that split.)
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
- [x] **P4 · AI self-verification loop (filed 2026-10-20, user goal: "turn Lume into an AI-self-verifying language").**
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
  2. **Semantic layer (closed 2026-10-21, NOT a language builtin)** — the
     committed design is a static declaration contract checked without
     running the program (verify-ai-semantics.sh): `//@semantic <label>:
     <expr>` with defined-and-claimed rules and a pure-computation shape
     guard, folded into the gate as a pre-leg reject step. See the P4
     semantic-layer item above; this row is kept only as the history of
     the decision to keep the DSL OUT of the core.
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
    programs PASS, all 8 enum-bad fixtures rejected. CI wired 2026-10-20:
    `ci.yml` gained an `ai-verify` job (ubuntu, llvm-dev + clang) running
    `make ai-verify` over hello / native-fact / stats / lang-basics with the
    libLLVM leg live and `make fuzz-driver` built first — the two legs this
    Windows box cannot run (libLLVM compile, standalone crash path) are real
    CI legs. Windows wired 2026-10-20: `windows.yml` gained the gate legs
    (make ai-verify over hello + norm-stats) under MSYS2 — the first real
    bash run of `scripts/verify-ai-lume.sh` on Windows, libLLVM leg live via
    the MSYS2 clang group's llvm-config, so bash/CRLF/quoting compatibility
    of the script is CI-checked on both OSes. Seeds + docs 2026-10-20: `examples/ai-generated/norm-stats.lume`
    (plausible LLM program that must PASS — with an explicit `let p: float`
    annotation, because ordinary numeric builtins like `tdist_cdf` are NOT in
    the native backends' inference table, so an unannotated `let p = tdist_cdf(...)`
    is rejected and the counter-example teaches the LLM to annotate; the
    interpreter accepts both — backend equivalence is the stricter oracle),
    `tests/ai-bad/type-mismatch.lume` (must REJECT, string for float param),
    `docs/ai-verify.md` (gate usage + three wiring entry points + exit codes +
    scope guard). Scripted entry point landed 2026-10-21:
    `scripts/ai-verify-loop.sh` / `make ai-verify-loop` — asserts the
    expected verdicts over the must-pass / must-reject seed sets and
    appends every counter-example to `out/ai-rejects.log` (the payload to
    log back to the LLM for retry); wired into both CI jobs. Producer half
    closed 2026-10-21: `scripts/ai-gen-programs.sh` (scripted "prompt →
    .lume" generator: program intents + one template candidate each, with a
    manifest name|expected accept|reject|prompt) + `scripts/ai-produce.sh`
    / `make ai-produce` (run every candidate through verify-ai-lume.sh and
    assert the manifest verdict: accept → READY for tests/, reject →
    counter-example appended to out/ai-rejects.log, violation → exit 1) —
    eight accept-class candidates (stats / portfolio-sort / summary-stats /
    json-marshal / enum-match / range-sieve / sql-tx / str-match, all on the
    native backends' covered surface) and one deliberate reject
    (gen-bad-untyped-tdist: `let p = tdist_cdf(...)` missing `let p: float`
    — the statistical builtins are not in the native inference tables, and
    the stderr reason is the counter-example that teaches the LLM to
    annotate). Wired into both CI jobs after ai-verify-loop; the contract
    for swapping in the real host tool dispatch is the manifest file.
    Seed updated 2026-10-23: ninth accept candidate `gen-while-shadow`
    (while-condition reads the enclosing binding under a same-line body
    shadow, plus the flat-env if-block rebind — the exact shape the
    while-shadow native fix pinned in tests/native-consistency.lume, so a
    backend drift in either semantics rejects the LLM program).
    (historical note: the older "sql-tx" and "str-match" seeds were folded
    into the count above on their respective landing days; the counter-
    example wording moved from "missing annotation" to "annotation
    mismatch" when the statistical builtins gained float inference.)
- [x] **P4 semantic layer — `assert_semantic()` closed 2026-10-21.** The
    old "optional / open" DSL builtin (below) was deliberately NOT added
    to the language; the open question is instead committed as a static
    declaration contract, `scripts/verify-ai-semantics.sh`, that the gate
    runs before the equivalence legs and that never executes the program:
    `//@semantic <label>: <expr>` — every referenced identifier must be
    defined (dangling rejects), every label must be claimed by a
    print(...) (unclaimed rejects), and the program must be a pure
    computation (no server{} / run()) the host executor can run
    unsupervised. Pass seeds: examples/ai-generated/norm-stats.lume +
    semantic-sort.lume; reject seeds: tests/ai-bad/semantic-dangling.lume
    + semantic-unclaimed.lume. The retry loop reuses the same exit-1
    counter-example contract. Remaining host-side (scope guard): the real
    LLM tool dispatch, whose contract is the manifest file plus this
    declaration language. — **Real dispatch closed 2026-10-22**:
    agent-httpd `examples/tools/ai-produce-llm.py` (prompts → OpenAI-
    compatible chat → ```lume extract → this gate → counter-example retry
    loop → accept rows into $AI_CANDIDATES_DIR). Verified end to end
    against the local router (Qwen3-8B): llm-summary-stats + llm-range-
    sieve generated, gate-accepted, produce-loop READY, committed as
    must-pass seeds. The loop needed one spec fix the runs exposed —
    without a few-shot reference program the small model writes Python,
    which the gate rejects (correctly) but the retry loop alone could
    not teach it out of.

- [x] **P4 gap (cont.) · `server{}` LLVM leg — closed 2026-10-20, including
  `req.*` member access.** env() part closed 2026-10-20: env() now has a
  static type in both native backends' inference tables (codegen_scan.c /
  llvm_cg_scan.inc), an emit path (codegen_call.c / llvm_cg_call.inc →
  `lume_bi_env`), a runtime helper in bridge_native.c that keeps native_env's
  credential-masking promise, and the LLVM backend now links bridge_native.o
  (`backend_build_native`, shared with backend.c) with `-lws2_32` on Windows
  (the LLVM link previously carried only rt.o — that is also why
  `now()`/`argv()` never worked there). server{} emission landed the same
  day: N_SERVER → lume_srv_init, N_ROUTE → lume_srv_route, and a
  route-handler FFI compiler (cg_route_handler + cg_handler_ret in
  llvm_cg_stmt.inc — string → text, map literal → JSON), plus a text-backend
  fix (handler locals were lifted into undefined @lv_ globals while the
  scanning_top window was open). `req.*` closed 2026-10-20: handler member
  reads (method/path/query/body → string, body_len → int) emit GEP+load
  against the SrvReq FFI struct in both backends' member_field_type and
  emitter; tests/serve-env.lume's handler now reads req.method/req.path on
  all three legs (interp==text==LLVM, "serve-env ok") + env-consistency +
  native-consistency three-leg run. Pinned by `tests/serve-env.lume`.
  **Remaining P4 footnote: `req.query_params("k")` / `req.get("k")` calls
  inside LLVM route handlers** (the text backend lowers these via
  lume_srv_qp; the LLVM emitter has no in-handler call branch yet).
  — **closed 2026-10-20**: get(req.query_params, k, d?) → lume_srv_qp and
  int(x) → lume_srv_atoi now emit in the LLVM backend (llvm_cg_call.inc
  in-handler branch); both backends' inference tables type int→int /
  get→string, with the text copy moved ahead of the generic get(m,"k")
  branch (which otherwise swallowed get(req.query_params,…) — its receiver
  types as string, not struct, so it returned NULL). Pinned by
  tests/serve-env.lume's handler now reading get(req.query_params, "tag",
  "none") on all three legs. P4 gap is now fully closed: serve programs
  using server{} + env() + req.* fields + req.query_params get() compile
  identically on interp/text/LLVM.
  LLVM now()/run() also landed 2026-10-20 (the text backend had them,
  the LLVM emitter did not): now() → lume_bi_now, run() → lume_srv_run.
- [x] **Native-backend serve coverage beyond P4 (filed 2026-10-20 from the
  lume examples; closed 2026-10-20).** ~~`write "/path"` / `read "/path"` verb groups~~ —
  **closed 2026-10-20**: alias routes now expand in both native backends to
  one registration per method (built-in groups write =
  POST/PUT/PATCH/DELETE, read = GET/HEAD — the native mirror of interp.c's
  seed_verb_groups), and handler-less routes (write "/x", get "/x") register
  the new lume_srv_default_handler ({ action, method, got } — the native
  mirror of builtins.c's native_default_route, action null for label-less
  groups). Pinned by tests/serve-env.lume (write "/items" + get "/health"
  on all three legs). Remaining: `tool ...` statements and the agent-side
  builtins (discovery_endpoints/skills/tools/mcps) are host (lume
  framework) bridge capability — examples/hello.lume reaches "tool (this
  construct)" once the verb groups compile; that is a host-integration
  boundary, not a lume-core language gap. And `func respond(d) { return d;
  return {...}; }` (untyped parameter, returns mixing an untyped value with
  a map literal) — **partially closed 2026-10-20**: the signature pass no
  longer fails early when a return expression's type is not yet resolvable
  (N_RETURN in codegen_sig.c now skips instead of ERR_SET, so the fixed
  point can back-fill untyped parameters from call sites first). The
  remaining blocker for abac.lume is `policy.decide(...)` — ns (imported
  module) member calls have no signature/type-inference path in
  walk_sigs (its N_CALL branch only resolves N_VAR callees), so `d` never
  gets typed. That is part of a broader native-backend module-support gap
  (imported-module function calls), filed here for follow-up.
  — **sig phase closed 2026-10-20**: `codegen_infer_signatures_multi` runs
  each imported module's AST through the signature pass first (unresolved-
  parameter check disabled — an exported function has no call site inside
  its own module), registers its functions under `"<ns>.<name>"`, then runs
  the entry pass with those signatures injected; walk_sigs and
  infer_node_type's N_CALL both resolve an N_MEMBER callee whose object is
  a variable (policy.decide) to the injected signature, so untyped module
  parameters are back-filled from the entry's call sites exactly like local
  ones. Pinned by tests/ns-member.lume (`lib.identity(x)` typed from
  `wrap(42)`; abac.lume advances past the decide call to the emitter).
  **Remaining (emit phase)**: the callee-emission paths
  (codegen_call.c / llvm_cg_call.inc) still refuse a non-N_VAR callee
  ("only direct calls to named functions"), and module functions/type
  decls/top-level bindings have no native emission story yet — the next
  gap abac.lume will hit once member calls emit.
  — **emit phase (IR-text backend) closed 2026-10-20**: `codegen_emit_ir_multi`
  emits the entry and every imported module together — module functions are
  collected under "<ns>.<name>" for member-call resolution but defined under
  their own source-name symbols (@L_<fname>, which a member call emits
  against), module type decls join the struct table under their plain names
  (`d.decision` has no namespace to qualify with), and module top-level
  statements run first inside L_top so a module function body reads its
  module's tables through the same @lv_ globals the entry uses.
  codegen_call.c's N_CALL now accepts an N_MEMBER callee whose object is a
  variable (policy.decide(...)) and emits against the module function's own
  symbol. Pinned by tests/ns-member.lume, which now compiles AND runs
  (`lib.identity(x)` typed from `wrap(42)`, prints 42 on the IR-text
  backend). **abac.lume's next blocker is entry-side**: `req.user` /
  `req.query_params.*` inside server route handlers still have no type (the
  server-handler FFI inference gap, P4-family) — decide's parameters are
  back-filled from call sites, but the call-site arguments themselves are
  untyped. **libLLVM backend now closed too (2026-10-20)**: the C-API
  codegen got the same treatment — llvm_codegen_module_core takes the
  multi-file form, module signatures carry body/pnames/node (so a member call
  recovers the callee's own symbol), llvm_cg_call.inc accepts an N_MEMBER
  callee over a variable object, and backend_llvm_native/emit_ir take the
  multi-file form. tests/ns-member.lume compiles AND runs on both native
  backends (prints 42); serve-leg-check stays green on all three legs.
  **Server-handler FFI + abac closed 2026-10-20 (Task #23)**: entry-side
  handler parameters typed end to end on both native backends — req.* fields
  (method/path/query/body/body_len/user) via the SrvReq FFI contract,
  req.query_params.<key> two-hop members lowering to lume_srv_qp, get() on
  req.query_params kept as its in-handler branch, handler struct returns
  (named via stringify, anonymous maps via cg_json_struct/llvm_json_struct
  with float/struct/list value serialization and typed map-getter defaults),
  now() aligned to epoch seconds, and the file builtins
  (read_file/write_file/lock_file/unlock_file) implemented natively with a
  self-contained lume_flock (the compile link line pulls only
  bridge_native.o, not os_win32.o). examples/abac.lume now compiles on BOTH
  native backends (out/abac-text.exe, out/abac-llvm.exe); pinned by
  tests/handler-struct.lume (the respond() shape — nested attribute maps,
  get(map,key,null), get(list,i,default), now(), file builtins,
  req.query_params, struct returns — compiles on both). serve-leg-check
  stays green: serve-env AGREE x2, native-consistency MATCH/AGREE x6.
  Run-time note: blocking listen on this Windows box fails with WSAENOTSOCK
  (errno 10038) for every process — even a PowerShell TcpListener on a free
  port — i.e. a winsock/host-environment fault, not an emitter defect; the
  same check is expected to pass on CI/Linux where serve-env's non-listening
  legs already run.
  **Closed 2026-10-21**: module functions that nest lambdas or local
  functions are now fully supported — the old boundary (cname counters
  colliding across files) is fixed. Synthetic names carry the module index
  (`__lume_lf_<m>_<n>` / `__lume_clo_<m>_<n>`; entry keeps the historic
  `__lume_lf_N`/`__lume_clo_N` spelling), both emitters collect module-file
  lambdas (not just the entry's), and a plain call inside a module function
  resolves "<ns>.<name>" first via the per-emit `cur_ns`. En route, a
  deeper inference gap surfaced and was fixed: `infer_node_type` read a
  lambda's return type from the AST slot, which the inference fixed point
  only writes back *after* the loop — a parameter typed from a lambda call
  (`twice(g(n))` where twice is a local function) never filled. It now
  reads the Sig's live return type (same as the member-call branch).
  Pinned by tests/ns-lf.lume + tests/ns-lib-lf.lume: entry and module both
  nest a local function and a lambda; interp/text/libLLVM all print
  39/10.
  **Module-side imports closed 2026-10-21**: a module function calling
  *another module's* member (`b.double(n)`) and reading its member
  variables (`b.factor`) now works on both native backends. The root gap
  was that the multi-file passes registered every module's functions under
  only the *entry's* alias for it — a module A importing B as `b` left B's
  functions registered under bare names, so `b.double` failed with "call to
  unknown function". The entry-alias table (`nsnames`) is now a per-module
  alias list (`alias_list[m]`/`alias_cnt[m]`), collected in main.c's
  compile_mods from every loaded module's imports (loader ns_names), and
  every register point (codegen pass 1, llvm pass 1, the multi-file
  signature pass) registers one "<ns>.<name>" key per referencing alias;
  module member *reads* (`b.factor`) resolve the object as a module alias
  to the module's @lv_ top-level binding in both cg_member emitters.
  Pinned by tests/ns-chain-main.lume (entry → A → B: `a.triple(5)` = 
  `b.double(5)+b.factor` = 17) across interp/text/libLLVM in
  tests/native_backends.sh.
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

## P6 — Cross-statement transactions (interpreter closed 2026-10-20; native parity open)

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

- [x] Value system: `sql_tx_begin()` returns a first-class handle —
  new OBJ type (`OBJ_SQLTX`, lume.h), GC finalizer (`sql_tx_finalize`,
  value.c obj_release → rollback + close; SQLite rolls back on close
  anyway, the explicit ROLLBACK keeps the error path honest).
  Interpreted 2026-10-20. Value semantics: the handle is a GC object
  like any other (map round-trip carries the pointer, not a copy);
  closure capture still hits the llvm capture machinery (see P7 — that
  half applies, but the native backends don't emit sql at all yet, so
  capture is moot until parity lands).
- [x] Connection management: per-handle connection — `sql_tx_begin`
  opens READWRITE|CREATE, `commit`/`rollback` close, GC collection of
  an open handle rolls back and closes. No per-worker cache: the
  stateless contract and fork-safety arguments above are inherited
  verbatim; the handle scopes the connection to the script's own
  lifetime. Interpreted 2026-10-20.
- [x] Guardrails: dedicated builtin entry (`sql_tx_exec`), never the
  `sql_write` whitelist — but the SAME `sql_validate_write` guard runs
  inside it (one known-shape statement per call). Error semantics:
  a failing statement leaves the transaction OPEN; the caller decides
  commit vs rollback (rolled-back effects are already gone, the handle
  stays usable). Interpreted 2026-10-20.
- [x] Three-backend parity (2026-10-21): interpreter + text + llvm
  emitters — the whole sql builtin set now emits on both native
  backends, not just the tx four. Makefile probes libsqlite3
  (`SQLITE_PREFIX` → `HAVE_SQLITE=1`, isolated include/lib dirs),
  `src/sql_guard_internal.h` shares the interpreter's validation verbatim
  with the native runtime (`bridge_native.c` `#if LUME_HAS_SQLITE`: query
  read-only, write read-write, tx per-handle connection + BEGIN; errors
  via `lume_error_set`), `codegen_call.c` + `llvm_cg_call.inc`
  dispatch the two forms `(sql[,params])` vs `(path,sql[,params])`.
  One native divergence, structural not a bug: the handle is a plain
  heap struct with no GC finalizer — commit/rollback free it, an
  abandoned open transaction leaks until process exit (the interpreter
  closes via the OBJ_SQLTX finalizer). Fixed en route: sql row maps
  must copy `sqlite3_column_text` (a volatile per-step buffer; map_put
  stores the string pointer verbatim), which corrupted TEXT values and
  crashed the smoke program. Verified: `native-sql-smoke` on the text
  and llvm backends prints the interpreter's exact expected output;
  sqlite regression 62 PASS + 0 known diffs (2026-10-21: the three
  "sqlite<3.35 DML-CTE diffs" were misattributed — the system library is
  3.51.0 and rejects CTE-body DML on every release; the guard now rejects
  that shape outright and the cases use top-level RETURNING). Host
  sqlite_tool.c needs no merge — validation semantics unchanged (the
  guard move to sql_guard_internal.h is behavior-preserving).

  CI leg 2026-10-21: tests/native_backends.sh now runs the sql fixture
  (tests/native-sql-consistency.lume/.expected, db path from SQLITE_DB)
  through interpreter/text/llvm and diffs every pair — green on this
  host. Two latent Windows-LLVM bugs found en route: the msys2/mingw
  libLLVM default triple "x86_64-w64-mingw32" parses with an empty OS, so
  EmitToFile emitted ELF objects the mingw lld refuses ("unknown file
  type") — llvm_cg_host_triple normalises it to "x86_64-w64-windows-gnu"
  (COFF); and the llvm sql emitters freed LLVMValueRef operands (copied
  from the text backend's char* free pattern) — heap corruption on every
  --compile-llvm sql script, now removed. Both made the llvm leg of the
  parity suite real on Windows; tx bool parity (TY_BOOL + trunc to i1)
  landed in the same commit.

Interpreter implementation (2026-10-20, user-level override of the
"deliberately not started / Done when" gate): `sql_tx_begin(path)` →
handle; `sql_tx_exec(tx, sql[, params])` → affected rows; `sql_tx_commit`
/ `sql_tx_rollback(tx)` → bool, close the handle. Registered in every
build (stub reports "unavailable" without libsqlite3, same as
sql_query/sql_write). 11 new cases in tests/sqlite.lume cover atomicity
(uncommitted rows invisible to a second connection), rollback, spent
handle rejection, whitelist enforcement, and error paths. Verified on a
real sqlite3 build (3.34.0 dev kit at `D:\Software\mingw64\opt`).

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
db_validate_write`, `cd94be1`), not copied by sync. The tx builtins
follow the same rule when the host picks them up (manual merge, same
path).

**Done when**: a lume-nsgm-scale workload demonstrates a real crash or
correctness need for cross-statement atomicity (not just purity), and
the value-system design clears the llvm closure-capture gap. (The gap
this gated on — closures capturing function locals on the llvm backend
— landed 2026-10-18, see P7; a transaction handle behind a closure
still hits the general capture machinery, and the top-level half of P7
would apply to it too.) Status 2026-10-20: interpreter closed under a
user override of the gate; parity (native-backend sql emission) closed
2026-10-21, see the parity item above.

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

**Core half closed 2026-10-21** — root cause found and fixed. The
crash is the interpreter's `for-in` handling, not the HTTP layer:
the iterable of `for (x in <inline-temp>)` was popped off the VM
stack immediately after eval, and the stack is the GC root set
(value.c marks `stack[0..stack_count)`) — a popped slot is no longer
rooted, and the C-level pointer in `exec_statement_for` was the only
reference to the list/map object. Any allocation inside the loop body
that crossed the 2 MiB threshold ran `gc_collect` and freed the
object under the pointer: use-after-free on the next round
(reproduced on Windows as 0xC0000005 before the fix; `let xs = ...`
bindings are safe because env keeps the object rooted — matching the
original "inline-temp bad, let good" observation). Fix: the iterable
stays ON the stack for the whole loop (body statements are
stack-balanced, so the slot stays valid); all exits pop it. Regression:
`tests/for-in-gc.lume` + `make for-in-gc-test` (interpreter only — the
native backends have no GC), and the CI asan job now runs the fixture
under ASan (`bin/lume-core-asan tests/for-in-gc.lume`).

Steps:

- [x] Build host `lume` with ASan (Linux container if needed; core's
      `build-asan` recipe shows the flag set) and replay the chinese
      `c10f23d^` server.lume: recite ×5 must crash under ASan with a
      readable report (use-after-free / heap-buffer-overflow expected).
      Core-side replay closed 2026-10-21 instead: the interpreter
      repro `out/forgc.lume` crashed with 0xC0000005 pre-fix and runs
      clean post-fix; the CI asan job replays `tests/for-in-gc.lume`
      under ASan on every push. (Host-side replay of the actual
      chinese/server.lume remains for a Linux container.)
- [x] Shrink to a minimal server.lume (one route, one inline-temp
      loop, one loader) — keep it as a regression fixture if it fits
      the agent-httpd test layout. (Core fixture already exists as
      tests/for-in-gc.lume; the HTTP-shaped minimal server would still
      prove the agent-httpd bracket side.) — Closed 2026-10-21 on the
      host: agent-httpd's tests/ are C-only (test_*.c, no .lume runner),
      so the fixture landed in the host tree where the live-server test
      harness lives — `examples/minimal-server-gc.lume` (server{} +
      GET /gc handler running for (x in range(60000)) with an
      inline-temp range(60) per round) + a `tests/run_all.sh` leg (4e)
      that boots the server, curls `/gc` and asserts the answer
      (`minimal gc ok: 1799970000`) over real HTTP on every push —
      covering the per-request arena/bracket side end to end.
- [x] Locate the lifetime bug: likely candidates are (a) the eval of
      the for-in iterable expression not being retained across
      statements while agent-httpd resets a per-request arena, or
      (b) the temp value being allocated in a pool that a later
      request's setup overwrites (explains "crashes on a LATER
      request, not the first"). Check how interp holds the iterable
      vs. how the HTTP layer brackets evaluation.
      Candidate (a) closed 2026-10-21 in the core: the iterable was
      not retained as a GC root after vm_pop (see "Core half closed"
      above) — the interpreter bug alone reproduces the crash. The
      agent-httpd arena/reset side is still to check on the host.
- [x] Fix + regression: if the fix lands in core (interpreter value
      lifetime), it flows to the host via sync-lang semantics — but
      note interp.c is host-owned (minijson include block), so a
      core-side fix must be hand-merged like typecheck_expr was; if
      it lands in agent-httpd (arena bracketing), the standalone
      agent-httpd repo is the canonical place.
      Fix landed in core (interp.c exec_statement_for, iterable kept
      rooted on the stack) 2026-10-21; interp.c is host-owned, so the
      host needs the same one-line-region hand-merge when it next
      syncs.
- [x] Done-when: under ASan, the killer sequence replays clean on the
      unfixed script shape; chinese's original handler works without
      the workaround; CI (three legs + fuzz) stays green.
      Done-when met on the core shape: ASan CI leg replays
      tests/for-in-gc.lume clean; make test + native_backends (three
      legs incl. sql) + fuzz stay green. "chinese's original handler
      without the workaround" remains to verify on the host once the
      host syncs the interp.c fix.

## P10 — Native compile of large scripts: intermittent SIGSEGV, plain build only (filed 2026-10-23, root cause fixed 2026-10-23)

Compiling `tests/native-consistency.lume` (~1.2k lines) with
`--compile-text` or `--compile-llvm` segfaults intermittently
(rc=139) on the plain mingw build; the same binary compiles smaller
scripts (≤ ~200 lines) reliably. Found while closing SPEC §8.1 #9:
it is **pre-existing** — `git stash` of the in-flight checker change
left `git stash pop`-equivalent HEAD (`8beb82b`) still crashing
~75% of runs on `head -450` of the fixture. The ASan build
(`make asan`'s `bin/lume-core-asan`) compiles the full fixture and
`head -450` cleanly every time and reports nothing, so this is a
layout-dependent memory error the sanitizer's allocator happens to
mask — not an ASan-detectable OOB/UB.

Evidence (original triage):

- `head -200` of the fixture: stable green (3/3).
- `head -450`: stable crash on the plain build (3/3), stable green
  under ASan (4/4).
- Middle sizes (400, 480, 500, 510, 520, 535, 550, 600): crash rate
  between 1/3 and 3/3, no stable boundary — scale-dependent, not
  content-dependent.
- Both emitters crash (text and libLLVM), so the fault lives in the
  shared codegen front-end (codegen.c / codegen_sig.c /
  codegen_scan.c …), before either emitter's IR is finalized.
- Full `tests/native-consistency.lume` (1.2k lines): ~4/4 crash on
  plain, clean under ASan. `native_backends.sh` on a plain build can
  therefore fail intermittently for this reason alone.

**Root cause (located 2026-10-23, fixed in both emitters).** Crash
stack always lands in `asg_find` (codegen.c) — `strcmp` reading a
corrupted Asg `name` — with the offending element always the *last*
entry (`i == n-1`) and its `name`/`slot` bytes being
uninitialized memory (0xA5 canary bytes / 0 / stale IR-text
residue), i.e. the locals table was truncated to a `n` *larger*
than the count of initialized entries. The truncation comes from the
while-scope machinery: `cg_while` hides the body's shadow binds
while emitting the condition by restoring `g->locals.n` from
`g->wh_saved[g->wh_idx]`, the value the scan phase recorded "before
the body's lets". That pairing is per-function (`cg_function` saves
`wh_base = g->wh_idx` on entry, restores it on exit), but **preset
mode (the synthetic `L_top` for top-level statements) broke the
pairing**: pass 1.7 pre-scans the top-level statements and records
`L_top`'s while-marks from counter **0**, while every named
function — emitted before `L_top` in pass 2 — re-uses the
post-pre-scan counter as its own base. So `L_top`'s `cg_while`
read `wh_saved[k…]` (the *last emitted function's* marks, or stale
realloc memory) instead of its own `[0..k)`, truncating `locals.n`
to a foreign size — often larger than the initialized entries — and
`asg_find` walked into uninitialized Asg rows until `strcmp`
dereferenced garbage. Layout dependence (which runs crash) follows
from what the foreign saved value happens to be; both emitters
share the scan/emit machinery, hence both crash. **Fix:**
`cg_function` now sets `int wh_base = g->wh_idx; if (preset)
wh_base = 0;` in both `src/codegen_stmt.c` and
`src/llvm_cg_stmt.inc` (symmetric), so `L_top` reads its own
pass-1.7 marks.

Diagnostics used (for the record, all temporary and reverted):
arena canary region + `arena_check_canaries`; `AddVectoredExceptionHandler`
crash dumper (CRT's `SetUnhandledExceptionFilter` is bypassed by the
mingw CRT converting SEH to SIGSEGV first) writing minidumps +
StackWalk64 traces, symbolized via `llvm-symbolizer --obj=bin/lume-core.exe
--demangle 0x140000000+<rva>` (the exe needed `-g` at link time to keep
DWARF); an `asg_find` guard asserting `arena_contains(name)` which
surfaced `ASG-NAME-CORRUPT i=n-1` with name bytes 0xA5A5… (canary),
0x6C25202A… (`"i64* %l"` IR text), and 0x0; an irbuf OOB probe (no
hits — the IR buffer itself never overran). Dead ends not worth
re-trying: clang-23 has no `-fanalyzer`; MSYS2 mingw64 ships no
`gcc.exe`; `-O0` still crashes (source-level bug, not optimizer UB);
command-line CFLAGS overrides break the RT_DEFS/Include chain (edit
Makefile instead); `SetUnhandledExceptionFilter` and lldb-on-minidump
both non-functional on this toolchain.

**Verification (post-fix, plain build):** `head -450` 6/6 and the
full `tests/native-consistency.lume` 6/6 `--compile-text` (was
~4/5 crash) and 6/6 `--compile-llvm` rc=0; `tests/native_backends.sh`
19/19 ok (incl. sql, stats, serve, ns-chain legs). ASan:
`bin/lume-core-asan` compiles h450 + full fixture 6/6 with zero
reports. `make asan`'s remaining red is the pre-existing tre-regex
UBSan misaligned-read noise on the interp leg (`regexec.c:264`,
documented under P3/P11) — unrelated to this fix. En route the
`closure-reassign-ok` regression leg in `tests/native_backends.sh`
gained the same CRLF normalization `check()` uses (the interpreter's
CRT text-mode stdout ends lines with `\r\n`; compiled binaries emit
LF), and `tests/smoke.c`'s `mkdir(".data", 0755)` was made
Windows-portable (CRT `mkdir` is single-argument) so `make asan`
reaches its `asan-compile` target — the smoke binary previously
failed to build under mingw.

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

## Interface contract — bridge.c ↔ agent-httpd config (filed 2026-10-22)

`lang/bridge.c` reads `agenthttpd_config.react_prefix` (configurable SSR
relay prefix — history `server.lume` uses `react_prefix = "/"`). The
field must exist in the pinned agent-httpd submodule or lume fails to
compile: history's SSR Dockerfile build hit
`'agenthttpd_config' has no member named 'react_prefix'` when the gitlink
pointed at 195cfaf (pre-0b43aa6). The implementation (0b43aa6 /
47320af) exists only on the local `win-local-chain` branch, so:

- keep the DSL↔agent-httpd config contract in sync when bumping either
  side (a lume-core commit reading a new agent-httpd field must land
  together with the submodule bump + a pushed agent-httpd commit);
- ~~until agent-httpd pushes react_prefix to origin/main, the lume
  submodule gitlink cannot be committed and SSR-relay images (history)
  are only buildable on this machine.~~ — **Closed 2026-10-22**: the
  react_prefix work (0b43aa6 feat(react) + 47320af feat(fcgi)) was
  cherry-picked from the local `win-local-chain` branch onto
  agent-httpd main and pushed (`b998e7f..23b1c4e`); the lume host
  committed the bumped gitlink (`7e73e91`) and a fresh
  `clone --recurse-submodules` now checks out 23b1c4e with
  `react_prefix` in `src/agenthttpd.h`. En-route lesson: an interim
  gitlink at b998e7f (pre-cherry-pick) briefly made the host
  uncompilable — `lang/bridge.c:662` reads `cfg.react_prefix` while
  the submodule lacked the field; the host had only compiled because
  the submodule working copy sat on `win-local-chain`. SSR-relay
  images are now buildable on any machine.
## P11 — 2026-10-22 memory-safety audit (php2shell-class review)

Status: closed (filed 2026-10-22, all sub-items done 2026-10-22). A post-CVE-2026-109618/109619
pass over `lume-core/src` (arena, rt, codegen, interp, bridge,
main.c) and the agent-httpd request path. Findings, all
contextual — none of them is a php2shell-shaped UAF or stale
global pointer surviving across requests in a long-lived worker:

- **arena.c** is deliberately single-threaded and reset-wholesale;
  the LSP path obeys the contract (one document version at a time).
  No fine-grained free, so no UAF surface.
- **rt.c** exits on OOM (`exit(70)`) — no half-initialised state
  leaks into a follow-up call (the failure mode CVE-2026-109618
  exploits).
- **rt.c statics** (`g_index_failed`, `g_in_try`, `g_native_err`)
  are reset per-lookup and the native backend runs in a spawned
  process, so they cannot leak state across requests.

Remaining action items:

### Confirm no global pointers survive a failed/aborted VM run

`interp.c` + `value.c` GC is the one unreviewed part of this pass.
The P9 (for-in heap corruption under agent-httpd) precedent shows
state can outlive a malformed script. The question is whether a
`lume_exec()` that returns an error leaves any VM-owned pointer
reachable from a subsequent `lume_exec()` on the same VM handle:

- [x] **Closed 2026-10-22**: `loader_run` now resets `vm->error` /
      `error_msg` / `stack_count` at entry (the error latch made a second
      run on the same VM silently no-op); contract documented in
      `docs/NATIVE.md` ("VM 生命周期与嵌入契约"); regression =
      `make vm-reuse-test` (bad script → good script → good script, one
      VM, any compiler; ASAN/LSAN runnable).

### Extend ASAN/LSAN to the LSP + codegen paths

P1 closes only the *parser* fuzz leg; the LLVM backend and LSP
bridge (`bridge_lsp.c`) have no sanitizer target yet:

- [x] Add `make test-asan-codegen` covering `native_backends.sh`
      under ASAN (new suppressions file scoped to `llvm_cg_*.inc`
      so backend-specific noise is trackable).
      **Done (already landed as `asan-compile`)**: the `asan` target's
      `asan-compile` prerequisite runs `--compile-text`, `--compile-llvm`
      and `tests/native_backends.sh` under ASan+LSan (Makefile comment
      block above the target); the existing single `tests/lsan-suppressions.txt`
      rule is scoped to `LLVMVerifyModule`'s 1-byte string, by function
      name not library name, so codegen's own leaks still report.
- [x] Add a `lume-lsp --check` smoke under LSAN to catch
      doc-lifetime leaks on document-switch.
      **Done 2026-10-22**: `bridge_lsp.c` gained `didClose` (drops the
      LspDoc from the open set) and exit-time `lsp_docs_clear()`; new
      `tests/lsp_smoke.sh` drives initialize/open/change/switch/hover/
      close/shutdown/exit and is wired into `make asan` under LSan.

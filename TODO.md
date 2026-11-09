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
- [x] **Quant-analysis builtins (filed 2026-10-24, closed 2026-10-24).** A
  layer above the CDFs for portfolio / backtest / return-series scripts,
  three-backend-consistent exactly like the statistical builtins:
  - `mean(l)` / `variance(l)` / `stddev(l)` — sample variance (n-1);
    the native helpers answer NAN and the interpreter wrappers
    vm_set_error on n<2
  - `median(l)` / `percentile(l, p)` — order statistics; p∈[0,100],
    linear interpolation between the bracketing order statistics
  - `covariance(x, y)` / `correlation(x, y)` — sample (n-1), equal-length
  - `linreg(x, y)` — least squares → fresh `{slope, intercept, r2}` map
  - `pct_change(l)` — fresh percentage list (n-1; zero prev → NAN)
  - `max_drawdown(l)` — positive percentage, 0 for monotonic, NAN for n<2
  Implementation: rt.c `lume_bi_*` native twins (element reads widen int
  slots through lume_list_at_f); builtins_math.c interpreter wrappers
  (same duplicated-core split as the CDFs); interp.c registration;
  typecheck.c `LUME_BUILTIN_NAMES` + arity; codegen_scan.c /
  llvm_cg_scan.inc inference (scalars→float, pct_change→list(float),
  linreg→struct); codegen_call.c / llvm_cg_call.inc list-arg emit
  branches (sort()'s pattern). Regression: tests/quant-consistency.lume +
  .expected run by tests/native_backends.sh across interp/text/llvm
  byte-identically; examples/quant.lume walks a price/benchmark study.
  Independent golden values: tests/quant-refcheck.py (29 pinned values vs
  Python's statistics module) and tests/quant-fuzz.py (seeded 10 series x
  18 rows; three-leg equality + Python reference with TOL=1e-5 + property
  assertions: variance>=0, |corr|<=1, covariance symmetry, r2 in [0,1],
  drawdown>=0), both hooked into native_backends.sh and skipped when no
  python is available. Numerical stability: variance/stddev/covariance/
  correlation/linreg were rewritten on Welford single-pass recurrences
  (rt.c and builtins_math.c use the identical recurrence); the naive
  two-pass mean-then-sum-of-squares degrades on large-offset data, and
  quant-fuzz.py gained a large-offset distribution family (base 1e6-1e10
  with 1e-6 relative fluctuations) that now pins that family.
  Distribution shape + rolling/EWMA (2026-10-24): skew (adjusted
  Fisher-Pearson, n≥3) and kurtosis (sample excess Fisher, n≥4) join as
  scalars; rolling_mean/rolling_stddev (list + window, NAN until the
  window fills, ddof=1) and ewma (list + alpha∈(0,1], recursive) join as
  list builders, all three-backend consistent — float windows truncate
  through the new rt.c `lume_bi_ftoi` helper on both native backends,
  matching the interpreter's (int64_t) cast. quant-refcheck.py now pins
  39 Python-verified cases and quant-fuzz.py runs 10 series x 38 rows
  (380 checks) covering the new family.
  norminv (2026-10-24): inverse standard-normal CDF joins the
  distribution family — Acklam rational approximation (2011), identical
  constants in rt.c lume_bi_norminv and builtins_math.c bi_norminv; p
  outside (0,1) errors on the interpreter / NAN natively. The reverse
  round-trip normcdf(norminv(p)) == p runs at every fuzz dataset (RPP 39,
  390 checks); fixed values are pinned in quant-refcheck (45 cases) and
  quant-consistency (35 rows).
  Monte Carlo RNG (2026-10-24): deterministic PCG32 (O'Neill PCG-XSH-RR)
  with a fixed default seed and increment joins the quant toolbox —
  seed(n) re-bases the stream (state=n, default inc), rand() = top24*2^-24
  in [0,1), rand_range = lo + (hi-lo)*rand, rand_int draws 64 bits (two
  outputs, low word first) and reduces mod span, rand_normal = Box-Muller
  with a 2^-25 anti-zero bias keeping u1/u2 out of log(0). No
  BUILTINS-table entry (rand is 0-arg, the rest are int/float-mixed 1-2
  args): both native emitters declare the five lume_bi_* and branch by
  name, widening ints via lume_bi_float and truncating floats via
  lume_bi_ftoi exactly like the interpreter's (int64_t) cast.
  quant-consistency pins the seed-42 stream (8 rows, three legs
  byte-identical), quant-refcheck adds 8 independent PCG32 cases (53
  total), quant-fuzz runs 10 series x 47 rows (470 checks) with
  rand/rand_range/rand_int bounds plus a re-seed round-trip under every
  distribution kind.
  Monte Carlo distribution family (2026-10-25): rand_bool(p) answers 1
  with probability p (p outside [0,1] errors on the interpreter / 0
  natively); rand_exponential(rate) = inverse-CDF -ln(u)/rate (rate <= 0
  errors / NAN); rand_poisson(lambda) = Knuth's multiplicative method
  (exp(-lambda) underflow errors / -1); shuffle(list) = Fisher-Yates on
  a fresh list (input untouched), each step consuming rand_int(0,i)'s
  two PCG32 draws so interpreter and native backends stay byte-identical.
  rand_bool/rand_poisson infer TY_INT, rand_exponential TY_FLOAT,
  shuffle a float list; explicit arities like the base five.
  quant-consistency pins 9 more seed-42 rows (52 total), quant-refcheck
  adds 9 independent cases (62 total), quant-fuzz runs 10 series x 56
  rows (560 checks) with bool/exponential/poisson bounds and a second
  re-seed round-trip after the distribution rows.
  Continuous samplers (2026-10-25): rand_cauchy(loc,scale) = one-draw
  inverse-CDF loc + scale*tan(pi*(u-0.5)); rand_gamma(shape,rate) =
  Marsaglia-Tsang 2000 (ACM TOMS 679) with a shape<1 boost (sample
  shape+1, raise a fresh u to 1/shape) and a shape>=1 rejection loop
  (Box-Muller z, then one u per round — loop count data-dependent but
  stream-fixed, so cross-backend byte-identity holds); rand_beta(a,b) =
  Ga/(Ga+Gb) from two sequential gamma draws; rand_chisq(k) = twice
  gamma(k/2); rand_student_t(df) = a standard normal over sqrt(chisq/df).
  All five infer TY_FLOAT (both scans) and emit through the 2-param /
  1-param RNG branches of both call emitters; invalid parameters error
  on the interpreter / NAN natively. quant-consistency pins 7 more rows
  (59 total), quant-refcheck adds 7 independent absolute-k Python
  Marsaglia-Tsang cases (69 total), quant-fuzz runs 10 series x 63 rows
  (630 checks) with positivity/range/finiteness assertions on the new
  samplers and the existing re-seed round-trips.
  Inverse-transform samplers (2026-10-26): rand_geometric(p) = the
  failure count floor(ln(ub)/ln(1-p)) for p in (0,1] (one biased draw,
  TY_INT); rand_weibull(shape,scale) = scale*(-ln(ub))^(1/shape);
  rand_logistic(mu,s) = mu + s*ln(ub/(1-ub)); rand_pareto(xm,alpha) =
  xm/ub^(1/alpha); rand_laplace(mu,b) = mu - b*sgn(ub-0.5)*ln(1-2|ub-0.5|)
  — each a closed-form inverse CDF on one draw (TY_FLOAT). All reuse the
  2^-25 anti-zero bias (every formula has a log pole at the endpoints);
  invalid parameters error on the interpreter / -1 or NAN natively. Both
  scans infer, both call emitters extend the 2-param / 1-param RNG
  branches. quant-consistency pins 7 more rows (66 total), quant-refcheck
  adds 7 absolute-k cases (76 total), quant-fuzz runs 10 series x 70 rows
  (700 checks) with non-negativity/positivity/finiteness assertions on
  the new samplers.
  Mixed samplers (2026-10-27): rand_binomial(n,p) = the success count of
  n independent Bernoulli draws (n plain draws, TY_INT) — the first
  builtin with a mixed (int64_t, double) signature, truncating n via
  lume_bi_ftoi and widening p via lume_bi_float in both call emitters'
  dedicated mixed-arity branches, mirroring the interpreter's casts;
  n<0 or p outside [0,1] errors on the interpreter / -1 natively.
  rand_lognormal(mu,sigma) = exp(Box-Muller) on two biased draws;
  rand_gumbel(mu,beta) = mu - beta*ln(-ln(ub)) and rand_rayleigh(scale) =
  scale*sqrt(-2*ln(ub)) each on one biased draw (the inner double log has
  no pole inside the biased domain); rand_triangular(lo,hi) = the
  symmetric inverse CDF (lo + sqrt(0.5u)*span below the midpoint, the
  mirror above) on one plain draw (no poles, so the unbiased draw is
  reused). Invalid parameters error on the interpreter / NAN natively.
  Both scans infer (binomial joins the TY_INT group, the rest TY_FLOAT),
  both call emitters extend the 2-param / 1-param branches plus the
  binomial mixed-arity block. quant-consistency pins 7 more rows (74
  total), quant-refcheck adds 7 absolute-k cases (83 total), quant-fuzz
  runs 10 series x 77 rows (770 checks) with binomial range
  (0..n), positivity and finiteness assertions on the new samplers.
  Advanced samplers (2026-10-28): rand_zipf(n,s) = one plain draw against
  the precomputed cumulative i^-s mass (O(n) setup, TY_INT); rand_
  negative_binomial(r,p) = the sum of r geometric failure counts (r
  biased draws, TY_INT); rand_erlang(k,rate) = the sum of k exponentials
  (k biased draws); rand_von_mises(mu,kappa) = Best-Fisher rejection
  (each round two draws — one plain z, one biased acceptance — so the
  data-dependent round count stays byte-identical; kappa = 0 and the
  vanishing-rho case use one plain draw on the uniform circle);
  rand_multinomial(n,probs) = n weighted draws answered as a fresh int
  list — the first list-returning sampler, emitted through the shuffle-
  shaped i8* branch with n truncated via lume_bi_ftoi. The (i64, double)
  mixed-arity emit block now serves four samplers. Invalid parameters
  error on the interpreter / answer -1, NAN or an empty list natively.
  rand_zipf's exact value is not a stable cross-libm contract (mingw pow
  vs Python UCRT pow differ by 1 ULP at a bucket boundary, flipping the
  discrete rank), so quant-fuzz asserts the [1,n] range on every dataset
  instead of exact equality. quant-consistency pins 7 more rows (81
  total), quant-refcheck adds 5 absolute-k cases (88 total), quant-fuzz
  runs 10 series x 84 rows (840 checks).
  Financial paths + Black-Scholes (2026-10-28): sim_gbm(s0,mu,sigma,T,n)
  = geometric Brownian motion (each step one Box-Muller pair via
  lume_bi_rand_normal, s *= exp((mu-0.5*sigma^2)*dt + sigma*sqrt(dt)*Z),
  answers an n+1-point float list with index 0 = s0); sim_bridge(x0,xt,
  sigma,T,n) = accumulated Brownian increments linearly bridged so the
  endpoint is exactly xt (each step one Box-Muller pair, the final value
  reproduces xt to the last ULP on every backend); sim_ou(x0,mu,theta,
  sigma,T,n) = the exact OU recurrence (same Box-Muller pair per step);
  sim_merton(s0,mu,sigma,lambda,jmu,jsig,T,n) = GBM diffusion plus
  Poisson jumps — per step: diffusion Box-Muller pair (2 draws), Knuth
  Poisson on lambda*dt (nj+1 draws including the mandatory first), and a
  joint jump Box-Muller pair scaled by sqrt(nj)*jsig when nj > 0, with
  the jump increment nj*jmu + that term. The six bs_* helpers share one
  bi_normcdf (0.5*(1+erf(x/sqrt(2))) so the three legs agree to the last
  ULP: bs_call / bs_put prices, bs_delta_call = N(d1), bs_delta_put =
  N(d1)-1, bs_gamma = phi(d1)/(S*sigma*sqrt(T)), bs_vega =
  S*phi(d1)*sqrt(T). sim_* return i8* float lists (step count truncates
  via lume_bi_ftoi, other ints widen via lume_bi_float), bs_* are
  double x5 -> double; both call emitters gained dedicated hand-built
  declare blocks. Invalid parameters return an empty list / NAN
  natively. quant-consistency pins 12 more rows (93 total), quant-
  refcheck adds 13 absolute-k cases (101 total), quant-fuzz runs 10
  series x 95 rows (950 checks) with endpoint/pricing/delta assertions
  and a re-seeded gbm replay round-trip. Fixture-layout audit: the
  linreg slope row is emitted inside "let fit%da = linreg(...);
  print(...)" (starts with "let"), so any per-dataset print count that
  scans for print(-prefixed lines undercounts by one; RPP and every
  row-position assertion now use the fixture's true 95-row layout with
  absolute row numbers instead of tail-relative offsets.
  Also closed the `map`-annotation binding bug the fixture surfaced: `: map`
  was a phantom named struct on the native emitters (a `let m: map = {...}`
  bound the map's length where the pointer belonged); parser.c now
  redirects `map` to type_anon_struct() (list's redirect) and type_compat
  accepts anonymous structs against a bare `map` while named structs stay
  strict.
  Hypothesis-testing family (2026-10-29): z_test(list, mu, sigma) =
  (mean-mu)/(sigma/sqrt(n)) with n>=1, sigma>0; z_pvalue(z) =
  2*(1-normcdf(|z|)) clamped at 1; tstat(list, mu) = (mean-mu)/(s/sqrt(n))
  with n>=2, s>0 (zero-variance sample answers NAN); ttest_p(t, df) =
  2*(1-tdist_cdf(|t|, df)) clamped at 1; chisq_stat(obs, exp) =
  sum((o-e)^2/e) (equal-length lists, e>0, all finite); chisq_p(x, df) =
  1-chisq_cdf(x, df) clamped at 0. All p-values reuse the interpreter CDFs
  (lume_bi_normcdf / lume_bi_tdist_cdf / lume_bi_chisq_cdf) so the three
  legs agree to the last ULP; invalid parameters error on the interpreter
  / NAN natively. The rt.c block sits before the lume_bi_mean /
  lume_bi_stddev definitions it reuses, so it carries forward
  declarations (otherwise the text backend's C99 implicit-declaration
  warning). quant-consistency pins 14 more rows (107 total, three legs
  byte-identical — z_test answers sqrt(3), chisq_p(3.84146,1) = 0.05 and
  z_pvalue(2.5758) = 0.0100008 on the classic critical values);
  quant-refcheck adds 14 cases (115 total) whose p-value references are
  definition-based — the Student-t CDF by numerical integration of its
  density, the chi-square CDF by closed forms (erf / exponential / gamma
  by parts) or integration — independent of rt.c's ibeta/igamma
  formulas; quant-fuzz runs 10 series x 101 rows (1010 checks) with
  finiteness and (0,1] p-value assertions.
  Bootstrap / resampling family landed later (2026-10-31, see the checked
  item below); this line is historical, not an open item.
- [x] **Multivariate distributions / copulas (2026-10-30).**
  `rand_dirichlet(alphas)`: iid Gamma(alpha_i, 1) draws via the existing
  bi_gamma_sample (boost below shape 1), normalized — a simplex share in
  (0,1), flat float list out. `rand_binormal(mu1,mu2,sd1,sd2,rho,n)`:
  two Box-Muller pairs per sample, x1 = mu1+sd1*z1, x2 =
  mu2+sd2*(rho*z1+sqrt(1-rho^2)*z2). `rand_gauss_copula(rho,n)`: the same
  pair through lume_bi_normcdf, so both marginals are exact U(0,1)
  probabilities. `rand_t_copula(rho,df,n)`: the pair scaled by a shared
  sqrt(df/chisq(df)) draw (2*bi_gamma_sample(df/2)) then lume_bi_tdist_cdf
  — common-tail dependence beyond the Gaussian copula. Invalid parameters
  answer an empty list (n<1, rho outside (-1,1), NAN shapes); the native
  emitters reuse the sim_* list-argument blocks (flat list for dirichlet,
  scalar->list with a trailing count for the rest; LLVM carrier free /
  reference-free). quant-consistency pins 13 more rows (120 total, three
  legs byte-identical; each call re-seeds 42 so the rows replay from the
  absolute start of the stream); quant-refcheck adds 12 absolute-stream
  cases (127 total) — pcg_dirichlet via pcg_gamma, the copulas via
  pcg_rand_normal + _bs_ncdf / the definition-based tdist_cdf_ref
  integration, independent of rt.c's gamma loop and ibeta CDF; quant-fuzz
  runs 10 series x 105 rows (1050 checks) with (0,1) share/probability
  and finiteness assertions.
- [x] **Bootstrap / resampling family (2026-10-31).**
  `bootstrap_mean_ci(list, n_iter, alpha)` / `bootstrap_median_ci` /
  `bootstrap_stddev_ci` answer [lo, hi] by the percentile method: each
  replicate resamples the list with n iid plain-uniform indices
  ((int64)(u*n), u = top-24 unbiased draw), evaluates mean / median
  (even count averaged) / Welford sample stddev (n-1), sorts the n_iter
  values and linearly interpolates the alpha/2 and 1-alpha/2 quantiles
  (pandas rule, identical to lume_bi_percentile) — a two-element float
  list out. `bootstrap_se_mean(list, n_iter)` returns the sample stddev
  (n-1) of the replicate means (Efron convention) as a double. Invalid
  parameters (n<2, n_iter<1, alpha outside (0,1), non-finite) answer an
  empty list / NAN. tests/bs-smoke.lume pins 7 rows of one continuous
  seed-42 stream; quant-consistency re-seeds 42 before every call and
  pins 5 more rows (125 total, three legs byte-identical);
  quant-refcheck adds 5 absolute-stream cases (132 total) — pcg_bootstrap
  _ci / pcg_bootstrap_se replay idx = int(u*n) position-by-position,
  independent of rt.c's qsort and Welford loops; quant-fuzz runs 10
  series x 110 rows (1100 checks) with finite / positivity assertions.
- [x] **Portfolio / risk metrics (2026-10-31).** `value_at_risk(list,
  level)` answers the negative of the level-quantile (pandas linear
  interpolation, lume_bi_percentile's rule — a positive loss figure);
  `expected_shortfall(list, level)` the negative mean of every
  observation at or below that quantile (tail conditional expectation);
  `sharpe_ratio(list, rf)` = (mean - rf) / sample stddev (n-1, Welford);
  `sortino_ratio(list, rf)` = (mean - rf) / downside deviation
  sqrt(sum(min(0, x-rf)^2)/n) (population n). Deterministic — no RNG, so
  the rows need no re-seeding. Zero volatility / zero downside
  deviation answers NAN on every leg (a data property, not a parameter
  error — fuzz series with no spread stay runnable); invalid parameters
  (level outside (0,1), n<2, non-finite rf) error on the interpreter,
  NAN natively. Emission shares bootstrap_se_mean's (list, scalar) ->
  double block on both native backends (fn dispatch). tests/va-smoke.lume
  pins 6 rows on all three legs; quant-consistency grows to 131 rows
  (three legs byte-identical); quant-refcheck adds 6 mathematical cases
  (138 total) — es_tail / sharpe_ratio_ref / sortino_ratio_ref recompute
  in Python; quant-fuzz runs 10 series x 114 rows (1140 checks) with
  finite / NAN agreement. With this the higher-level portfolio /
  backtest direction called out after the bootstrap family lands; the
  natural next step is sim-portfolio composition on top of the existing
  path simulators and these metrics.
- [x] **Portfolio layer: `sim_portfolio(paths, weights)` (2026-11-01).**
  Deterministic (no RNG) combination of equal-length price paths into a
  portfolio-return series: paths is a list of strictly-positive finite
  price paths (each a list, length >= 2), weights one finite number per
  path (sum NOT normalised — equal weight means [1/n]*n); answers a
  fresh float list of r_t = sum_i w_i * (p_{i,t}/p_{i,t-1} - 1), t =
  1..len-1, feeding the #48 metrics and max_drawdown. Invalid shapes
  error on the interpreter, empty list natively. Native nested reads
  walk LUME_SLOT_OBJ slots via bi_list_at_list; emission is two i8*
  list args -> fresh i8* list on both backends. tests/pf-smoke.lume
  pins 6 rows on all three legs; quant-consistency grows to 137 rows
  (deterministic, no re-seeding); quant-refcheck adds 6 cases (144
  total) — sim_portfolio_ref recomputes weighted returns in Python;
  quant-fuzz grows to 10 series x 119 rows (1190 checks, two seeded
  positive price paths per series). The quant/portfolio surface now
  covers simulation, statistics, risk metrics and composition; the next
  natural step is backtesting-shaped builtins (rolling portfolio
  evaluation on windowed returns) or the P6 sql_tx_query leftover.
- [x] **`sql_tx_query(tx, sql[, params])` — transaction read-your-writes
  (2026-11-02).** `sql_query` opens a fresh read-only connection per call,
  so rows written (but not committed) by sql_tx_exec on the transaction's
  private connection were invisible to it; sql_tx_query prepares the
  SELECT on the transaction's own connection instead — same row-map list
  shape (200-row cap, optional params), same read whitelist as sql_query
  (a write statement is rejected and the tx stays OPEN like sql_tx_exec).
  Three backends: native_sql_tx_query (builtins_sql.c),
  lume_bi_sql_tx_query (bridge_native.c), text/LLVM emission (two/three
  i8* args -> fresh i8* row list) + typecheck whitelist/arity 2-3.
  tests/txq-smoke.lume pins read-back/rollback/commit on all three legs
  (byte-identical); tests/sqlite.lume +6 check() cases; tests/
  native-sql-consistency.lume/.expected grows to 12 rows, all three legs
  agree. This closes the last named open builtin in TODO — the SQL
  surface (query/write/tx + read-your-writes) is now complete.
- [x] **Rolling window portfolio evaluation: `rolling_sharpe` /
  `rolling_sortino` / `rolling_var` / `rolling_cvar` (2026-11-02).**
  The backtesting shape: each builtin takes (returns, window, scalar) —
  window in [2, len], scalar rf (Sharpe/Sortino) or level in (0,1)
  (VaR/CVaR) — and answers a fresh float list with the exact #48
  formula on every window; the first window-1 slots are NAN (rolling
  preamble), zero-spread windows answer NAN (data property). Invalid
  window / non-finite scalar error on the interpreter, empty list
  natively. New emission group (list, i64, double) -> fresh float list
  on text + LLVM (window truncates via lume_bi_ftoi, scalar widens via
  lume_bi_float); inference joins the float list group.
  tests/r51-smoke.lume pins preamble NAN + last-window values on all
  three legs (Python recomputes: 0.264906 / 0.57735 / 0.005 / 0.02);
  quant-consistency grows to 145 rows; quant-refcheck adds 5 cases (149
  total) with rolling_*_ref; quant-fuzz grows to 10 series x 123 rows
  (1230 checks). The quant/portfolio surface now spans simulation,
  statistics, risk metrics, composition and rolling evaluation — a full
  single-asset backtest loop (sim_gbm -> pct_change -> rolling metrics)
  can be written in ~15 lines of Lume.
- [x] **Technical indicators — the signal layer: `rsi` / `macd` /
  `bollinger` (2026-11-02).** Wilder RSI (window preamble NAN, Wilder
  alpha 1/w, avgLoss==0 -> 100, flat -> 50); MACD (EMA diff + signal +
  hist, anonymous struct of three full-length float lists); Bollinger
  (rolling mean ± k × sample stddev, window-1 NANs). Deterministic, no
  RNG. Emission: rsi joins the (list, i64) float-list group; macd and
  bollinger join the anon-struct group (i8* returns) on both emitters.
  **Fixed en route**: get(map, "k", []) answered the maps' anon struct
  so indexing failed on the native backends — the list flavour now
  answers list<any>. Verification: r52-smoke (3 legs byte-identical,
  Python recomputes the pinned rows), quant-consistency 145 -> 154
  rows, quant-refcheck 149 -> 152 cases, quant-fuzz 10 series x 126
  rows (1260 checks), native_backends 18/19 (serve errno 10038 known).
  The quant surface now spans simulation -> statistics -> risk ->
  portfolio -> rolling evaluation -> signals: a full strategy chain in
  Lume.
- [x] **Performance attribution — `alpha_beta` / `information_ratio` /
  `calmar_ratio` (2026-11-02).** The benchmark-aware evaluation layer
  that completes the #48 risk family: alpha/beta vs a benchmark
  (joint-Welford cov/var, per-period alpha, NAN on zero-variance
  benchmark), information ratio (excess mean over sample tracking
  error, NAN when identical), Calmar ratio (mean pct_change over max
  drawdown percent, NAN on monotonic paths). Emission: alpha_beta in
  the anon-struct group, information_ratio in the (list, list) block,
  calmar_ratio in the mean (list) block, both emitters. Verification:
  r53-smoke (3 legs byte-identical, Python recomputes alpha 0.0064 /
  beta 1.2 / IR 0.358057 / Calmar 1.0139 + 3 NAN properties),
  quant-consistency 154 -> 162 rows, quant-refcheck 152 -> 156 cases,
  quant-fuzz 10 series x 130 rows (1300 checks), native_backends 18/19
  (serve errno 10038 known). New `examples/portfolio.lume`: one full
  strategy chain (sim_gbm -> pct_change -> sim_portfolio -> risk ->
  rolling -> RSI -> attribution) byte-identical on all three backends.
- [x] **Time series & ratios: `autocorrelation` / `treynor_ratio` /
  `omega_ratio` (2026-11-02).** Lag-k ACF on the aligned segment
  (joint-Welford cov/var, white-noise / residual check); Treynor ratio
  reusing the alpha_beta beta (reward per unit of systematic risk);
  Omega ratio at a threshold (full-distribution gain/loss). All NAN
  data properties (zero variance / zero beta / zero loss side). Emission:
  omega joins percentile's (list, scalar) block, autocorrelation gets
  (list, i64) -> double, treynor (list, list, double) -> double, both
  emitters. Verification: r54-smoke (3 legs byte-identical, Python
  recomputes ACF -0.72093 / 0.147059, Treynor 0.0066667, Omega 3 /
  0.25), quant-consistency 162 -> 170 rows, quant-refcheck 156 -> 161
  cases, quant-fuzz 10 series x 133 rows (1330 checks), native_backends
  18/19 (serve errno 10038 known).
- [x] **Rank statistics & signal evaluation: `rank` / `spearman` / `ic`
  (2026-11-02).** Average ranks with ties sharing the mean; Spearman as
  the Pearson over the rank pairs (joint-Welford); IC as the rank
  correlation between a signal and forward returns at lag k. Constant
  sides answer NAN (data property). Emission: rank joins the pct_change
  float-list block, spearman the covariance double block, ic a new
  (list, list, i64) -> double block, both emitters. Verification:
  r55-smoke (3 legs byte-identical, Python recomputes ranks [3.5,1,2,3.5]
  / [2,2,2], Spearman -1 / 0.833333, IC 1 / 0.8), quant-consistency
  170 -> 178 rows, quant-refcheck 161 -> 166 cases, quant-fuzz 10 series
  x 136 rows (1360 checks), native_backends 18/19 (serve errno 10038
  known).
- [x] **Annualization & return families: `annualized_return` /
  `annualized_volatility` / `sharpe_annualized` / `cumulative_return`
  (2026-11-02).** Geometric annualized return and cumulative return on
  strictly positive prices (non-positive -> NAN), the sqrt(252)
  annualized volatility and the annualized Sharpe (reuses #48).
  Emission: price/return scalars join the mean block, sharpe_annualized
  the bootstrap_se_mean block, both emitters. Verification: r56-smoke
  (3 legs byte-identical, Python recomputes 0.0192449 / 0.332986 /
  7.56787 / 6.0543 / 0.1 / 0.125), quant-consistency 178 -> 186 rows,
  quant-refcheck 166 -> 171 cases, quant-fuzz 10 series x 140 rows
  (1400 checks), native_backends 18/19 (serve errno 10038 known).
- [x] **Volatility & risk derivatives: `semivariance` /
  `downside_deviation` / `param_var` / `kelly_fraction` (2026-11-02).**
  Mean-based lower-part moments (semivariance, the Sortino denominator
  exposed as downside_deviation), the normal-model VaR -(mu + z*sigma)
  with the #37 norminv z-score and the #48 positive-loss convention,
  and the continuous Kelly fraction mu/sigma^2 (zero variance -> NAN).
  Emission: the two scalars join the mean block, the two
  (list, scalar) forms join the bootstrap_se_mean block, both
  emitters. Verification: r57-smoke (3 legs byte-identical, Python
  recomputes 0.000216667 / 0.00912871 / 0.0245027 / 22.7273 / 0 / 0 /
  -0.01 / NAN), quant-consistency 186 -> 194 rows, quant-refcheck
  171 -> 176 cases, quant-fuzz 10 series x 144 rows (1440 checks),
  native_backends 18/19 (serve errno 10038 known).
- [x] **Trading statistics: `profit_factor` / `win_rate` / `avg_win` /
  `avg_loss` / `max_consecutive_losses` (2026-11-02).** The
  backtest-quality layer: gross-wins-over-gross-losses, the fraction
  of strictly positive returns, the mean of each side and the longest
  strictly-negative streak. Data properties: no loss side -> NAN
  (profit factor / avg_loss) with all-loss -> 0 for the factor; empty
  win side -> NAN for avg_win; no losses -> 0 for the streak. All five
  emit through the mean (i8*) -> double block, both emitters.
  Verification: r58-smoke (3 legs byte-identical, Python recomputes
  2.33333 / 0.571429 / 0.035 / -0.02 / 2 / 0 / NAN / NAN / NAN / 0),
  quant-consistency 194 -> 204 rows, quant-refcheck 176 -> 181 cases,
  quant-fuzz 10 series x 149 rows (1490 checks),
  native_backends 18/19 (serve errno 10038 known).
- [x] **Quant surface spec: `docs/SPEC.md` §6.7 量化与统计
  (2026-11-02).** Normative documentation for the 100+ quant builtins
  across all seven families, with the cross-cutting contracts: float
  returns; `nan` as a data property (zero sigma / downside / benchmark
  variance / beta / loss side / drawdown / non-positive price / empty
  side / zero Kelly variance) vs invalid-parameter errors (interpreter
  raises, native answers nan); rolling padding; deterministic PCG32
  RNG. Section renumbering 6.7 -> 6.8 ... 6.10 -> 6.11 and the stale
  §6.0 reference fixed. Worked example triple-checks profit_factor = 4.
- [x] **Regression harness: Windows python fallback in
  `tests/native_backends.sh` (2026-11-02).** Probe falls back to
  LOCALAPPDATA Programs/Python/Python3*/python.exe when bare
  python3/python fail, so a Windows install with an
  App-Execution-Alias stub no longer silently skips the quant
  reference leg.
- [x] **F-test family: `f_stat` / `f_pvalue` (2026-11-02).** The
  variance-comparison test missing from the z/t/chi2 set: `f_stat(a,b)`
  answers the sample-variance ratio s1²/s2² (n−1 denominators via
  lume_bi_variance, so df = n−1 each side; zero-variance denominator ->
  NAN as a data property), `f_pvalue(F,df1,df2)` the F upper tail
  1 − I_x(df1/2,df2/2), x = F·df1/(F·df1+df2), through lume_bi_ibeta
  (ULP guard at 0). Emission: two-list -> double hand-written block on
  both emitters (chisq_stat's pattern; LLVM must use rt_decl, never free
  the ValueRef operands) plus a scalar-driven table row for f_pvalue
  (3 doubles -> double); rt.c forward-declares lume_bi_variance for the
  native runtime compile. Verification: r60-smoke 3 legs byte-identical
  (1 / 0.0672043 / 0 / nan / 0.464758 / 0.171067 / 0.0567628),
  quant-consistency 204 -> 212 rows, quant-refcheck 181 -> 190 cases
  (f_pvalue references definition-based: F-density integration in log-x,
  cross-checked via the F(1,d2)=t(d2)² identity), quant-fuzz RPP 149 ->
  151 with the two rows appended at the dataset tail (1510 checks, 0
  failures), native_backends 29 ok (serve errno 10038 known).

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

## P6 — Cross-statement transactions (closed 2026-10-23)

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
  closes via the OBJ_SQLTX finalizer). Fixed 2026-10-23: an open-handle
  registry (`g_open_tx[64]` + register/unregister in bridge_native.c)
  plus `lume_bi_sql_tx_cleanup()` — the serve loop rolls back + closes
  every still-open handle at the request boundary, so a handler that
  drops its handle cannot pin the connection (and the BEGIN lock) for
  the life of the process. Regression: tests/native_backends.sh serve
  leg (`tests/native-sql-tx-serve.lume` — two sequential /open requests
  both get 200 with a fresh "ok" body; without the reclaim the second
  BEGIN would time out on SQLITE_BUSY). Verified 2026-10-23 with a real
  libsqlite3 (`SQLITE_PREFIX=/d/Software/mingw64/opt`): sql interp/text/
  llvm legs and the whole native_backends.sh sql block are green on this
  host; the serve leg itself cannot run in this session — the local
  runtime sandbox denies raw socket creation (even Python's socket()
  fails WSAENOTSOCK 10038), so the listener never comes up. Linux CI
  runs the serve leg (no sandbox). Fixed en route: sql row maps
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

Leftover gap (2026-10-23, surfaced by the ai-produce CI failure): the
tx handle's connection is private to sql_tx_* — `sql_query` opens a
fresh read-only connection per call and shares no transaction state
with an open tx, so rows written inside an open transaction are
invisible to `sql_query` until commit (SQLite's default isolation).
The gen-sql-tx seed originally read back inside the transaction; the
machine gate judged it a verdict violation on every platform
(reproduced locally: "no such table: t"). The seed now commits before
the read-back. If "read your writes inside a transaction" should be
expressible, a `sql_tx_query(tx, sql)` builtin (interp + text + llvm +
guard + tests) is the follow-up.

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
reports. `make asan`'s remaining red was the pre-existing tre-regex
UBSan misaligned-read noise on the interp leg (`regexec.c:264`,
documented under P3/P11) — **fixed 2026-10-23**: the scratch buffer in
`tre_tnfa_run_parallel` was aligned to `long` (32-bit on Windows
LLP64) instead of pointer width, so `tre_tnfa_reach_t{state; tags}`
landed off the 8-byte boundary. `regexec.c` (4 call sites) and
`tre-mem.c` now use `void *`; `make asan`'s emitter gate is green and
the interp leg no longer reports UBSan. Windows-local limit (Linux CI
authoritative): `tests/smoke-bin-asan` still OOMs inside the
allocation-heavy tests (Windows ASan runtime shadow-map exhaustion,
"internal allocator is out of memory trying to allocate 0x70 bytes",
not parameter-fixable) and `tests/lsp_smoke.sh` cannot fork the ASan
binary under MSYS2. En route the
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
- No unguarded shell primitive: a child process is started only through a
  gated, rooted `spawn()` builtin (P14), never a language keyword or an
  implicit capability. `run()` is the agent loop, not shell execution — a
  process must call `spawn()` explicitly, under a `LUME_SPAWN_ROOT` allowlist
  with a no-spawn default, exactly like `--no-fs` / `LUME_FS_ROOT`.

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

## P12 — Native backends SIGSEGV on key access to a map function parameter (filed + FIXED 2026-10-28)

Found while porting a peer-to-peer module (`lib/peer.lume`) that passes the
request map into helpers. **Interpreters are fine; both native backends crash.**

Minimal repro:

```lume
func f(m, k: string): string { return str(get(m, k, "d")); }
print(f({ k: "v" }, "k"));
```

- `lume-core repro.lume` → prints `v` (correct)
- `lume-core --compile-text repro.lume && ./out/...` → **SIGSEGV**
- `--compile-llvm` → same

Scope narrowed by hand:

| Case | Native result |
|---|---|
| pass a map to a function, never touch it | ok |
| `print(m)` inside the callee | ok |
| `get(m, k, d)` inside the callee | **SIGSEGV** |
| `keys(m)` inside the callee | **SIGSEGV** |
| `m["a"]` inside the callee | **SIGSEGV** |

So it is not the call itself nor the annotation (unannotated `m`, `m: map`
both crash) — it is *any key read of a map that arrived as a parameter*.

Why it went unnoticed: `tests/native-consistency.lume` has **zero** cases
passing a map into a function (`grep -c "func.*: map"` → 0), so the
three-backend invariant never covered this shape.

**Root cause (confirmed, not the suspected one):** the parameter really is
the value, but `cg_var` loaded it anyway — the read path treated every
`%lv_`-style slot as a stack cell. For an opaque-pointer parameter the slot
already holds the heap pointer, so `load i8*, i8** %p0` reinterpreted the
first word of the LumeMap header as a pointer and faulted on first key use.

**Fix.** Both backends now mark opaque-pointer parameters as
"slot IS the value" and skip the load:

- text: `codegen_expr.c` `cg_var` returns the `%p` slot directly.
- libLLVM: `Asg.param` (new field, zeroed in `asg_push`) +
  `ptr_like_param()` (`llvm_cg_stmt.inc`) makes `llvm_cg_expr.inc`'s `cg_var`
  return the slot; the entry-block alloca/seed loops skip those parameters.
  A *named* struct parameter keeps the original rule (slot = incoming
  pointer, dereferenced for field access) — narrowing that condition to
  opaque kinds only broke `p.age`-style reads and was caught by the
  consistency suite.

- [x] Fix the codegen.
- [x] Add a `native-consistency` case (P12 block: `get` / `keys` on a map
      parameter, plus the default-value arm).
- [x] Audit the other container kinds (list, struct) as parameters for the
      same shape. List parameters were exercised while narrowing the fix and
      behave; a dedicated case is still worth adding (`p12_list` was dropped
      because `list` annotations collide with `int[]` at the call site).
      **Done 2026-11-04 as an audit + close**: the coverage already exists.
      Map parameters are pinned by the P12 block (`p12_get` / `p12_keys`);
      list/`int[]` parameters by the `tc_apply` / `tc_apply2` / `tc_applyv`
      trio (all three native backends); named-struct parameters by the
      `p.age`-style regression that caught the over-narrowed fix. `int[]`
      and `list` share the same heap-object slot shape, so a separate
      `p12_list` case would duplicate the `tc_apply` trio, not add a new
      shape — closed as a decision, not re-filed.

Impact: any library module whose helpers take a map (very common — request
maps, config maps) cannot be compiled by the native backends today. The
interpreter and `server{}` (which runs on the interpreter/bridge path) are
unaffected, which is why app-level code did not surface it.## P13 — Language gaps exposed by the fastapi-web migration (filed 2026-10-30)

Filed 2026-10-30 while scoping a FastAPI service onto a lume DSL server
(`personal/web-services/fastapi-web`). The framework side turned out to
cover most of it — MySQL is a real backend, static serving has
gzip/ETag/Range, htpasswd/bcrypt/session cookies exist. What the
**language** is missing is four things; two are an afternoon, one is a
real project, one is a policy decision. Cross-refs: agent-httpd
`TODO.md` P2 (the HTTP-side security items) and lume host `TODO.md` P13
(the dependent-project disposition).

Two constraints apply to every item below:

1. **Five coordinated edits per builtin** — `LUME_BUILTIN_NAMES` and the
   arity contract in `typecheck.c`, the dispatch table in `interp.c`, the
   `b_*` wrapper in `builtins.c`, and the prototype in
   `builtins_internal.h`. The single-source-of-truth list already exists
   for the first two; the other three are still hand-maintained, which is
   how a builtin ends up callable but untypable.

2. **Land in core, then `make sync-lang`** into the host. A builtin added
   in the host's `lang/` becomes DRIFT on the next sync (see lume host
   `TODO.md` P13 for the in-flight warmup change, which is exactly this
   shape).

### Password verification — the one item with no workaround

`builtins_crypt.c` has `crypt_sha512` (hash generation only) and
`builtins_crypt.c`/`builtins_math.c` provide `sha256` +
`hmac_sha256` + `base64_*`. There is **no verify side and no
constant-time compare**, so a DSL program can create a password hash but
cannot check one. Consequence is concrete: every login in the affected
project has to stay at the framework gate (htpasswd + `htpasswd -B`),
which means fixed accounts and no self-service registration, no password
change, no per-user rows.

- [x] Add `crypt_verify(hash, password)` → bool. Dispatch on the `$N$`
      prefix: `$5$`/`$6$` via `crypt(3)`, `$2a$`/`$2b$`/`$2y$` via the
      bundled bcrypt. Reject plaintext/`{SHA}`/DES/`$1$`/`$apr1$` in
      production builds (same policy as agent-httpd's `is_strong_hash`),
      with the existing dev escape hatch if one is needed.
      **Done 2026-11-04**: `crypt_verify(hash, pw)` in `builtins_crypt.c`,
      dispatched as specified; weak hashes return `false` (fail-closed, no
      reason leaked). `$2*` verdicts agree across interpreter and both
      native emitters — platform-independent probes in
      `tests/native-consistency.lume`, the glibc-only `$6$` success loop in
      `tests/test-crypt.lume`.
- [x] Add `const_time_eq(a, b)` → bool: equal-length branchless compare,
      length itself is not secret. Needed both for the above and for any
      token/CSRF compare written in the DSL.
      **Done 2026-11-04**: `const_time_eq(a, b)` in `builtins_crypt.c`
      (NUL-safe via `obj_string_len`); unit cases in `test-crypt.lume`,
      cross-backend probes in `native-consistency.lume` + `.expected`.
- [x] bcrypt lives only in `agent-httpd/src/security/bcrypt.c` today, and
      the host links it — decide whether core vendors it (a second copy
      is the thing `make sync-lang` exists to avoid) or whether
      `crypt_verify` is host-only with a documented "this build has no
      `$2*`" limitation. The native-backend (`--compile-text` /
      `--compile-llvm`) and interpreter paths must agree on the verdict
      either way, so this needs a `native-consistency` case.
      **Done 2026-11-04**: decided **no vendor** — a second bcrypt copy is
      precisely the drift `make sync-lang` exists to prevent. Core's
      `crypt_verify` rejects `$2*` with an explicit documented error
      ("support lives in the host build — lume links agent-httpd's
      bcrypt"); host gains the strong symbol via `sync-lang`. Both native
      emitters share the same builtin table, so the `native-consistency`
      probes (weak-reject, `$2*` error, wrong-password `$6$`) pin the
      verdict agreement.

### Request bodies — multipart, and the 64 KiB ceiling behind it

Neither tree contains `multipart`/`boundary`/`Content-Disposition`
handling, and `MAX_REQUEST_SIZE` (64 KiB, single read buffer,
`agent-httpd/src/httpd.h:8`) means even a correct parser could not
receive a photo. Uploading is therefore blocked by *two* layers, and the
second is architectural: the 413 path force-closes and the body is a
stack buffer, so "parse multipart" without "stream the body to a temp
file" buys nothing.

**Status 2026-11-09:** the paragraph above is the 2026-10-30 reasoning
record, kept as-is. Both halves have now landed, in the order the warning
below demanded — (b) on 2026-11-04, (a) on 2026-11-09. Uploads are no
longer blocked in either tree.

- [x] **Done 2026-11-09 (and correction to the old warning):** (a) the
      `multipart(body, content_type)` builtin landed, but only after (b),
      so the 60 KiB-trap the item warned about cannot recur. It is a
      **three-backend** builtin (not an interpreter-only primitive like
      P14 `spawn` / P17 `http_stream_*`): `typecheck.c` name + arity,
      `interp.c` registry, `builtins.c` / `builtins.h` interpreter native
      + `b_multipart`, `rt.c` `lume_bi_multipart`, `codegen_call.c`
      declare/dispatch, `llvm_cg_call.inc` dispatch, and
      `codegen_scan.c` / `llvm_cg_scan.inc` `list(any)` inference. The
      RFC 7578 parser is a `(buf, len)` walker duplicated in
      `builtins.c`, `rt.c` and the host `lang/builtins.c` — `rt.c` is a
      self-contained TU compiled on its own by `backend.c`, so a shared
      header would have to ship with every packed build; this follows the
      `native_str_match` / `lume_bi_str_match` precedent instead, and the
      `multipart` leg of `tests/native_backends.sh` diffs all three so a
      copy that drifts turns `make test` red.

      Returns a list of `{name, filename, type, body}` maps. Absent
      `filename` / `type` are `""`, **not null**: the native anon-row
      getter `lume_map_get_s()` collapses a null OBJ slot to `""`, so a
      null there would make the same program disagree across backends.
      Handles quoted boundaries, case-insensitive header names, CRLF and
      bare LF, preamble/epilogue, and backslash-escaped quoted parameter
      values; rejects wrong media type, missing/empty/overlong boundary,
      missing delimiter or closing boundary, and unterminated part
      headers through the VM error channel (`vm_set_error` /
      `lume_error_set`, so `try()` catches it). Bounded at 4096 parts and
      a 200-byte boundary.

      **Binary boundary (deliberate, not a TODO):** the interpreter's
      strings carry a length, so `native_multipart` uses
      `obj_string_len()` and a NUL-containing part body survives intact
      (`tests/multipart.lume` pins a 3-byte `A\0B` part and a 128 KiB
      part). The compiled runtime's `LumeSlot.str` is a NUL-terminated C
      string with no length field, so `lume_bi_multipart` passes
      `strlen(body)` and a compiled program truncates such a part at the
      first NUL — the same platform boundary `read_file` / `str_match`
      already have. Making compiled strings binary-safe would mean
      redesigning `rt.c`'s value representation, not this builtin.

      Tests: `tests/multipart.lume` (35 interpreter assertions, `make
      multipart-test`, wired into both `make check` — the Windows lane —
      and `make test` — the POSIX CI lane) and the `multipart` leg of
      `tests/native_backends.sh` (`make native-consistency`, also part of
      `make test`) for interpreter / IR text / libLLVM verdict parity.
      Host: hand-merged into all four
      `host_owned` files, `lang/bridge.c` now exposes `req.body` with
      `content_length` instead of `strlen` so binary parts reach the
      parser, and `examples/upload-multipart.lume` shows the buffered /
      spooled-body pattern.
- [x] **Done 2026-11-04**: (b) the transport redesign landed: resumable
      body-to-tempfile in agent-httpd. Content-Length bodies above the
      64 KiB read buffer (`MAX_REQUEST_SIZE`) but within
      `MAX_BODY_SPOOL_BYTES` (default 8 MiB; `MAX_BODY_SPOOL_BYTES` /
      `BODY_SPOOL_DIR` envs) are streamed to a `mkstemp` temp file,
      `req->body` stays NULL and `req->body_temp_path` names the file; the
      declared length is fully consumed so keep-alive/pipelining stays
      exact. Over the cap keeps the historical 413 + forced close; spool
      failures map to 408 (deadline/select timeout) / 400 (vanished
      socket) / 500 (I/O), all force-close; the temp file is unlinked at
      request teardown. CGI and FastCGI consume the spooled body in
      64 KiB chunks (CGI: polled pipe writes, same discipline as the
      buffered path). The lume bridge exposes `req.body_temp_path`, so a
      handler reads the body with `read_file()` — capped at 16 MiB, which
      still outgrows the old 64 KiB ceiling by 256×. The chunked path
      deliberately keeps its 64 KiB ceiling: `http_dechunk_feed` returns
      -2 once the decoded body would exceed cap and cannot resume
      mid-chunk, so streaming chunked bodies would need a decoder change
      of its own (tracked separately). Delivered
      `lume/examples/upload-spool.lume` + `run_all.sh` legs 4h/4i (spool
      under cap, buffered small body, over-cap 413) and agent-httpd
      `tests/test_spool_body.c` (real-server: 100 KiB spool + same-connection
      keep-alive GET in sync, 300 KiB over a 256 KiB cap -> 413, no
      leftover `body.*` temp files). `lume --check` passes; Linux CI runs
      `make test-spool` and the run_all legs.
- [x] **Done 2026-11-04**: Downloads were **not** blocked: Lume strings
      are NUL-safe, so `read_file` + a `{type, headers, body}` envelope
      already serves binary. Delivered `lume/examples/download.lume` (CSV
      + octet-stream routes, try()/err 404 branch) and a `run_all.sh` leg
      4g that pins body + Content-Type over real HTTP; `lume --check`
      passes. Projects that only move files out should use this instead of
      reaching for uploads.

### `no_fs` was all-or-nothing — now the LUME_FS_ROOT allowlist is the middle setting

`fs_permitted()` (`builtins_fs.c`) only consults `vm->no_fs`
(`--no-fs` / `LUME_NO_FS=1`). `read_file` / `write_file` / `files` accept
any path the process can reach, and under `agent-httpd` the POST worker
shares a UID with the server — so one string-concatenation bug in a
handler reads `AUTH_SESSION_FILE` (every live bearer token), the
htpasswd file, and `.env` (DB credentials). `env()` is likewise
unfiltered.

- [x] Add a path allowlist mode: `LUME_FS_ROOT=<dir>` restricts
      `read_file`/`write_file`/`files` to realpath-checked descendants
      (reuse the boundary test from `agent-httpd/src/http/static.c`, not
      a bare `strncmp` prefix), and `LUME_FS_RO=1` for read-only.
      **Done 2026-11-04**: `fs_check_path()` realpath-checks the target (or,
      for a not-yet-existing write target, its parent — same reason
      `resolve_within()` realpaths before joining: .. and symlinks are
      already resolved, so this is not a prefix allowlist). Applies to
      `read_file`/`write_file`/`files`, and also `mkdir`/`lock_file` so a
      read-only/rooted run cannot create or lock anything outside either.
      Root resolution is a hard error at startup (a typo that silently
      falls back to unfiltered access would be the opposite of the point).
- [x] Keep `--no-fs` as the hard switch; the allowlist is the
      middle setting.
      **Done 2026-11-04**: `--no-fs` / `LUME_NO_FS=1` still refuse every
      path-taking builtin with the same runtime error; `LUME_FS_ROOT`
      only engages when `no_fs` is off, so a supervisor can stack
      `LUME_FS_ROOT` + `LUME_FS_RO=1` for read-only-rooted and still reach
      for `--no-fs` as the full lock.
- [x] Document the deployment-side mitigation that works today and is
      not obvious: run the server as its own unprivileged user, keep
      htpasswd / session / `.env` outside the docroot and unreadable by
      the docroot owner, and never let a handler accept an arbitrary path
      parameter (map an ID to a fixed directory instead). This is the
      difference between a bug and a disclosure.
      **Done 2026-11-04** (this paragraph + the two below): the allowlist
      is a second line of defense, not a replacement for the three rules
      above. In order of strength: (1) no arbitrary path parameters — a
      handler that maps an ID to a fixed directory has no path-injection
      surface at all; (2) OS-level separation — the server runs as its own
      unprivileged user, and htpasswd / session / `.env` live outside the
      docroot, unreadable by the docroot owner, so even a full docroot
      disclosure stays a content leak instead of a credential disclosure;
      (3) `LUME_FS_ROOT` + `LUME_FS_RO=1` shrink the blast radius of any
      remaining string-concatenation bug to the rooted directory, and
      `--no-fs` is the kill switch. A deployment should use (1) always,
      (2) always, and (3) whenever the server does not need general filesystem
      access.

### Deliberately not filed

Recorded so the next review does not re-open them (see "Intentional
boundaries" above): no ORM, no schema migrations, no OpenAPI generation,
no Redis client, no middleware chain. Each was considered for the
migration and rejected as a design trade for the zero-dependency
property — which is also what removes the whole Python dependency CVE
surface. `run()` is **not** shell execution (it is `bridge_run`, the
agent loop); the dangerous primitives are the file/`env` ones above.

## P14 — Sidecar execution layer: what a pure-lume-core executor needs (filed 2026-11-04)

Filed after the resolve-lume / lume-a2a C-A work: `task.delegate` routes a
job to a host **sidecar** (Node, `spawn('sh'|-c)`, `node:sqlite` job store,
SSE streaming, MCP client) because **lume-core has no process builtin**. The
question on the table: "replace the Node sidecar with lume-core?" Answer,
scoped here so a future review does not re-derive it: **the logic layer can
move to core today; the execution layer cannot, without these two additions.**

Ground truth (verified in `src/`, not from README memory):

| sidecar uses Node for | Node primitive | in lume-core? |
|---|---|---|
| job metadata store | `node:sqlite` | ✅ `builtins_sql.c` links `sqlite3` (P6 closed the tx surface) |
| HTTP routes / auth | `http.createServer` | ✅ `server{}` DSL + `exec_statement_route`/`_verbs`/`_tool` |
| 42 tool definitions | `builtin-tools.json` | ✅ `builtins_catalog.c` tool/skill registry |
| business rules / summaries | JS pure logic | ✅ `.lume` |
| **`sh -c` / `make` / `uv run python`** | `child_process.spawn` | ❌ no process builtin (searched `src/builtins*.c` + `interp.c`) |
| **SSE streaming a running job** | long-lived streamed response | ❌ `builtins_http.c` collects the whole body synchronously |
| **MCP streamableHttp client** | long-lived SSE client | ❌ same |
| **crash-restart launcher** | `spawn bash` + exit handler | ❌ no process management |

**Status of this snapshot (updated 2026-11-09).** The table is kept as
filed on 2026-11-04, not rewritten: the `sh -c` row is now closed by
P14's `spawn`, and the MCP streamableHttp client row is closed by P17's
`http_stream_*` (an inbound SSE client). The **SSE streaming a running
job** row is still open — P17 *receives* a stream, but nothing in core
*emits* a long-lived streamed response yet; the crash-restart launcher
also remains out of scope. Recorded here so the ❌ column is not read as
current ground truth.

So: migrate routing / SQLite / tool defs / summaries to `.lume` now; keep only
the three "hands" — **spawn a child, stream its output while it runs, hold a
long-lived MCP stream** — either in `agent-httpd` or a tiny Node shim. Two core
builtins close that gap without pulling the whole runtime in.

- [ ] **`spawn()` — gated, allowlisted, with a no-spawn default (the big one,
      ~a project).** Original shape: `spawn(args, {input?, env?})` returns
      `{pid, stdout_fd, stderr_fd}` with `proc_read` / `proc_wait` /
      `proc_kill` siblings. **Landed 2026-11-08: step 1 (the gate + the
      interpreter leg) and step 2 (root resolution, filtered `env`,
      generation-tagged handles).**
      **Status corrected 2026-11-09:** the old "Still open: the two native
      emitters, host integration and streaming" list is superseded.
      Decision (a) below closed the emitter question **permanently**
      (interpreter-only, enforced by `tests/spawn-native-gate.sh`; P17
      has its own `stream-native-gate.sh`), host integration is closed as
      a permanent core-only boundary in the host `TODO.md` P14 mirror,
      and `649f2df` landed streaming `proc_read` (a live child is tailed,
      not collected at exit). No builtin is missing. This item stays
      unchecked only because **Done when** below still asks for the
      `resolve-lume` sidecar to serve the same `/api/jobs` contract from
      a `.lume` executor — integration work, a separate project.

      **Verified 2026-11-09 against the remote project (read-only):** the
      `Done when` condition is still unmet. `erishen/resolve-lume@e90b5dc`
      (main, 2026-11-05) keeps the whole gateway in `sidecar/*.js`:
      `server.js` answers `GET /api/jobs` and `POST /api/jobs` and calls
      `server.listen`, while `store.js` persists jobs through
      `node:sqlite`'s `DatabaseSync` (WAL). Its own README lists the
      sidecar as a **Node** host-resident process on 127.0.0.1:3000 and
      credits `/api/jobs` persistence to `node:sqlite`; the
      "zero-Node-runtime-dependency" claim covers the backend/frontend
      containers, not that host sidecar. The three `.lume` files are
      `lume-tools/tools.lume` (five demo MCP tools), the generated
      `lume-tools/sidecar_ro.lume` (stdio MCP bridge onto the Node
      sidecar's 42 tools) and `peer_node.lume` (an A2A static peer) —
      none of them owns routes, SQLite, tool defs or summaries. So the
      remaining work is a rewrite/integration in that external repo,
      not a missing core builtin: gated `spawn`, streaming `proc_read`
      and P17's inbound SSE client are all shipped here. Keep this box
      unchecked until a `.lume` executor actually serves `/api/jobs`.
- [x] **Gate first, spawn second** (2026-11-08). Opt-IN `--spawn` /
      `LUME_SPAWN=1` **plus** a mandatory comma-separated
      `LUME_SPAWN_ALLOW`; enabling spawn with an empty allowlist permits
      nothing (fail closed). `spawn(argv)` hands a string list straight to
      `execv()` / `execve()` with a realpath-resolved absolute binary — no
      shell, no `system()`, no `execvp()`, no PATH search, no command line
      assembled from script text — and argv is length/type/size checked.
      `input` (<= 4096 bytes) and a filtered `env` map are supported
      (step 2, see below).
      Refusals answer `{ok:false, err}` so a supervisor can branch on them,
      and the error never echoes the rejected command back (no allowlist
      probing through error text). An ungated shell primitive is exactly the
      disclosure P5/P11/P13 and lume-a2a's `check_public.lume` guard against,
      so the gate landed before the spawn builtin, as the original item
      required. (The original text proposed `--no-spawn` / `LUME_NO_SPAWN`;
      that is an opt-OUT switch and would have left the pure binary spawnable
      by default — inverted to opt-IN.)
- [x] **The four interpreter builtins + the full registration chain**
      (2026-11-08). New `src/builtins_spawn.c`; `LUME_BUILTIN_NAMES` and
      `LUME_BUILTIN_SIG` in `typecheck.c`; dispatch in `interp.c` behind a
      `LUME_HAS_SPAWN` switch (same pattern as `LUME_HAS_HTTP`); `b_*`
      wrappers and Windows stubs in `builtins.c`; declarations in
      `builtins.h` / `builtins_internal.h`; `VM.spawn_enabled` /
      `spawn_allow` in `lume.h`; CLI + env parsing in `main.c`; Makefile
      SRCS, Windows `filter-out`, `CFLAGS`, `spawn-test`, `test:` and
      `.PHONY`. `make test` is green including `spawn-test` (five offline
      rounds, 52 assertions after step 2) and `native-consistency`
      (no regressions).
- [x] **Two native emitters + a native-consistency case — CLOSED by
      decision (a), 2026-11-08 (no emitter work will be done).** Adopted
      option (a): `spawn` is **interpreter-only**, permanently. A compiled
      lume binary has no `VM` (`spawn_enabled` / `spawn_allow` /
      `spawn_root` are VM fields) and `rt.c` has no gate of its own, so a
      native helper could only ever be an ungated process primitive.
      `make check` → `spawn-native-gate` now *enforces* the decision: it
      fails if a spawn token appears in `rt.c`, `codegen_call.c`,
      `llvm_codegen_internal.h`, `codegen_scan.c` or `llvm_cg_scan.inc`;
      asserts `--compile-text` still rejects
      `tests/spawn-native-denied.lume` with "call to unknown function";
      and (POSIX) asserts the interpreter leg can still spawn when gated,
      so the gate cannot be satisfied by deleting the builtin from the
      language. Both negative controls were replayed — an injected
      `"spawn"` token in `rt.c` turns it red, and a blinded matcher is
      caught by its own self-check. Option (b) remains available, but
      requires a real fail-closed startup policy in `rt.c` **and** a
      deliberate rewrite of this gate; it must not be started by merely
      declaring the helpers in the survey below. That survey is kept as
      the reasoning record, not as a work plan.

      Code survey
      ground truth, not README memory: `rt.c` is **not** in the main
      binary's `SRCS` (`backend.c` compiles it only for native output), so
      the process table cannot simply be shared between the interpreter and
      the emitters. The `BUILTINS[]` tables in `codegen_call.c` and
      `llvm_codegen_internal.h` only express homogeneous scalar signatures,
      so a map-returning helper needs the `kdj` / `alpha_beta` shape:
      `type_anon_struct()` inference in both `codegen_scan.c` and
      `llvm_cg_scan.inc`, a hand-written `declare i8*` in
      `emit_builtin_declares()`, a call branch in each emitter, and a
      `LumeMap*` helper in `rt.c`. LLVM declares helpers on demand, so it
      needs the call branch but no declare-table row.

      **Step-2 security finding (2026-11-08 — blocks a naive landing).**
      `src/rt.c` has **zero** `getenv` / `LUME_*` gate logic, and
      `codegen_call.c:3693` / `:3737` reject an unregistered builtin with
      "call to unknown function". So today a natively compiled binary
      simply cannot spawn — which is the *safe* default. Adding the helpers
      exactly as described above would instead produce a standalone binary
      with an ungated process primitive and no VM policy to consult,
      undoing the gate-first rule that P14 itself demands. Before landing
      the emitters, decide one of: (a) keep `spawn` interpreter-only and
      document that compiled lume binaries never gain a child-process
      surface; or (b) give `rt.c` its own fail-closed startup gate (read
      `LUME_SPAWN` / `LUME_SPAWN_ALLOW` / `LUME_SPAWN_ROOT` into a static
      policy, default deny) and prove it in a native case. Option (a) is
      smaller and matches the "no shell primitive" posture. This item
      must NOT be closed by merely declaring helpers. A byte-identical
      native-consistency case is in any case impossible while `spawn`
      returns a live OS pid — the opaque handle is a prerequisite either
      way.
- [x] **Host integration — CLOSED by a permanent core-only boundary,
      2026-11-08 (no hand-merge).** The host `lang/` build has neither the
      gate nor a use for a child-process surface, and with decision (a)
      there will be no native-emitter half to merge either. Copying the
      P14 registration chain (`typecheck.c` / `interp.c` / `builtins.c` /
      `lume.h` / `main.c` / `builtins_internal.h` / `builtins.h`) into
      host-owned files would add an **ungated** child-process primitive to
      a server binary, so it is deliberately NOT done. The host repo now
      records the boundary as permanent: `builtins_spawn.c` is a
      `CORE_ONLY_NAMES` entry carrying a decision note, the four spawn
      names in `tests/builtin-parity.allow` are marked PERMANENT rather
      than TRANSITIONAL, and the host `TODO.md` has a P14 mirror section.
      `lang/PIN.lag`'s accepted upstream-only deltas for those 7
      host_owned files are **intentionally retained**, not a pending
      merge — they are mostly the core-only spawn registrations. Re-opening
      this item requires landing an equivalent fail-closed gate in `lang/`
      FIRST, then removing the `CORE_ONLY_NAMES` entry, refreshing the
      parity allow list and `PIN.lag`, and documenting the policy. The
      pure `lume-core` binary stays no-spawn by default.
- [x] **`env` option + `LUME_SPAWN_ROOT` (step 2, 2026-11-08).** The old
      allowlist matched `argv[0]` exactly, but `execvp()` still searched
      `PATH`: a writable directory earlier in `PATH` could shadow an
      allowlisted name. `execvp()` is now gone entirely.
      `LUME_SPAWN_ROOT` is a colon-separated set of trusted executable
      directories (validated with `realpath()` + `S_ISDIR` at startup, an
      unresolvable entry is a hard error — the `LUME_FS_ROOT` precedent).
      Bare command names are resolved only inside those roots; **without a
      root they are refused**, and only an explicit (realpath-checked,
      `X_OK`-checked) absolute/relative path in the allowlist can run.
      Every candidate is realpath'ed again and boundary-checked, so a
      symlink inside a root cannot escape to a binary outside it. The
      original line above used `LUME_SPAWN_ROOT` for the *command list*;
      that is `LUME_SPAWN_ALLOW`, and `*_ROOT` keeps this repo's usual
      realpath-boundary meaning. Residual, documented: TOCTOU between the
      parent's `realpath` and the child's `execv` (a root that is writable
      by an attacker is already a broken deployment), and no `argv[0]`
      basename fallback for a command listed as a bare name.

      `spawn(argv,{input?,env?})` now accepts a string map. An explicit
      `env` does **not** inherit the whole parent environment: only a
      small baseline (`PATH`/`HOME`/locale/`TERM`/`TZ`/`USER`/...) plus
      the script's entries. `LD_` and `DYLD_` loader variables, `PATH`,
      `IFS`, `ENV`, `BASH_ENV`, `SHELL` overrides and non-string values
      are refused, and refusals never echo the rejected key. Without the
      option, behaviour stays `execv` with the inherited environment.
- [x] **Streaming tail of a spawned job (the `proc_read` half) — LANDED
      2026-11-08.** This is the `read_chunk(handle, timeout)` polling pair the
      item recommended: `proc_read(handle, "stdout"|"stderr", timeout_ms?)`
      already does a non-blocking poll + read (the read ends are `O_NONBLOCK`,
      the loop drains only what `poll()` reports available, and a timeout
      returns an empty `{ok,data:"",eof:false}` instead of blocking). What was
      missing was a regression test proving *incremental* tailing rather than a
      collect-at-end. New `stream` round in `tests/spawn-smoke.lume` (wired
      into `make spawn-test`): it spawns
      `sh -c '/bin/sleep 0.5; /usr/bin/printf "FIRST\n"; /bin/sleep 1.5;
      /usr/bin/printf "LAST\n"'` (external `printf` so dash's builtin
      pipe-buffering on Linux cannot mask a false green) and asserts the first
      non-empty read contains `FIRST` but not `LAST` (process still sleeping =
      real streaming), the empty poll is graceful, all chunks reassemble,
      `iters > 1` (multi-poll, not one-shot collect), and `eof` is observed.
      Verified on macOS/clang and a clean gcc:13-bookworm copy (dash as
      `/bin/sh`): both 8 assertions green. This closes the "a pure-core
      executor can **tail** a job" half of the item — `task.delegate`'s lazy
      `observe_delegate` can now read a streamed child instead of polling a
      status endpoint. CAVEAT (documented, not fixed): `proc_read` only ever
      surfaces bytes the child *wrote*; a child whose stdout is block-buffered
      by its own stdio (e.g. a Python script that never `flush()`es) will not
      stream until it flushes or exits. That is correct Unix tailer semantics,
      not a lume bug — callers must flush.
- [x] **`http_stream(url, {…})` / inbound SSE client — LANDED 2026-11-08
      as P17 (`f101133`).** Original 2026-11-04 assessment (kept as the
      reasoning record): `builtins_http.c`'s `http_get/post` collect the
      whole body before returning, so the executor cannot *receive* a
      response that streams (MCP `streamableHttp` SSE client rides on
      this). The estimate was that a new HTTP builtin would need either
      its own native-emitter registration — and therefore a fresh pass
      through the P14 emitter security decision — or an interpreter-only
      variant consistent with the no-async-runtime constraint, and that
      it was a separate project from the `proc_read` work above.
      **Landed:** `http_stream_open` / `http_stream_read` /
      `http_stream_close`, an interpreter-only SSE client with a
      generation-tagged slot table and an incremental line parser
      (`src/builtins_http.c`, +402 lines), reusing `url_parse`, the
      `host_blocked` SSRF gate, `open_conn` (proxy + TLS), `read_head`
      and `vm->no_net`. The emitter question was settled the same way as
      P14 — no `BUILTINS[]` registration, enforced by
      `tests/stream-native-gate.sh`. `http_stream_read` returns one
      parsed event per call; v1 does not auto-reconnect, handing back the
      last event id instead. See **P17** below. **Remaining:** the
      cross-platform verification named there — Linux `gcc:13-bookworm`
      (dash `/bin/sh`) and the Windows `LUME_HAS_HTTP=0` lane.
### 2026-11-08 — step 1 LANDED: the gate and the interpreter leg

**API deviation, deliberate.** The original spec above says `spawn()` returns
`{pid, stdout_fd, stderr_fd}` and that the return map "must agree
byte-identically on all three legs" of `native-consistency`. Those two
requirements cannot both hold: an OS pid and two pipe fds differ on every
run and in every process, so three independently executed binaries can
never print the same numbers. The shipped API returns
`{ok, handle, pid, err}` where `handle` is an **opaque, generation-tagged
id** (`generation * 32 + slot`) into a fixed 32-entry process table, and
`proc_read` / `proc_wait` / `proc_kill` all take that handle. A script never
holds a raw fd. Step 1 never reused slots, which capped a long-lived VM at
31 spawns — a sidecar executor would have died with "process table is
full" after 31 jobs. Step 2 reclaims a slot only after its child has been
waited for and bumps the generation, so a stale handle is rejected instead
of ever addressing a different child; `atexit` still kills + reaps
anything left behind. The deterministic id is also the only shape in which
the two native emitters could later agree byte-identically.

**Two documentation tensions found while implementing.** (1) The original
third item said to "hand-merge the native-emitter half into the host's
`lume/lang/`", but the host `PIN` / `sync-lang.py` rules classify every
`codegen*` / `llvm_*` file as `CORE_ONLY` — the host has no native backend
at all, so there is no emitter half to merge there. What the host needs is
the gate plus the registration chain. (2) The original allowlist name
`LUME_SPAWN_ROOT` described a *command list*, while every other `*_ROOT` in
this repo is a realpath boundary; the implementation uses
`LUME_SPAWN_ALLOW` for the list and reserves `LUME_SPAWN_ROOT` for the
realpath check that is still on the checklist above.

### 2026-11-08 — step 2 LANDED: no PATH, filtered env, reclaimable slots

`main.c` parses and validates `LUME_SPAWN_ROOT`; `builtins_spawn.c` gained
`resolve_exec()` (root lookup, `realpath` containment, `X_OK`, `execv` on
an absolute path) and `build_env()` (baseline + filtered explicit map,
`execve`). `lume.h` gained `VM.spawn_root`. The generation-tagged handle
and `slot_alloc()` reclamation above also landed here.

Verification (local, 2026-11-08): macOS clang `make -j4 all` and a clean
Ubuntu 24.04 / GCC 13 copy both build with **zero diagnostics** — the new
file uses `clock_gettime` / `poll` / `realpath` / `execve` / `environ`, so
the Linux leg is not optional. `make spawn-test` — five offline rounds
(locked / emptyallow / open / rooted / abspath) plus a bad-root startup
negative, **52 PASS assertions on both platforms**, covering the two gate
halves, fail-closed allowlist, argv type/length checks, stdout round-trip,
stdin `input`, stderr, `proc_wait` exit codes, `proc_kill` + reap (143),
unknown handles/streams, bare-name resolution inside roots, refusal of a
root-outside executable and a non-executable path, explicit absolute paths
with no root, `env` propagation + baseline filtering + `LD_PRELOAD` /
`PATH` / type refusals, 64 sequential spawns (slot reclamation) and a
recycled handle rejected by its generation tag. `make -j4 check` and the
full `make -j4 test` rc=0 including the existing `native-consistency`
three legs (no regression); host `make check-sync` rc=0 after the
transitional classification above.

One test-harness trap worth remembering: deriving the trusted directory
with `dirname $(command -v cat)` produced a WorkBuddy `brokered-bin` shim
directory on this machine, so `/echo` did not exist and three assertions
went red for a reason unrelated to the code. `SPAWN_ABS` is now the fixed
`/bin`, which realpath's into `/usr/bin` on usrmerge systems and stays
inside the `/bin:/usr/bin` roots on both platforms.

### 2026-11-08 — step 3 LANDED: emitter decision (a) + host boundary

No product C code changed in this step: the outcome is a **security decision
made enforceable**. `spawn` stays interpreter-only; a compiled lume binary
never gains a child-process surface. `tests/spawn-native-gate.sh` (wired
into `make check`, hence every CI lane) proves it three ways — the emitter
and `rt.c` files carry no spawn registration token, the text emitter
rejects `tests/spawn-native-denied.lume` with
"call to unknown function 'spawn'" (rc=1, no binary produced), and the
interpreter positive control (`tests/spawn-native-gate.lume`) still spawns
and waits when `LUME_SPAWN=1` + a one-command allowlist + two trusted
roots are supplied. A self-check plus two replayed negatives guard against
a false green.

Verification (local, 2026-11-08): `sh tests/spawn-native-gate.sh` rc=0 on
macOS/clang; `make -j4 check` rc=0 with the gate in the dependency graph
(also green under `-j4`, i.e. no race with `all`). Negative A: appending
`/* "spawn" */` to `src/rt.c` makes the gate fail with the exact line.
Negative B: replacing `SPAWN_TOKEN_RE` with a never-matching literal makes
the self-check fail. Both files were restored afterwards; `git status`
showed only the three new test files. Windows/mingw runs the static and
negative half and skips the POSIX positive control by `uname -s`.

Host side (lume): the transitional classification is gone —
`builtins_spawn.c` is a permanent `CORE_ONLY_NAMES` entry, the four spawn
names in `tests/builtin-parity.allow` are marked PERMANENT, and `TODO.md`
gained a P14 mirror section recording why the accepted `lang/PIN.lag`
deltas are deliberately not hand-merged. **Update 2026-11-09:** the
"Streaming remains the only open P14 engineering item below" note was
superseded by `649f2df` (2026-11-08), which landed streaming `proc_read`.
What remains is sidecar integration, not a missing core engineering item.

**Done when:** a resolve-lume sidecar whose logic is `.lume` (routes, SQLite,
tool defs, summaries) and whose only non-core surface is `spawn` + streaming,
serves the same `/api/jobs` contract the current Node sidecar does — and the
pure `lume-core` binary remains no-spawn by default. This is the path to
"replace the Node sidecar"; it is **not** "add a shell keyword".

Note for sizing (as filed 2026-11-04): `spawn` + gating is ~1 project (two
weeks), streaming is ~1 project; together they remove the last Node
dependency on the executor path. **Update 2026-11-09:** both landed on
2026-11-08 (`843194b`..`458feef` for spawn, `649f2df` for streaming
`proc_read`, `f101133` for P17's inbound SSE client). What still keeps the
Node shim in play is the sidecar integration in **Done when** above, not
an unimplemented builtin.

## P15 — `builtins_http.c`: `phost` use-after-scope fixed (2026-11-08)

`open_conn()` declared `char phost[512]` inside `if (proxy && proxy[0])`,
then kept `chost = phost` for the rest of the function: `getaddrinfo`, the
connect debug line, all `why` failure reasons and the CONNECT error
branches. GCC's `-Wdangling-pointer` was reporting a real read of a dead
stack buffer, not a cosmetic warning — the 2026-11-08 review had initially
filed it as "one of the two warnings blocking -Werror". `phost` now lives
at function scope with a comment pinning the reason. The host tree
(`lume/lang/builtins_http.c`) is host-owned and received the same
hand-merge; its `lang/PIN.lag` was refreshed with `sync-lang.py accept-lag`
(upstream-only lines for this file dropped 24 -> 7, because the same pass
also hand-merged core's `set_why()` into the host).

Verification: macOS clang and Ubuntu 24.04 / GCC 13.3 full builds with zero
diagnostics (host under `WERROR=1`, core under its normal flags);
`make -j4 check http-noproxy-test` rc=0 with the ambient proxy env cleared;
`LUME_HTTP_PROXY=http://127.0.0.1:9 NO_PROXY= ./bin/lume-core
examples/http-get.lume` logs `connect(127.0.0.1) ... Connection refused` —
the proxy hostname is read back intact after the `if` block, with no crash
and no garbage in the failure reason. The host's full `make test` is green
(`all tests passed`). No behaviour change on the success path: only the
lifetime of the proxy host buffer.

## P16 — lume 0.12 compatibility of `lib/` + conditional static responses (filed 2026-11-08)

Filed while migrating `work/education/lume-energy` from lume-core 0.13 to
lume 0.12.2. Two independent gaps surfaced; both are core-side, not
project-side.

### P16.1 `lib/*.lume` cannot be loaded by lume 0.12 (duplicate declarations) — CLOSED 2026-11-08

`lib/linalg.lume` `mat_qr_q()` declared `for (let i ...)` twice (lines 479/480;
`mat_qr_r()` repeated it at 513/515), so the first 0.12.2 import aborted with
`lume: line 480: duplicate declaration of 'i' in the same scope`. The precise
0.12.2 rule is **block** scope, not function scope as originally filed: a
`for (let X ...)` header is registered in the block *containing* the loop, so
two such loops in the same block collide while the same names in nested or
sibling blocks do not (verified against the released 0.12.2 binary). lume 0.13
has proper for-init block scoping and additionally rejects an inner loop that
shadows an outer local. Dropping the second `let` is NOT portable — 0.13 then
reports "assignment to undefined variable" — so every repeated loop variable
must be renamed together with the references in its header/body.

- [x] **Make the shipped libs parse under lume 0.12.2.** 47 + 8 mechanical
      renames in `sci.lume` / `optim.lume` / `linalg.lume` / `pde.lume`
      (`i`→`i2`..`i12`, `j`→`j2`.., `k`→`k2`.., `J`→`J2`); no expression,
      literal or comment text changed, no `let` removed. The three shipped
      libs now import cleanly on the released 0.12.2, and `entropy` /
      `poly_fit` / `linprog_simplex` / `mat_qr_q` give identical values on
      0.12.2 and 0.13. `pde.lume`'s duplicate declarations are cleared too,
      but see the builtin gap below.
- [x] **Regression gate: `tests/lib-compat.py` → `make test` → `lib-compat`.**
      Static leg (always runs, CI included): brace-aware, string/comment-
      masked scan of all 13 `lib/*.lume` for duplicate `let` names in one
      block, with a known-bad self-check. A grep is not used. Dynamic leg:
      when a *legacy* 0.12.2 binary is present, it must first FAIL a probe
      with two `for (let i)` loops (a 0.12 build that already has the
      for-init scoping fix cannot prove anything), then really import the
      three shipped libs and check four known values. No binary → explicit
      skip; the static leg stays authoritative. Both negative controls were
      replayed (blinded matcher → red; injected duplicate `let` → red).
- [x] **Compatibility contract documented in the three shipped lib headers**
      (and in `tests/lib-compat.py`): every loop variable must be unique per
      block; do not rely on 0.13's for-init scoping; a new lib that only
      works on 0.13 belongs in a `lib/compat-0.12/` split, not in the shared
      set.
- [x] **Close the coverage gap found while verifying this fix.**
      `tests/{sci,linalg,optim,pde}_backends.sh` existed but no Makefile
      target ever called them, so a change to `lib/` had no automatic
      three-backend check. `make test` now runs `lib-backends` (all four,
      with each script's own interp/text fallback when libLLVM is absent).
      Before/after comparison of the four `*-consistency.lume` outputs
      (22 + 33 + 13 + 144 = 212 lines) is byte-identical, which is the
      evidence that the 55 mechanical renames changed no numeric value.

**Known limits deliberately NOT in this item.** The released 0.12.2 also
cannot load `complex.lume` / `sig.lume` (`atan2` undefined), `dist.lume`
(`norminv`), `pde.lume` (`sin`) or `ml.lume` (parser difference at line 449).
Those are missing-builtin / parser gaps, not duplicate declarations; fixing
them means either backporting the 0.13 builtin set or a 0.12-specific lib
split. `graph.lume` / `identity.lume` / `peer.lume` already import cleanly on
0.12.2 — their repeated names live in different blocks, which is why the
static leg is block-aware rather than function-wide.

Workaround used by energy (not a fix): copy only the four needed functions
into the app's `numeric.lume` and give every loop variable a unique name.

### P16.2 `bridge_serve.c`: static responses had no validator, so no 304 — LANDED 2026-11-08

The original symptom (kept as the reasoning record): `send_resp()` wrote a
fixed header block with no `ETag`, `Last-Modified`, `Cache-Control` or
`Accept-Ranges`; the static path always sent 200 + the whole body. Measured
on lume-energy, a 200 KB `app.js` was re-downloaded on every browser reload
(transfer 200.4 KB / decoded 204898), while lume 0.12's `agent-httpd`
answered `0.3 KB / decoded 0` (304). `curl -I` also 404ed because HEAD was
not routed.

- [x] Emit validators on 200 static responses: `ETag`, `Cache-Control:
      no-cache`, `Last-Modified`. Format and semantics are aligned with
      `agent-httpd/src/http/static.c` rather than invented: weak ETag
      `W/"<size-hex>-<mtime-hex>"`, IMF-fixdate `Last-Modified` in UTC, and
      `no-cache` (store, but revalidate — the 304 is what makes it cheap).
      **`Accept-Ranges: bytes` is deliberately NOT emitted.** agent-httpd
      implements single-range 206 responses and can honestly advertise it;
      this bridge has no Range handling at all, so 200 responses declare
      `Accept-Ranges: none`. A server that ignores `Range` while claiming
      byte support makes clients retry and re-download behind our back.
      Revisit only if 206/Content-Range lands; the test gate greps for the
      literal `Accept-Ranges: bytes` in `bridge_serve.c` to keep it honest.
      `Vary: Accept-Encoding` is likewise omitted (no gzip negotiation).
- [x] Parse `If-None-Match` / `If-Modified-Since` on static GET and answer
      `304` with no body. `stat()` runs BEFORE `fopen()`, so a conditional
      hit reads zero body bytes — that is the actual transfer saving.
      RFC 9110 13.2.2 precedence is honoured: a present-but-unmatched
      `If-None-Match` suppresses `If-Modified-Since` entirely. `*` and
      comma-separated lists work, with weak (`W/`) comparison. 304 omits
      `Content-Type` / `Content-Length` (they describe a representation
      that is not sent) but repeats the validators, `Cache-Control` and all
      security headers (RFC 9111 4.3.4).
- [x] Route `HEAD` for static paths. The route-miss fallback now accepts
      GET and HEAD; `send_resp_full(..., head_only)` keeps the exact GET
      headers including `Content-Length` while sending no body. 404/500 on
      that path are body-less too. `curl -I` no longer 404s.
- [x] Regression: `tests/static-cache-test.sh`, wired into `make test` and
      `.PHONY`. Self-contained temp docroot on port 18893 (docroot-test owns
      18892, so `make -j4 test` is safe). 37 assertions: independent UTC
      date / ETag expectations, 304 + zero body, `*` and weak-list matching,
      INM-beats-IMS, IMS replay / older / malformed, HEAD 200/304/404 with
      `Content-Length` parity, security headers on 200 and 304, 404 without
      a validator, traversal still 404, and an ETag that changes with the
      file. Three replayed negatives: advertising byte ranges -> red,
      blinded assertion matcher -> red at the self-check, and C `304`
      disabled + rebuilt -> red. A matcher self-check runs on every pass.

Implementation notes: `send_resp()` keeps its original six-argument shape
(all 12 route/error call sites untouched) and delegates to a new
`send_resp_full()` that accepts extra header lines and a `head_only` flag.
Conditional headers use a new strict `header_value()` (line-anchored,
case-insensitive name + colon) because the existing `ci_find()` is a plain
substring search and would treat `X-If-None-Match:` or a value containing a
header name as a validator. GMT conversion is pure integer arithmetic
(`gm_parts()`, civil-from-days): no `gmtime_r`/`gmtime_s` signature
divergence between POSIX and UCRT, no `timegm()` on Windows, and no
locale-dependent `%a`/`%b`. Windows/mingw `stat` differences are isolated
in `LUME_STAT` / `LUME_ISREG`.

**Known limits (documented, not fixed here):** no `Date` header (a pre-existing
gap of `send_resp()` for every response, not introduced by this item); no
Range / 206, no gzip sibling negotiation; dynamic route responses have no
validator (correct — their content is not a file); a same-second, same-size
rewrite can collide with the weak ETag, exactly as with agent-httpd.

**Done when:** a 0.12 host can `import` all three shipped libs without a
parse error (done in P16.1), and `bridge_serve` serves static assets with
ETag/304 parity with `agent-httpd`. Verified with a live curl probe: a 200
KB GET is `200` with `W/"3201b-..."`; replaying its ETag returns `304` with
`size_download=0`; the same holds for `If-Modified-Since`; `curl -I` returns
`200` with the matching `Content-Length` and no body. The energy app itself
was not redeployed in this pass.

## P17 — interpreter-only inbound SSE client `http_stream_*` (landed 2026-11-08)

Recorded here 2026-11-09: the feature shipped as `f101133` on 2026-11-08
but existed only in that commit message and the host's P17 mirror section
(`lume/TODO.md`). This `TODO.md` had no P17 section at all and still
showed the P14 `http_stream` checkbox as DEFERRED; both are corrected
above and below.

**What landed.** `http_stream_open` / `http_stream_read` /
`http_stream_close` in `src/builtins_http.c` (+402 lines, 700 across the
commit): a fixed, generation-tagged slot handle table, an incremental SSE
line parser, and reuse of the bare-socket HTTP/1.1 transport (`url_parse`,
the `host_blocked` SSRF gate, `open_conn` proxy + TLS, `read_head`) with
the shared `vm->no_net` gate. Registrations are interpreter-only
(`interp.c` / `typecheck.c` / `builtins.c` / `builtins_internal.h` /
`builtins.h`); neither `codegen_call.c` nor
`llvm_codegen_internal.h`'s `BUILTINS[]` sees the names, enforced by
`make check` -> `tests/stream-native-gate.sh`. `http_stream_read` returns
one parsed event `{ok, event, data, id, eof, err}` per call; v1 does not
auto-reconnect — the last event id is handed back so a script can do it.
`tests/http-stream-smoke.lume`, `tests/http_stream_srv.py`,
`tests/stream-native-denied.lume` and `tests/stream-native-gate.sh` ship
with it. `2317de2` folded `http-stream-test` into `make test` as a
POSIX-only leg (same shape as `spawn-test`), and `5b15f16` gated the
`[dbg] tls handshake failed` stderr print behind `NDEBUG`.

**Why interpreter-only.** P14's decision (a) closed spawn's emitters but
did not authorise a *new* network primitive, and a compiled binary has no
VM to carry `vm->no_net`. The host repo therefore treats `http_stream_*`
as a permanent core-only boundary (`lume/TODO.md` P17 mirror) and marks
the three names PERMANENT in `tests/builtin-parity.allow`; the accepted
`lang/PIN.lag` deltas are deliberately retained, not a pending merge.

**Cross-platform verification — CLOSED 2026-11-09:**

- [x] Windows `LUME_HAS_HTTP=0` lane: local `make -j4 check` (MinGW gcc
      14.2) is green, including `stream-native-gate` (static self-check,
      no stream token in emitters / `rt.c`, `--check` knows
      `http_stream_open`, `--compile-text` refuses it) and
      `spawn-native-gate`. CI run `42553136470` (Windows MSYS2) succeeded
      on `63c3e1a`. The Windows port excludes `src/builtins_http.c`, so
      no stream primitive is compiled into that binary at all.
- [x] Linux dash `/bin/sh`: CI run `42553136496`, `test (ubuntu-latest)`
      → `make test` → `==> http_stream 流式单测通过 (stream + locked)`;
      `test (macos-latest)` green too. `2317de2` is what folded
      `http-stream-test` into `make test` (POSIX-only).
- **Coverage note closed 2026-11-09:** the literal `gcc:13-bookworm`
      container named in the original note now has its own `bookworm:`
      job in `ci.yml`, running `make check` + `make test` under gcc 13
      and dash `/bin/sh`. It used to be hand-verification only — P14's
      `649f2df` checked spawn streaming on that image — with no docker
      on this machine (WSL launch blocked by the local security policy),
      so it was recorded as coverage, not a correctness gap. Adding the
      lane was optional; it is now done, and Ubuntu/dash, macOS and the
      container leg all exercise P17's stream tests.

**Found during the Windows lane (new item):**

- [x] `LUME_HAS_HTTP=0` builds answer `undefined variable
      'http_stream_open'` at runtime instead of the clear
      `http_* unavailable: this build was compiled without LUME_HAS_HTTP`
      error that `src/builtins.c`'s `native_http_unavailable` stubs
      promise. `typecheck.c` registers the names unconditionally
      (`LUME_BUILTIN_NAMES`), but `interp.c`'s whole HTTP builtin table
      sits inside `#if LUME_HAS_HTTP`, so the interpreter never finds
      them and the stubs are unreachable. Repro on the Windows
      `LUME_HAS_HTTP=0` binary: a one-line
      `let c = http_stream_open("http://127.0.0.1:1/sse");` exits 1 with
      `lume: line 1: undefined variable 'http_stream_open'`. The older
      `http_get` / `http_post` / ... are affected the same way, so this
      is a pre-existing diagnostic gap, not a P17 regression and not a
      security issue. The original note proposed registering the
      `b_http_*` wrappers in the `#else` branch — **that proposal was
      rejected**, see the correction below. **Sync impact:** `interp.c`
      is `host_owned`, so the host owes a hand-merge / `accept-lag` and a
      `PIN.lag` refresh.

**Fixed 2026-11-09 (and correction to the proposed fix):** the
`#if LUME_HAS_HTTP` / `#if LUME_HAS_SPAWN` gates in `src/interp.c` are
**link-level opt-outs**, not "hide the name" switches. `bfe1f6d`
(2026-10-04) added them so a downstream that reuses `interp.c` but
supplies its own `builtins.c` never needs an undefined `b_http_get`
symbol; registering `b_http_*` under `#else` would break exactly that
guarantee. `de5c012` (2026-10-06) added the Windows stubs two days later
and their comment promised a clear runtime failure that the older gate
made unreachable — the two halves never agreed. The fix keeps the gates
intact and adds a **self-contained diagnostic stub defined inside
`interp.c`** (`b_http_unavailable` / `b_spawn_unavailable`), seeding all
eight HTTP names and all four spawn names in the `#else` branches with
the same messages the `builtins.c` stubs carry. No external `b_*`
symbol is referenced, so downstream linking is unchanged and the host's
`lang/interp.c` can still eventually leave `host_owned`. A name whose
only behaviour is `vm_set_error` grants no capability; the P14 double
gate and the P17 interpreter-only emitter boundary are untouched. The
`LUME_HAS_SPAWN=0` Windows lane had the identical defect and was fixed
in the same change. `src/builtins.c` comments were corrected to say the
native stubs exist purely to close the link for the always-compiled
wrappers.

Regression: `tests/unavailable-builtins-gate.sh`, wired into `make check`
as `unavailable-builtins-gate` on the Windows lane only (POSIX has
`LUME_HAS_HTTP=1` / `LUME_HAS_SPAWN=1`). It asserts `--check` still
accepts each name, that running it fails with the clear "unavailable"
message, and that `undefined variable` never comes back. Verified on
MinGW gcc 14.2: `make -j4 check`, `make native-consistency`,
`make native-text` and `make native` are green; `http_stream_open`,
`http_get` and `spawn` now report `http_* unavailable: ...` /
`spawn_* unavailable: ...`.

Host sync landed in the same change: `lume/lang/interp.c` hand-merged
with the five outbound verbs (no spawn — the host never adopted P14 —
and no `http_stream_*` — the permanent core-only boundary),
`lume/lang/builtins.c` comment corrected, `tests/http-unavailable-gate.sh`
added to the host's Windows `make check` (it also asserts
`http_stream_open` stays absent), and `lang/PIN.lag` refreshed with
`sync-lang.py accept-lag`. That `accept-lag` also absorbed the
pre-existing `builtins_http.c` +4 drift from `5b15f16` (the host copy
deliberately omits the `[dbg]` TLS print), which had been making
`make check-sync` red before this change. `make check-sync` and
`make check-parity` are green again. `lang/PIN`'s sha stays folded:
no synced file changed.

**Known limits, deliberately not scheduled:** v1 has no auto-reconnect;
P17 is an inbound SSE *client* and does not emit long-lived streamed
responses (P14's "SSE streaming a running job" row stays open); the host
will not merge the registration chain without an equivalent fail-closed
gate of its own.

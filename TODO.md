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
  so Windows runs the driver shape instead).
- [ ] Decide CI policy: a short 60s fuzz leg in the `asan` job, or
  local-only. Start local-only; promote once it proves stable. — Local
  smoke passed: 1,094 mutation inputs (truncate/flip/random bytes over
  the fixtures) through `--check`, zero unexpected exits; the repeated-
  cycle global state (orphan batch, ty_allocs, g_ck_scopes) is clean.
  A 30-minute libFuzzer run still needs a Linux host.
- [ ] Any crash found lands in `tests/fuzz-crashes/` and gets minimized
  into a `tests/*-bad/` fixture so the regression stays covered. — No
  crash sample yet; the mechanism is wired (the fuzz corpus seeds from
  every `tests/` fixture, so a new `-bad/` file is covered
  automatically).

**Done when**: 30 consecutive minutes of fuzzing with zero crashes,
hangs, or leaks.

## P2 — Arena allocator for the AST

The 2026-10-15 leak fixes (N_LET_EXPR subtrees, truncated scope tables,
throwaway map-put results) are all instances of one structural problem:
manual per-node freeing that every new node type and scope rule must
remember. An arena removes the whole class instead of patching points.

- [ ] Design first: arena lifetime = one compilation unit
  (`parse_program` .. end of backend emission). `nalloc` redirects to
  the arena; `node_free` / `node_free_kids` / `node_free_own` and the
  orphan journal collapse into a single arena reset.
- [ ] Enumerate the lifetime hazards before writing code: AST nodes are
  shared across phases (checker, codegen scan, both emitters read the
  same tree) — the arena must outlive all of them. Cross-check every
  phase that today calls `node_free` on a subtree it claims to own.
- [ ] Keep `asg_truncate` / `arm_scope_truncate` semantics correct: with
  an arena the strings they freed become arena-owned, so the truncate
  helpers degenerate to count resets.
- [ ] Gate on the existing suite: `make asan` must stay at zero leaks
  (now proving the arena, not the frees), native-consistency unchanged,
  all `*-test` targets green.

**Done when**: the free-path patches from `ecd7afc`/`7e5d2f8` can be
deleted without the suite noticing.

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
- [ ] If LLVM emission proves heavy on the runner, land the interpreter
  + text legs first and track the llvm leg separately.

**Done when**: the Windows workflow runs the three legs green.

## P4 — Later / larger

- [x] Cross-backend `json(str)` parsing: needs a tagged-any value (2026-10-16: rt.c `lume_json_parse` + slot helpers; codegen json/get/str/len/for-in on any, verified interp = IR-text native on nested containers; `--compile-llvm` (llvm_cg_*.inc) wired 2026-10-16 too (json parse / str / len / get / for-in / infer), pending real-machine check on the Linux copy; try() failure path fixed 2026-10-16 on the text backend: rt.c `lume_map_put_slot` (tag-dispatched {ok} writer) + codegen_call.c try TY_ANY branch + json() failure routed through `lume_error_set` (try() fills {err: "json parse error..."}, outside try() prints and exits like the interpreter) — verified `try(() => json("{bad"))` => `{"ok":null,"err":"json parse error (at offset 1)"}` and `a={"ok":3,"err":null}`; LLVM side still pending: try() itself is unimplemented on the LLVM backend (no error_begin/end/branch emission; json failure there serializes as null — noted at llvm_cg_call.inc, land together with branch emission); index / dflt-less get verified identical interp vs IR-text 2026-10-16 — and closed 2026-10-16: `j["k"]` subscript on an any value now lowers on the text backend (rt.c `lume_slot_at_key` / `lume_slot_at` + `lume_index_error` failure branch in codegen_expr.c cg_index) and dflt-less `get(j,k)` reads the raw slot (`lume_slot_get_any` — absent key is a null slot, not an error); `str()` of a string slot is now the bare string (`lume_slot_str_scalar`) while `stringify()` keeps JSON quotes (`lume_slot_str`), matching the interpreter. LLVM backend mirrors: any-subscript branch (llvm_cg_expr.inc), dflt-less get (llvm_cg_call.inc — merged with 2ebe3cf's three-arg `lume_slot_get_any`, both backends share one call shape), str scalar (llvm_cg_call.inc); try() itself landed 2026-10-16 (llvm_cg_call.inc: error_begin/end bracket + branch + slot-ized ok, llvm_cg_scan.inc: {ok,err} inference, json() failure routed through `lume_error_set` like the text emitter); interp json error message unified to "json parse error (at offset N)" (value.c) matching both native emitters; cross-backend regression suite added at tests/llvm-json/ (trymin/jedge/jt2b/jfail + .expect + run.sh) — the whole LLVM side (json three-piece + try) still pending real-machine verification on the Linux copy (`make` + `--compile-llvm`, then `tests/llvm-json/run.sh`))
  representation in the native backends — a real project, not a patch.
  SPEC §"intentional boundaries" documents the current interp-only rule.
- [x] Ecosystem: README first-screen example pass, one non-toy example
  program, then exposure. No marketing before P1-P3 close. — README/README.zh.md
  first-screen example (2026-10-16, verified on all three backends) +
  `examples/stats.lume` (statistics toolkit: typed functions, HOFs, closures,
  struct values — `make check` covers it via EXAMPLES).

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

Gray zone that does not need the above: whitelisting write CTEs
(`WITH ... INSERT/UPDATE/DELETE`) — still one statement, still
stateless, guardrail-only change; collapses some multi-step units
(not general cross-call sequences). Consider on its own merits.

Host-tree debt: `builtins_sql.c` is HOST_PINNED (ownership fork, not
visibility — the host version carries portfolio-mirror policy on a
`db_layer` abstraction), so `make sync-lang` will never overwrite it;
a core transaction builtin owes the host a manual merge, not a copy.

**Done when**: a lume-nsgm-scale workload demonstrates a real crash or
correctness need for cross-statement atomicity (not just purity), and
the value-system design clears the llvm closure-capture gap. (The gap
this gated on — closures capturing function locals on the llvm backend
— landed 2026-10-18, see P7; a transaction handle behind a closure
still hits the general capture machinery, and the top-level half of P7
would apply to it too.)

## P7 — LLVM closure capture (ported 2026-10-18) + the top-level gap (found, not started)

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
- [ ] Top-level lambda captures are broken on BOTH native backends
      (pre-existing, not introduced by the port above; found with the same
      probes): `let s = 0; let bump = (n) => { s = s + n; return s; };` at
      the top level prints `3 4 101 100` interpreted but `0 0 0 100`
      compiled. The signature pass's capture walk does not treat a top-level
      binding as capturable — the name is lifted to a module global — while
      the body scan then registers the free variable as a fresh alloca, so
      reads hit an uninitialized slot and writes are lost. Function-body
      captures (the shape `native-consistency`'s cap_* series covers) are
      three-way consistent; only the top-level shape diverges.
  - [ ] Decide the semantics first: bind a top-level free name in a closure
        body to the lifted global's address (capture-like, matching the
        interpreter's call-time env lookup), or reject it at typecheck with
        the §4.14 no-capture vocabulary. An interp/native divergence in
        silent-wrong shape is the one outcome that must not ship.
  - [ ] Whatever wins, it lands in the shared sig pass plus BOTH native
        emitters (the text scan registers the same fresh alloca today), and
        `native-consistency.lume` gains a top-level capture case with an
        expected baseline so the gate pins it.

**Done when**: the top-level shape is either three-way consistent or
loudly rejected, and the CI llvm leg has run green once on Linux (the
P4 pending real-machine check this note also clears).

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
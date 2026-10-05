# Changelog

All notable changes to Lume are documented here. The format follows
[Keep a Changelog](https://keepachangelog.com/), and versions aim for
[SemVer](https://semver.org/).

## [Unreleased]

### Changed — the tree itself is named lume-core

- **The directory is now `work/research/lume-core`** (previously `lume-lang`).
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
- **`LUME_HAS_HTTP` gates the `http_get` registration.** The builtin lives in
  `builtins_http.c`, which not every downstream compiles. A downstream that
  lacks it can now pass `-DLUME_HAS_HTTP=0` and take this `interp.c` as-is
  instead of pinning its own copy.

### Changed — this tree is the host-independent standalone compiler

`work/research/lume-core` starts as a two-file research sketch and now carries
the language itself (moved over **without** the host's history, on top of the
existing `594648b` initial commit). The host tree `work/research/lume` keeps
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

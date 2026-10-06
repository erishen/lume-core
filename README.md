# Lume — standalone language tree

[English](README.md) | [简体中文](README.zh.md)

> Pronounced **lu-MÉ** — `/luˈmeɪ/`, two syllables, stress on the second.

**This is the host-independent tree.** It is the language part of Lume on its
own: front end (lexer / parser / type checker), two native emitters — a
hand-written LLVM IR text emitter and a libLLVM C API emitter — and a
tree-walking interpreter. It links nothing but libc (+ libLLVM when the build
finds `llvm-config`); there is no HTTP server, no agent runtime, no Docker
image here.

The tree that embeds Lume **in** the agent host (`libagenthttpd.a`, live HTTP,
chat, tool dispatch, SQLite tools, Docker image) is the sibling project
`work/research/lume`. Same language, different deployment shape; changes to
the language belong in this tree first, and the host tree carries them from
here.

| | this tree (`work/research/lume-core`) | host tree (`work/research/lume`) |
|---|---|---|
| purpose | standalone language + libLLVM native path | language embedded in agent-httpd |
| links | libc, libLLVM (optional) | + `libagenthttpd.a` |
| `make test` | interpreter tests, crypto, both native emitters, parity | + HTTP/e2e suite (`tests/run_all.sh`) |
| server/demo targets | none | `make dev` / `hub` / `invest` / `image` / … |

### Keeping the two trees in sync

The front end here (`lexer.c`, `parser*.c`, `interp.c`, `value.c`,
`typecheck*.c`, `loader.c`, `vdom.c`) is a **hand-maintained copy** of the same
files in the host tree — not a fork that tracks automatically. They have drifted
already: `lexer.c` / `parser_stmt.c` / `typecheck_stmt.c` / `vdom.c` / `token.c`
are still byte-for-byte identical, while `parser.c` is 386 lines in the host and
675 here (the host splits `parser_stmt.c` + `parser_expr.c`, this tree keeps one
`parser.c`), and `interp.c`, `value.c`, `typecheck.c`, `loader.c` differ too.

Rules that follow from that:

1. **A language change lands here first**, then the host tree carries it.
2. Changing any of the shared files means **editing both copies** in the same
   working session — there is no automatic propagation.
3. **Prove the sync instead of assuming it.** After editing, compare the two
   copies (`diff work/research/lume/src/lexer.c work/research/lume-core/src/lexer.c`
   and the rest of the front-end files). Identical line counts alone are not
   proof; diff the files.
4. Host-only capabilities (HTTP/agent surface such as `cache_control`) must
   never be expected here: this tree has no `agent-httpd`, and `run()` calls
   `bridge_run()`, which exits `2`.

A single-binary **C11 compiler**: business logic lives in `.lume` scripts, and
`lume --compile script.lume` turns one into a native executable via libLLVM.
Two emitters are kept and cross-checked against each other on purpose (see
[docs/NATIVE.md](docs/NATIVE.md)); `--no-pass` skips the optimisation
pipeline when you want to see the lowering an emitter produces.

> ℹ️ **Not related to** [lumeland/lume](https://github.com/lumeland/lume) (the
> Deno static site generator) or [lume/lume](https://github.com/lume/lume)
> (the CSS3D/WebGL UI toolkit) — different projects that happen to share the
> name.

> ℹ️ The security notes further down ("read before exposing the port") belong
> to the **host** tree's HTTP server. Nothing in this tree listens on a port.

## ⚠️ Security

**This tree ships no server, no port and no agent runtime** — it is a compiler
and CLI. The notes that follow describe the *host* tree's HTTP surface
(`work/research/lume`); read them only if you are running that tree.

Here the whole surface is the `lume` binary itself, and the rules are just the
usual ones for a local compiler:

- `--compile` / `--compile-llvm` shell out to `cc` / libLLVM; untrusted source
  runs with your privileges. Don't compile (or `run()`) scripts you don't trust.
- Built-in `fs` helpers resolve relative to the script's directory; keep
  scripts in a directory you control.
- `bridge_run` (the DSL's `run()` statement) exits `2` in this tree — reaching
  it means a script asked for the host server, which this build does not have.

Lume's HTTP server has **no built-in authentication**. The chat endpoint
(`/react/api/chat`) can drive `fs` tools that read and write files, so **anyone
who can reach the port can use `/chat` and read local files**. Treat the port as
a trusted, loopback-only surface:

- **Bind to loopback** (`127.0.0.1`) or put it behind a reverse proxy that adds
  auth. Never expose the port to an untrusted LAN/WAN.
- **Enable Basic Auth** for any non-loopback bind. Container deployments do this
  via the bundled `htpasswd` + `.env`; **local `make dev` / `make invest` do
  _not_ enable auth by default** — keep them on `localhost` only.
- **The Basic-Auth gate is global** — it fronts built-in routes and custom
  routes declared in the `.lume` script alike (the `http.c`/`event.c` auth
  check runs before any routing). Hash formats: bcrypt (`$2a$/$2b$/$2y$`)
  verifies through the bundled portable implementation on every platform;
  `$5$`/`$6$` go through libcrypt, which Linux (glibc/musl) supports but
  **macOS/BSD libcrypt is DES-only** — there those entries always deny, and
  lume prints a loud warning at startup. For portable files prefer
  `htpasswd -bnB` (bcrypt).
- **Data egress.** When `LLM_API_KEY` is set, chat content, session memory and
  the SQLite schema are sent to `LLM_API_URL`. For a public/third-party provider
  this is "providing personal data to a third party" under PIPL — the in-app
  settings/discovery pages disclose this, but running it for *others'* data
  needs your own privacy notice and consent flow.
- **Same-origin guard.** `/api/reports` and `/api/settings` (iquest) reject
  cross-origin reads/writes, and the native chat endpoint (`/react/api/chat`)
  also enforces a same-origin + POST-only check in agent-httpd
  (`chat_origin_ok`) — a cross-site browser request is rejected with 403 before
  any generation or session write. Requests with no `Origin` (non-browser
  clients, the test harness) are still allowed, so the network boundary above
  remains the primary control.

## Quick start

```bash
make             # build bin/lume-core (needs cc; libLLVM only if llvm-config exists)
make check       # type-check the bundled examples, no output artifacts
make test        # parity + unit tests + crypto + both native emitters + consistency
make treecheck   # same thing as a privacy gate: no build-machine path tracked in HEAD
make asan        # rebuild with ASan/UBSan and run the unit tests
make pack        # dist/lume-core-<os>-<arch>.tar.gz (binary + README + CHANGELOG)
make dump        # print the IR text backend produces for examples/hello.lume
make native      # native-bench through the default (libLLVM) backend
make native-text # native-bench through the hand-written IR text backend
make native-bench # compare the two backends on the same script
make clean
```

Run a script with the interpreter, or emit and run native code:

```bash
./bin/lume-core examples/hello.lume                  # interpret
./bin/lume-core --compile examples/native-fact.lume  # -> native-fact via libLLVM
./bin/lume-core --compile-text examples/native-fact.lume   # hand-written IR text
./bin/lume-core --no-pass --compile examples/native-fact.lume  # skip -O passes
```

Dependencies: a C11 compiler (`cc`) and, for the libLLVM path, `llvm-config`
on `PATH` (the build then links `llvm-config --libs`; without it you still get
`bin/lume-core`, just with `--compile-llvm` unavailable). Nothing else — no
`node`, no `pnpm`, no server runtime, no SQLite.

## Install

There is nothing to install — no npm registry, no package manager, no release
tarball for this tree. Clone and build:

```bash
git clone <this-repo> lume-core && cd lume-core
make                 # -> bin/lume-core (~270 KB, two-thirds of it the LLVM glue)
make check           # sanity: the bundled examples type-check
sudo cp bin/lume-core /usr/local/bin/lume-core    # optional
```

The install-time overrides below (`LUME_VERSION`, `LUME_PREFIX`,
`LUME_SHA256`, the `install.sh` one-liner and the `react-ssr` demo) all
belong to the **host tree**'s release pipeline; this tree produces just the
compiler, so `make && cp bin/lume-core` *is* the install.

Build variables worth knowing:

- `CC=` — pick a different compiler
- `LLVM_PREFIX=` / `LLVM_CONFIG=` — where `llvm-config` lives, when it isn't
  on `PATH`
- `CFLAGS=` / `LDFLAGS=` — appended, e.g. `CFLAGS=-O2` or extra include paths

## Layout

| Path | Contents |
|---|---|
| `src/` | Lexer / parser / type-checker / tree-walking interpreter + the **fork-local bridge stub** (`bridge_stub.c`) + native backend (`codegen.c` + `codegen_{types,expr,scan,sig,stmt}.c` / `irbuf.c` / `backend.c` for IR text, `llvm_codegen.c` / `backend_llvm.c` for the optional libLLVM path) - 45 files, ~15k lines of C11 |
| `examples/` | Language-only scripts: `hello`, `lang-basics`, `modules/app`, `native-fact`, `native-bench`, `http-get` |
| `tests/` | C unit tests (`smoke.c`, 134 checks) + `test-crypt.lume` + `native_backends.sh` (three-way interp / text / llvm comparison) + the consistency expectations |
| `scripts/` | `check-backend-parity.sh` — AST-label coverage parity between the two emitters |
| `docs/` | Full docs, see below |

> The host tree additionally has `frontend/` (React client), `www/` (docroot),
> `docker/` (image + compose), `editor/lume-vscode/` and the server/demo
> `.lume` scripts — this tree has none of those, by design.

### This tree is the upstream, the host is downstream

`work/research/lume` (same repo, sibling directory) is the **server product**.
It holds `lang/` as a **vendor+pin copy** of this tree: `lang/PIN` records the
upstream `sha` plus the file list the host owns on top (`host_owned`). In that
tree `make sync-lang` advances the pin, `make check-sync` reports what has
drifted. So:

- a change here that belongs to the **language** should land here, and the
  host picks it up by bumping its pin — not by hand-copying;
- files the host owns (`interp.c`, `main.c`, `typecheck.*`, `lume.h`,
  `builtins*` … note it keeps its own `builtins.c`, and has no
  `builtins_http.c`) **diverge on purpose** and are never overwritten by sync;
- dropping `builtins_http.c` means the host compiles this `interp.c` with
  `-DLUME_HAS_HTTP=0` (see `docs/ARCHITECTURE.md` 5.1).

### Release packaging

```sh
make pack        # rebuild with pack CFLAGS, strip, tar up dist/.pack -> lume-core-<os>-<arch>.tar.gz
make packcheck   # grep the finished tarball for build-machine paths; non-zero on success
```

The tarball used to leak `/Users/…` in two places: DWARF from `-g`, and
`LUME_RT_SRC` baked into `backend.o` as a string literal (which `strip` cannot
remove — only a rebuild with a package-relative path can). `packcheck` is the
guard, and CI runs it after `pack`, so a regression fails rather than shipping.

## Documentation

> Docs are currently written in Chinese; the user guide and developer guide
> below are the canonical sources.

- [docs/LUME.md](docs/LUME.md) — **User guide**: language quick tour
  (types / control flow / Result), built-ins, route & tool registration, the
  `el()` / `html()` page APIs, SSR serialization details, multi-file modules
  (`import` / `export`).
- [docs/DEVELOPMENT.md](docs/DEVELOPMENT.md) — **Developer guide**: layout,
  interpreter core conventions (read before changing code), how to add
  built-ins / statements / types, testing conventions (the fork's `make test`
  has no HTTP/e2e leg), rendering built-ins, known conventions and pitfalls.
- [docs/ARCHITECTURE.md](docs/ARCHITECTURE.md) — **Architecture**: system
  overview and design decisions, process / memory model, layering, request
  lifecycles, deployment topology and the security boundary.
- [docs/STYLE.md](docs/STYLE.md) — **Style guide**: how to write `.lume`
  with the current feature set (arrow expression bodies, `try` fixed keys,
  string-key map literals, `spa`, write-path discipline), with a pre-commit
  self-check list.
- [docs/PITFALLS.md](docs/PITFALLS.md) — **Pitfall ledger**: real-world
  traps with their fix status; read before writing business `.lume`.
- [docs/NATIVE.md](docs/NATIVE.md) — **Native backend** (`--compile` defaults to
  the libLLVM path, `--compile-text` opts out, experimental): Lume → native
  binary by two routes (libLLVM C API vs hand written IR text), why `print`
  avoids `printf`, the builtins it covers and what it does not.

## Native backend (experimental)

`--compile` turns a `.lume` script into a real executable. Two back ends share
the same front end (lexer → parser → type checker):

* **default (`--compile`, also spelled `--compile-llvm`)** — the type-checked
  AST goes through `src/llvm_codegen.c`, which builds the IR as LLVM **values**
  through the **libLLVM C API**. LLVM then checks the module (`LLVMVerifyModule`)
  right where it is produced and emits the object file itself;
* **`--compile-text`** — the AST goes through `src/codegen*.c` (one file per
  section: types / expressions / scanning / signature inference / statements,
  see the header of `src/codegen.c`), which emits LLVM IR **text**; clang
  finishes the job.

```bash
make native                     # default back end: compile + run + diff vs tests/native-fact.expected
make native-text                # same expected baseline, forced through the text path
make native-llvm                # same expected baseline, forced through libLLVM
bin/lume-core --compile examples/native-fact.lume && ./out/native-fact
bin/lume-core --compile script.lume -o mybin       # IR lands in out/mybin.ll
```

The default is libLLVM whenever `bin/lume-core` was built with it: a malformed IR
shape is rejected at the point it is produced, instead of surfacing later at the
clang step. The back ends are compiled into one `bin/lume-core` — the Makefile looks
for `llvm-config`, and when it is missing the binary simply has no libLLVM path
and `--compile` uses the text one (stderr says so).

The known cost of the libLLVM path is that **no optimization pass runs**:
`LLVMRunPasses` aborts when called from a pure-C translation unit on this
libLLVM build — a minimal probe reproduces it for every pass string (`verify`,
`mem2reg`, `instcombine`, `default<O2>`) and every code model — so the output is
correct but slower than the text path on a loop-heavy target (there, clang's
`-O2` runs mem2reg). Use `--compile-text` when throughput matters. `make
native-bench` compiles the same nested-loop program through both back ends and
prints compile time, IR size, object size and run time for each — live numbers
from `make native-bench` (nested loop, 1e8 inner iterations):

```
backend   compile-ms   ir-KiB  obj-KiB  bin-KiB    run-ms
text            194        2        1       34         7
llvm            245        2        1       34       220
```

Correctness and every size column tie; the only gap is `run-ms` — without
mem2reg the locals stay memory slots for all 1e8 iterations. See
[docs/NATIVE.md](docs/NATIVE.md).

It covers the core subset — scalars, structs, functions, `if` / `while` /
`for`, arithmetic and comparisons, and a first set of builtins
(`print` / `abs` / `min` / `max` / `sqrt` / `pow` / `floor` / `ceil` / `round`
/ `str` / `int` / `float`, string `+`). `server {}` / `route` / `tool` /
`run()` still belong to the interpreter and the agent-httpd bridge: they are
not part of the native path yet. Details and the reasoning behind the design —
in [docs/NATIVE.md](docs/NATIVE.md).

## Containers

> ⚠️ **This section describes the host `lume` tree, not this one.** Upstream,
> this tree has no `docker/` directory, no `Dockerfile` and no
> `make invest` / `make demo-sqlite` (see `docs/ARCHITECTURE.md` §5.4,
> "container deployment: not for this tree"). Containers, compose and k8s
> belong to the host `lume/` tree; this tree ships one static tool: `bin/lume-core`.

```bash
cd lume && docker compose -f docker/docker-compose.yml up -d --build
```

One image `lume:latest`; the compose `invest` (host `127.0.0.1:18082`) and
`hub` (`127.0.0.1:18083`) services each only swap
`examples/<name>.lume` + ports + allow-list env, sharing one docroot. The
build context is the `lume/` repo root (agent-httpd is a submodule inside the
repo), so `cd lume && docker build -f docker/Dockerfile` works — Lume
statically links agent-httpd's `libagenthttpd.a`, and the host-built archive
is Mach-O, so it must be rebuilt inside the Linux container.

The image carries no `.env` (injected by compose `env_file`) and no `.data/`
sessions; the `skills/router/` sync copy **does** ship in the image — it is a
fallback cache written by `llm-router` at startup so the container keeps the
last skill definitions if sync fails; it is not a reproducible artifact and
stays git-ignored.

## Security notes

> ⚠️ **This section describes the host `lume` tree, not this one.** Basic
> Auth, session TTL, `/api/reports` origin guard, the `portfolio.json` ledger
> and the SQLite mirror all belong to the host (agent-httpd layer) -- only
> that tree has them. Upstream, this tree is a pure language runtime whose
> three boundaries are `--no-fs` / `--no-net` / the SSRF gate [LUME.md](docs/LUME.md).

- **Two-layer access control**: the Lume server itself has no auth module —
  `/api/reports`, `/react/api/chat`, `/discovery` are all plain endpoints, and
  chat can read/write files via the `fs` tool. So (1) compose binds host ports
  to `127.0.0.1` rather than `0.0.0.0` to keep the service off the LAN, and
  (2) containers enable Basic Auth — the password lives only in `.env`
  (`LUME_AUTH_USER` / `LUME_AUTH_PASSWORD`, git-ignored); the image entrypoint
  converts it to `/app/auth/htpasswd` (bcrypt) at startup and `server{}`
  reads it via `htpasswd = env("HTPASSWD_FILE")`. Verified: no credentials →
  `401 + WWW-Authenticate: Basic realm="lume"`. The framework only loads
  `$5$` / `$6$` / bcrypt strong hashes; plaintext and weak hashes abort
  startup. After editing `.env`, `docker compose up -d` applies; local
  `make invest` does not inject these envs → they resolve to null → auth
  stays off. Keep passwords alphanumeric — `#` / `$` and friends are parsed
  inconsistently between compose's env parser and `llm_env_init()` and can be
  silently truncated.
- **Logout & account switching** (optional envs): `AUTH_REALM_FILE` enables
  `/logout` — it rotates the 401 challenge realm (`<realm>#N`) so the browser
  re-prompts instead of silently reusing cached credentials, plus an optional
  one-shot 30s deny for the logged-out user. `AUTH_PUBLIC_PATHS` (a `;`-
  separated prefix list) exempts credential-free frontend resources (e.g. an
  `/accounts` switch page) from the 401 gate with a segment-boundary prefix
  match (`/accounts` covers `/accounts/list`, not `/accounting`) — without it
  the switch page itself prompts after logout.
- **Access logs never record query strings** (explicitly truncated in
  `http_log.c`); log files are 0600; request bodies are never logged.
- **Narrowed model reach**: `read_file` is jailed to the web root via
  `resolve_within`; `MCP_FS_ROOT` points at `.sandbox`, and startup warns if
  that root would scan into `.env` / `.data`; MCP children `unsetenv` the
  `LLM_API_KEY/URL/MODEL` before spawning.
- **Sessions have a 30-day TTL by default** (configurable via
  `SESSION_TTL_DAYS`, `0` disables the sweep; memory.json is never pruned)
  and 0600 file permissions.
- `GET /discovery` `endpoints` report only config state and model names, never
  internal URLs; MCP entries publish `args` (which may carry local absolute
  paths) as `<redacted>`.
- Sensitive directories never enter git: `.data/` (sessions), `.sandbox/`
  (fs-MCP sandbox root), `.env` (secrets; the `.env.example` template is
  committed).
- **Chat data goes upstream**: with `LLM_API_KEY` set, user messages, session
  memory and the configured SQLite database's schema ride along in the model
  request (`LLM_SYSTEM_EXTRA` appends deployment guidance). Point
  `LLM_API_URL` only at endpoints you trust with that data (a public provider
  is a PIPL-style "provision to a third party" — disclose and minimize).
- **Scripts are trusted code**: `.lume` files can read any file and any env
  var, so only run scripts you authored or audited. Credential-named env vars
  (`*API_KEY`, `*TOKEN`, `*SECRET`, `*PASSWORD`, ...) are masked (null) from
  the script surface; the runtime still reads them itself. The DSL has no
  outbound HTTP builtin, so a script cannot exfiltrate what it reads.
- **Startup guard**: binding a non-loopback address with Basic Auth off prints
  a one-time WARNING (stderr) that /chat, /dsl and the SQL data behind them
  are reachable by any host that can reach the port.
- `examples/invest.lume` must be started via `make invest` — the allow-list
  env is only injected there; running `./bin/lume-core` directly prints a warning
  and exposes the full capability catalog.
- To make the invest settings page actually gate the paid review models, point
  `IQUEST_ENV_FILE` at the same `autogen-pse/.env` file `pse-review` reads
  (unset → settings writes are refused with HTTP 500, read-only display still
  works).
- The invest account ledger (`.data/portfolio.json`) is written with a
  per-process flock + atomic rename: concurrent workers cannot lose an update,
  and a crash mid-write never leaves a truncated file.
- **Same-origin guard on the product API**: `/api/reports*` and `/api/settings`
  (GET and POST) reject cross-origin browser requests with 403 — a page from
  another site cannot read your reports off `localhost:8082`
  (DNS-rebinding style theft). Origin-less callers (curl, local scripts) keep
  working.

## SQLite support (host tree only — not in this one)

> ⚠️ **This section describes the host `lume` tree, not this one.** The
> `sql_query` / `sql_write` / `sql_tables` / `sql_schema` family is registered
> by the host's agent-httpd through `SQLITE_DB`; upstream, this tree's
> interpreter has **no** such builtins -- its only data capability is `json()`.

SQLite is built into the server: `agent-httpd` links libsqlite3 directly
(`agent-httpd/src/agent/sqlite_tool.c` — that file belongs to the host tree,
**not** to this one) and registers three native tools whenever
`SQLITE_DB` points at a database — no Python, no MCP stdio process, and the
static container image works too:

- `sql_query` — a single read-only SELECT; the DB is opened
  `SQLITE_OPEN_READONLY`, so writes/DDL are physically refused even if a
  statement slips past the text check. Guardrails mirror the old MCP server's:
  single statement, SELECT-only after stripping comments, prepare-time syntax
  validation, 200-row cap.
- Both tools accept optional `?` bind parameters — `sql_query(sql[, params])`
  / `sql_write(path, sql[, params])` — where `params` is a list of scalars
  bound via `sqlite3_bind_*`. Values never enter the SQL text, so guardrail
  checks see only the statement skeleton and injection through a parameter
  value is impossible (quotes, `;`, `--`, DDL keywords in a value are
  inert). Prefer this over string interpolation whenever a value is not a
  compile-time constant.
- `sql_write` — *opt-in, off by default*: a single write statement —
  `INSERT` / `UPDATE` / `DELETE` (UPDATE/DELETE must carry a WHERE clause) or
  `CREATE TABLE` for a new table. `DROP` / `ALTER` / `TRUNCATE` / `VACUUM` /
  `ATTACH` / `PRAGMA` / `GRANT` / `REVOKE` and any statement mentioning the
  `portfolio` mirror table are rejected. It is compiled into the server but
  **not** in the default whitelist; add `sql_write` to `HARNESS_TOOLS_ALLOW`
  (Makefile `INVEST_TOOLS`, compose/k8s) to let the model create analysis
  tables. The ledger itself stays authoritative in `.data/portfolio.json`.
- `sql_tables` — list table names.
- `sql_schema` — introspect tables/columns/row counts/sample values as prompt text.

- **Data**: the typed domain tools (`portfolio_add` / `portfolio_remove`) remain
  the authoritative writer to the JSON ledger. `make invest` re-seeds the SQLite
  mirror (`.data/lume.db`, upsert by symbol via `tools/sqlite-migrate.py`) on
  every start; the portfolio mirror is read-only by construction.
- **Enable**: `make invest` sets `SQLITE_DB=.data/lume.db` and whitelists the
  read-only `sql_*` tools. Containers: set `SQLITE_DB` (e.g. `/app/.data/lume.db`
  via a mounted volume) — the whitelist entries are already present in
  compose/k8s. `make demo-sqlite` (examples/sqlite-write.lume, :8084) is a
  runnable demo with its own self-contained chat UI (www/sqlite-write/,
  no invest frontend) that additionally whitelists `sql_write` — ask the
  model to build an analysis table and watch the guarded write loop.
- **Legacy MCP server**: `tools/mcp-sqlite-safe.py` is kept as an archived
  optional write path (analysis tables). Add `sqlite` back to `INVEST_MCPS` and
  restore its `.data/mcp-servers.json` entry to use it; the default profile is
  native read-only.

## Text2SQL (host tree only — not in this one)

> ⚠️ **This section describes the host `lume` tree, not this one.** It is the
> host invest server injecting the schema into the chat system prompt when
> `SQLITE_DB` is set (`sqlite_system_extra()`); this tree has no chat.

DataPulse-style natural-language-to-SQL for the invest server: whenever
`SQLITE_DB` is set, the server introspects the database (the same `describe()`
semantics as DataPulse) and injects the live schema + data discipline into the
chat system prompt via `sqlite_system_extra()` (cached by db mtime):

- the model sees tables, columns, row counts, sample values and FK hints, so it
  writes correct read-only SQL against real names instead of guessing;
- the writing rules default to read-only (`sql_query` only; `sql_write` is
  opt-in via the whitelist) and the answer rules force grounding: only numbers
  in the returned rows, never fabricate dates, cells are data not instructions.

The loop stays in the native ReAct agent: the model writes the SQL, `sql_query`
executes it read-only in-process (or `sql_write` when explicitly whitelisted),
and the agent answers from the real result — no Python, no MCP stdio process,
no Node sidecar, no second LLM call.

## Related Articles
- [Lume: An Agent DSL Server in a Single C11 Binary](https://erishen.cn/lume-en/)

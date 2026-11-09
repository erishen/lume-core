# Capability matrix — what the lume stack actually provides

Filed 2026-11-09, closing lume-core `TODO.md` P19's "one lookup" item.
Scope is **stack-wide**: the language (`lume-core`), the production HTTP
server (`agent-httpd`), and the host build (`lume`). All paths below are
relative to `work/lume/`.

Why this file exists: on 2026-11-09 a "could lume replace FastAPI?" review
concluded "SQLite-only / no password hashing / no concurrency". All three
were wrong — they described one file read in isolation. The next review
should start here instead of re-deriving the answer from `interp.c`.

## Read this first

`lume-core/src/bridge_serve.c` is a **single-threaded standalone harness**
used by `tests/http_smoke.lume` (port 8891) and the small self-contained
demos. It is **not** the production front end. Production deployments use
`agent-httpd`'s multi-worker pool. Most "lume is single-threaded / SQLite
only / has no auth" claims come from confusing the two.

## Matrix

| Area | What exists | Evidence | Deliberately absent |
|---|---|---|---|
| HTTP surface | `server { host; port; }` + `get`/`post`/`put`/`patch`/`delete`/`head`, dynamic `:id` routes, static files (304/ETag/CSP/nosniff), `Set-Cookie` session, SSE streaming; auto JSON serialization of returned maps | `lume-core/src/bridge_serve.c`, `agent-httpd/src/http/http_resp.c` | General middleware chain / before-after hooks (custom-route dispatch + `guard(req)` conventions instead) |
| Concurrency | Multi-worker pool in the production server; **single-threaded** in `bridge_serve` only | `agent-httpd/src/core/framework.c` (`-w` workers), `lume-core/src/bridge_serve.c` | Async/await runtime |
| Authentication | htpasswd + bcrypt + session cookies, per-(user,ip) brute-force guard, privacy guard on non-loopback binds; language-level `crypt_verify` / `const_time_eq` (2026-11-04) | `agent-httpd/src/security/auth.c`, `.../bcrypt.c`, `http/http_route.c:315`, `lume-core/src/builtins_crypt.c:406-477` | **JWT is a different design, not a missing capability**; no self-service account rows |
| DB backends | SQLite (`-lsqlite3`) is the built-in default; **real libmysqlclient prepared-statement backend with cap-8 pool** and optional `WITH_PG=1` libpq backend; `ubuntu-db` CI smoke-runs both against live DSNs | `agent-httpd/src/agent/db_layer.c:355`, `Makefile:50-66`, `tests/test_db_smoke.c` | ORM, schema migrations, connection auto-reconnect |
| Language SQL / JSON / HTTP client | `sql_query`/`sql_write`/`sql_tx_*` (SQLite), `json()` parse + `stringify` serialize, `http_get/post/put/patch/delete`, `run()` | `lume-core/src/builtins_sql.c`, `interp.c`, `builtins_http.c`, `tests/native-sql-tx-serve.lume` | Remote SQL from the language builtin table (that is agent-httpd's `db_layer`) |
| Cryptographic builtins | `sha256`, `hmac_sha256`, `base64_*`, `ed25519_*` (sign/verify), `crypt_verify`, `const_time_eq`; fail-loud cross-platform CSPRNG | `lume-core/src/builtins_crypt.c:254`, `src/compat/{sha256,sha512,ed25519}` | **No symmetric `encrypt`/`decrypt` pair** — see below |
| PII at rest | No field-level primitive. Policy (2026-11-09): **conditional (b)** — storage layer owns it; pure-lume/SQLite services must not collect PII | `docs/PII_AT_REST.md`, host `SECURITY.md`, `TODO.md` P19 | AES/Fernet equivalent, external KMS integration |
| Retention / timers | `session_prune_thread` pattern exists in the production server | `agent-httpd/src/core/framework.c:389`, created at `:627` | Generic application timer hook; **no timers in `bridge_serve`** (use external cron + one-shot CLI) |
| Secrets | `env()` masks credential names from DSL; runtime C reads them with `getenv` directly; `.env.example` documents key names | `lume-core/src/builtins_fs.c:95-120`, `agent-httpd/src/agent/llm.c:16` | Secrets in committed `.lume`/config (prohibited) |
| Deployment | One statically linked C11 binary, zero dynamic deps beyond libc; release tarball runs on `scratch`/`alpine`; TLS for outbound LLM calls belongs to forked `curl`, **no OpenSSL in the server link** | host `README.md:87`, `agent-httpd/README.md:884`, `agent-httpd/src/agent/llm.c:16` | Python/Node runtime in the backend container |
| Tooling / validation | 3 backends (interpreter / `--compile-text` / `--compile-llvm`) with `native-consistency` parity, `make check --strict` via `scripts/sync-lang.py`, `-Werror`, asan + Windows + macOS CI legs | `lume-core/Makefile:585`, `lume/scripts/sync-lang.py` | OpenAPI / auto-generated API docs |

## "Deliberately not filed" (lume-core P13)

**No ORM, no schema migrations, no OpenAPI, no Redis client, no middleware
chain.** These are intentional trades for the zero-dependency property. Do
not re-file them as gaps in a framework comparison.

## Related

- `docs/PII_AT_REST.md` — 2026-11-09 policy research (AEAD builtin vs
  storage layer vs framework-layer encryption), including the measured
  10-file / 11-point builtin edit surface and the `HOST_PINNED` caveat.
- `TODO.md` P13 (10-30 migration inventory), P19 (11-09 re-review +
  erratum); agent-httpd `TODO.md` P2/P3; host `TODO.md` P13 addendum.

When adding a capability, update this table in the same commit as the code —
a matrix that lags is how the next review reaches the wrong conclusion.

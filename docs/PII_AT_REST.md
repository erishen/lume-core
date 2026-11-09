# PII at rest — policy research (2026-11-09)

**Status: research only. No policy has been decided.** The open decision lives
in `TODO.md` P19 ("(a) add an AEAD builtin pair keyed from an env secret, or
(b) declare PII-at-rest out of scope for pure-lume services and make the data
store / an external KMS own it"). This document supplies the evidence for that
decision; it does not take it. Cross-refs: agent-httpd `TODO.md` P3, host
`TODO.md` P13 addendum, `personal/web-services/fastapi-web/app/crypto_pii.py`.

## 1. Why this is a policy question

`SECURITY.md` (host, 38 lines) only states that credentials belong in the
environment and never in a committed `.lume`/config. It says nothing about
encryption at rest, key ownership, or retention. There is no existing policy
to apply — this is a genuine gap, not a forgotten decision.

The reference implementation is deliberately narrow: `crypto_pii.py` derives a
Fernet key as `base64.urlsafe_b64encode(sha256(SECRET_KEY))`, encrypts
`name`/`phone` before insert, decrypts at the response boundary, and returns
non-token values unchanged so legacy plaintext rows stay readable. There is
**no key id, no AAD, no rotation path**; rotating `SECRET_KEY` makes existing
rows unreadable. Any option (a) design should not copy that limitation.

## 2. Hard constraints, verified

| Constraint | Evidence |
|---|---|
| No OpenSSL; statically linked, zero dynamic deps | `agent-httpd/src/agent/llm.c:16`, `agent-httpd/README.md:884`, host `README.md:87` |
| Vendored primitives are fresh-written C11, PD/CC0-style | `src/compat/sha256/sha256.c:1-9` ("written fresh for lume-core ... no upstream file was copied"), `src/compat/ed25519/ed25519.c` |
| Three backends must agree (interpreter / `--compile-text` / `--compile-llvm`) | `Makefile` `native-consistency` (line 585); `tests/native-consistency.lume` 1560 lines + `.expected` 608 lines |
| `sync-lang` does **not** carry builtin registration into the host | `scripts/sync-lang.py:97-133` `HOST_PINNED` contains `interp.c`, `typecheck.c`, `builtins.h`, `builtins_internal.h`, `builtins.c`, `builtins_catalog.c`, `builtins_fs.c`; `check --strict` reports the delta as lag |
| DSL must never read a secret via `env()` | `src/builtins_fs.c:95-120` masks `API_KEY`/`SECRET`/`PASSWORD`/`PASSWD`/`TOKEN`/`CREDENTIAL`; runtime reads them with `getenv` directly |

## 3. Option (a) — AEAD builtin pair in lume-core

### 3.1 Infrastructure already present (most of the work is *not* new)

- **CSPRNG**: `read_urandom()` at `src/builtins_crypt.c:254` covers Linux
  `getrandom(2)` → `/dev/urandom`, macOS `arc4random_buf`, and Windows
  `BCryptGenRandom`, and fails loudly (`vm_set_error`) rather than degrading
  to `rand()`. Nonce generation needs no new platform code. Co-locating the
  AEAD implementation in `builtins_crypt.c` reuses this `static` helper.
- **KDF / encoding**: `src/compat/sha256/sha256.c` and the existing
  `base64_encode`/`base64_decode` in `builtins_crypt.c` cover key derivation
  and token packaging.
- **Secret handling**: the `env()` mask plus `getenv` in the C runtime is the
  established pattern (`src/builtins_catalog.c:159`, `builtins_http.c:477`).
  An AEAD builtin should read its key in C and expose no key argument to the
  DSL. Name the variable so the mask catches it (e.g. `LUME_PII_SECRET`);
  a name without a credential keyword would be readable by untrusted scripts.
- **Native twin pattern**: `src/rt.c:4818-4825` (`lume_bi_crypt_verify`) and
  `src/codegen_call.c:566` show the exact shape the two native emitters need.

### 3.2 Corrigendum: "five coordinated edits" is an undercount

P13/P19 describe five conceptual edits. A builtin that returns a string and
must work on all three backends actually touches **10 source files / 11+
registration points** — measured by tracing `crypt_verify`, the most recent
crypt builtin (2026-11-04):

| File | Role |
|---|---|
| `src/typecheck.c:448` + `:544` | `LUME_BUILTIN_NAMES` + arity table (2 edits) |
| `src/interp.c:1541` | dispatch table |
| `src/builtins.h:48` | `b_*` prototype |
| `src/builtins_crypt.c` | VM implementation + native twins |
| `src/rt.c:4821` | native runtime twin |
| `src/codegen_scan.c:374` | return-type inference for the text backend |
| `src/codegen_call.c:566` + `:3368` | LLVM `declare` + emit branch (2 edits) |
| `src/llvm_cg_scan.inc:369` | LLVM scan return type |
| `src/llvm_cg_call.inc:1724` | LLVM call emission |

Plus `tests/native-consistency.lume` + `.expected`, a dedicated crypt test,
`tests/builtin-parity.allow`, and `lang/PIN.manifest` / `lang/PIN.lag`.
Because the registration files are `HOST_PINNED`, `make sync-lang` copies
`builtins_crypt.c`/`rt.c`/`codegen_*` but **never** the host's
`interp.c`/`typecheck.c`/`builtins.h`: those must be hand-merged, with the
drift recorded through `sync-lang.py accept-lag`. P19's "a host
`make sync-lang`" is therefore necessary but not sufficient.

### 3.3 Algorithm / format proposal (if (a) is chosen)

- **ChaCha20-Poly1305 (RFC 8439)**, not AES-GCM and explicitly not raw
  AES-CBC. Rationale: pure C, no AES-NI/CLMUL dependency, no table-lookup
  side channel, GHASH is easy to get wrong, and RFC 8439 §2.8 supplies
  published test vectors so the implementation is *verified*, not
  home-grown. P19's "do not ship an unverified home-grown cipher" is
  satisfied by pinning those vectors as a test, not by importing a library.
- **Token format**: `lume1.<key-id>.<base64(nonce ‖ ciphertext ‖ tag)>`.
  A 12-byte CSPRNG nonce per encryption, never reused under one key. The
  `key-id` fixes the rotation hole in `crypto_pii.py`: decrypt selects the
  key by id, so a rotation does not orphan old rows. Optional AAD binds
  table/column names to prevent ciphertext relocation.
- **Backward compatibility** (P19's second checkbox): unparseable / non-
  `lume1.` values return unchanged, never a 500 and never an empty string —
  exactly the `InvalidToken → return value` rule in `crypto_pii.py`.
- **Fail-closed on missing key**: if the secret is unset, `encrypt` raises
  instead of silently storing plaintext. Production startup should refuse
  to serve a PII-collecting route without it (cf. fastapi-web's
  `SECRET_KEY`/`ADMIN_PASSWORD_HASH` production gate).
- **Blind index, if SQL lookups are needed**: randomised AEAD makes
  `WHERE phone = ?` impossible. A deterministic `hmac_sha256(key, phone)`
  column (the primitive already exists) restores equality lookup without
  exposing the plaintext. Note the reference service does **not** need this:
  `app/anti_spam.py:25` de-duplicates in a process-local dict and no SQL
  in that project filters on `name`/`phone`. Only add a blind index if a
  lume port genuinely queries by PII.

### 3.4 Acceptance checklist for (a)

1. RFC 8439 §2.8 vectors green on Linux, macOS and the Windows CI lane.
2. Round-trip + tamper-rejection + nonce-uniqueness (two encryptions of the
   same plaintext differ) + wrong-key rejection.
3. Legacy plaintext passthrough pinned as a test (P19 checkbox 2).
4. `native-consistency` case agrees across interpreter / text / LLVM;
   `make check --strict` clean.
5. `sync-lang` real sync + host hand-merge of pinned registration files +
   `PIN.lag` accepted, no `PERMANENT` allow-list entry smuggled in.
6. Key rotation drill: decrypt a row written under key-id 1 after key-id 2
   is active.

## 4. Option (b) — storage / external KMS owns it

P19 says (b) "keeps the zero-dependency property". True for the binary, but
the coverage is **not** uniform, and for the pure-lume deployment shape it is
effectively unachievable:

| Backend | What storage-layer encryption can do | Residual gap |
|---|---|---|
| MySQL / MariaDB (`WITH_MYSQL=1`, `db_layer.c:355`) | InnoDB TDE via keyring plugin; encrypt binlog/redo | TDE protects data files only. `db_write_exec` uses prepared statements (`MYSQL_BIND`, `db_layer.c:512`); plaintext can still reach general/slow logs, binlog, memory, backups and read replicas unless separately configured |
| PostgreSQL (`WITH_PG=1`, `PQexecParams`) | `pgcrypto` column functions, or filesystem/TDE | `pgcrypto` puts the key into SQL parameters; community PG has no native TDE |
| **SQLite (lume-core `sql_query`/`sql_write`, the single-binary path)** | **None in-tree** | No `sqlcipher`, `sqlite3_key`, `SEE` or `SQLITE_HAS_CODEC` anywhere in either tree (verified). SQLCipher pulls OpenSSL (violates the project rule); SEE is commercial. Only whole-volume encryption (APFS/FileVault/LUKS) applies — copy the `.db` file out and it is plaintext |

Consequences:

- (b) is viable **only** for an agent-httpd deployment against a remote
  MySQL/PG instance whose operator explicitly enables TDE and log hygiene.
- For a pure `lume-core` single binary with its built-in SQLite, (b) means
  "no field-level protection at all" — precisely the compliance regression
  the host P13 addendum warns about ("any port that collects names/phone
  numbers would store them in plaintext").
- Field-level and volume-level encryption are not equivalent against DB
  dumps, backups, and read replicas. If a privacy notice claims "encrypted
  storage", the claim has to match what is actually deployed.
- An external KMS with envelope encryption still requires one encryption
  step in the application, which leads back to (a) or (c).

## 5. Option (c) — framework-layer field encryption (not in P19)

A third path is available and fits the host's existing disposition ("port
the compliance layer as handler conventions, not new builtins",
`lume/TODO.md:898-905`). Put the AEAD at the **agent-httpd framework** level
— the two uniform entry points `db_query_json()` (`db_layer.c:710`) and
`db_write_exec()` (`db_layer.c:749`) — or expose it as an MCP tool to DSL
handlers rather than as a language builtin.

- Avoids the entire §3.2 edit surface: no `typecheck`/`interp`/LLVM changes,
  no `native-consistency` triple, no host-owned hand-merge, one tree to
  maintain, secrets stay in C.
- Keeps field-level protection, so it works with SQLite too.
- Cost: it does **not** cover the standalone `bridge_serve` / pure-lume-core
  shape; it needs a new agent-httpd tool plus gate/parity discipline (cf.
  the P14 spawn decision: no new server primitive without its own gate), and
  it is a new framework responsibility to document and own.
- Note the project has no general middleware chain; the custom-route
  dispatch hook (`src/core/framework.c:4`) and `guard(req)` conventions are
  the accepted substitutes.

## 6. Recommendation (for the owner to accept or reject)

**Adopt (b) as the stated policy, but make it conditional and explicit.**
Concretely:

1. State in `SECURITY.md` and the P19 capability matrix: *pure-lume /
   SQLite deployments must not collect PII*; PII-at-rest belongs to the
   storage layer, and is only considered covered for agent-httpd deployments
   against MySQL/PG with TDE + log hygiene explicitly enabled by the
   operator. This closes the policy vacuum without shipping any crypto.
2. If a future port must collect PII **and** must run on the pure-lume
   shape, prefer **(c)** over (a): framework-level encryption buys field
   granularity at a fraction of (a)'s maintenance surface.
3. Treat **(a)** as the last resort. Its long-term cost is not the C code —
   CSPRNG, SHA-256, base64 and the native-twin pattern already exist — but
   the 11 registration points across three backends, the host-owned
   hand-merge, and taking on key lifecycle/rotation/backup responsibility
   in a stack whose defining property is having no such state.

Whatever is chosen, P19's two existing acceptance items still apply: pin
legacy-plaintext passthrough, and keep the retention scheduler out of the
single-threaded `bridge_serve` loop (agent-httpd P3).

## 7. Open questions for the owner

1. Is a pure-lume/SQLite service that collects names/phone numbers a
   supported use case at all? If no, (b) closes the question immediately.
2. If TDE is the answer, who owns the operational assertion that it is on —
   is there a startup check, or is it a documented deployment requirement?
3. If (a)/(c) is chosen, is key rotation in scope, and where do backups of
   the retired keys live?
4. Does the privacy notice wording ("encrypted storage") need to vary by
   deployment shape?

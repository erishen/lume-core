# Changelog

All notable changes to Lume are documented here. The format follows
[Keep a Changelog](https://keepachangelog.com/), and versions aim for
[SemVer](https://semver.org/).

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

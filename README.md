# lume-llvm

A research compiler that compiles the [Lume](https://github.com/erishen) language
down to native machine code: **`.lume` → AST → LLVM IR (text) → native binary**.

```
examples/hello.lume
        │
        │  vendored frontend (lexer / parser / typecheck)   upstream/
        ▼
      AST (Node*, Type*)
        │
        │  hand-written backend                             src/codegen.c
        ▼
      LLVM IR text (.ll)
        │
        │  clang/cc                                         src/main.c
        ▼
      ./out/hello   (runnable)
```

The frontend is not re-implemented: `upstream/` holds the real Lume lexer,
parser and type checker, vendored in and de-coupled from Lume's runtime
(the only code-level coupling in `lume.h` was `#include "agenthttpd.h"`, which
is gone). So type errors and parse errors are the upstream compiler's errors,
not ours.

## Build and run

Host toolchain is **C11 + libc only** — no LLVM dev packages, no CMake:

```sh
make                                    # -> bin/lume-llvm
make check                              # type-check every example
make test                               # build + run every example, diff output
make examples                           # build + run every example, print output
make dump f=examples/fact.lume          # print the AST
make clean
```

Single example:

```sh
./bin/lume-llvm --compile examples/fact.lume && ./out/fact
./bin/lume-llvm --emit-ir  examples/fact.lume  # writes out/fact.ll, no linking
./bin/lume-llvm --check    examples/fact.lume  # parse + typecheck only
```

`make test` compares each example's stdout against `tests/<stem>.expected`, so a
backend regression (wrong GEP index, dropped `alloca`, bad coercion) shows up as
a diff instead of a silent wrong number.

## Why LLVM IR as text instead of the LLVM C API

* **Zero link dependencies.** libLLVM is a huge shared object to link against
  for what is basically string formatting. `clang` is already on every
  developer's machine and can take a `.ll` file directly.
* **The IR stays readable**, which is the entire point of a research compiler —
  `make dump`-style inspection is possible at every stage, and the `.ll` output
  can be read to see exactly what a construct lowered to.
* **Swappable.** If this ever grows into using MLIR or the real IRBuilder,
  `src/irbuf.c` is the only file that has to change.

The cost is that nothing checks the IR but clang — which is why the backend
never emits a partial module: anything it cannot lower is a reported compile
error (`line 26: for (this construct) is not supported by the native backend
yet`) rather than a broken `.ll` for clang to choke on.

## Layout

| Path | What |
| --- | --- |
| `src/main.c` | driver: CLI, parse, typecheck, IR → native |
| `src/codegen.c` | AST → LLVM IR text (the backend) |
| `src/irbuf.c` | growable text buffer + IR string-literal escaping |
| `src/rt.c` | runtime helpers the generated IR calls (`print`) |
| `upstream/` | vendored Lume frontend (token/lexer/parser/typecheck) |
| `examples/` | `.lume` programs |
| `tests/` | expected stdout per example, used by `make test` |

## Supported subset

Types: `int`, `float`, `bool`, `string`, and `type Name = { f: T, ... }`.

* top-level `func` declarations, recursion, multiple parameters, typed returns
* `let` with or without a type annotation, assignment, member assignment
* `if` / `else`, `while`, C-style `for`, `break`, `continue`
* literals for numbers (int/float), booleans and strings
* `+ - * / %` (int) and `+ - * /` (float), `== != < <= > >=`, `! -`
* `&& ||` — lowered to plain `and`/`or` bit ops, i.e. **eager, no
  short-circuit** (the parser hands over both operands already evaluated). Fine
  for a research subset; it would need branches to become short-circuiting.
* field access and struct values (`{ w: 3, h: 4 }`, struct-returning functions)
* no string concatenation (a string arithmetic is a reported error, not a crash)

Not supported yet, rejected with a locateable error: closures / function
literals, list literals, `import`, and anything the frontend keeps only for
Lume's HTTP server (`server`, `route`, `tool`, `verbs`).

## Backend notes (the interesting part)

Locals are `alloca` + `load`/`store` rather than SSA with `phi` nodes, which
keeps the emitter trivial; `clang -O2` runs `mem2reg` and turns it into proper
SSA. IR is appended to one buffer in source order, so every function body is
scanned once before the entry block is emitted to hoist all `alloca`s there.

Things that cost real debugging time and are easy to get wrong:

* **printf is not usable from hand-written IR on macOS/arm64.** The variadic
  argument save area has to be built by the *caller*, and clang will not build
  one for a hand-written call. `print` therefore goes through
  `src/rt.c`'s non-variadic helpers (`lume_print_i64`, `lume_print_double`,
  `lume_print_bool`, `lume_print_str`); the IR only ever makes plain,
  non-variadic calls.
* **The `.ll` must declare a `target triple`.** Without one LLVM lowers for a
  generic target, the ABI disagrees and output comes out as garbage. The triple
  comes from the host toolchain (`$(CC) -print-target-triple`) and is also
  passed to the C compiler so the runtime object matches.
* **Struct field access is a two-index GEP**:
  `getelementptr inbounds %Rect, %Rect* %obj, i32 0, i32 <field>`. A single
  index is an *array* index and silently reads offset 16 instead.
* **A named struct type needs two spellings** (`%Rect` for a value, `%Rect*` for
  a pointer), and they must be interned — building them into a scratch buffer
  fails as soon as one format string asks for both in the same call.
* **Reading a struct variable yields its slot pointer**, a struct parameter
  already *is* a pointer, a struct literal is an `insertvalue` chain, and a
  struct return cannot be written as `ret %Rect 0`.

## Roadmap

* function literals / closures and list literals
* more of the frontend's surface (`import`, higher-order calls)
* a `--opt` pass over the emitted `.ll` before handing it to clang
* switch from text emission to the LLVM C API once the IR shape settles

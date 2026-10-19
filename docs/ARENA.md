# Arena allocator for the AST — P2 design (2026-10-19)

Design record for TODO.md P2. Scope: the AST memory regime only. The Type
graph (type_release_all) and its allocator are out of scope for v1 — they
already have a single centralized sweep.

## Goal

One bump arena per compilation unit. `nalloc` and every parser-owned string
route into it; `node_free` / `node_free_kids` / `node_free_own` and the
orphan journal collapse into a single arena reset. Done-when (from TODO):
the free-path patches from `ecd7afc`/`7e5d2f8` can be deleted without the
suite noticing.

## Lifetime map

Three memory regimes exist today; the arena absorbs exactly one of them.

| regime | allocations | teardown today | after arena |
|---|---|---|---|
| AST | `nalloc` node shells, parser strdup'd strings, array shells | `node_free` family + orphan journal sweep | arena reset |
| Type graph | `typecheck.c` strdup'd names, Type shells, export tables | `type_release_all()` (centralized) | unchanged |
| codegen Asgs | xstrdup'd name/slot strings per bind | `asg_truncate` / `arm_scope_truncate` (4 copies) | strings → arena; helpers degenerate to count resets |

Reset points per entry path (these are exactly today's `node_free` call
sites — the callers already know where a tree dies):

| entry path | tree consumer | today's teardown | arena reset point |
|---|---|---|---|
| compile / run CLI | parse → check → emit | end of main (type_release_all) | after emission, before exit |
| `--dump` | parser only | `node_free(prog)` main.c:473 | same point |
| fuzz cycle | parse → check | `node_free` fuzz_target.c:43 | **per input** (RSS bound; campaign peaked 504 MB) |
| loader (server/fork) | modules parsed pre-fork, interpreter reads AST per request | `node_free(m->prog)` loader.c:342 at module destroy | v1: module-**set** teardown only (watch reload / full unload) |
| LSP | parse → check per edit | `node_free` bridge_lsp.c:151 | per document version |

## What moves into the arena

- `nalloc` → `arena_alloc` (node shells). The orphan journal is deleted:
  journaling existed solely to reclaim mid-parse garbage, which an arena
  reclaims wholesale.
- Parser-owned strings currently freed by `node_free_own`: LIT_STR text
  (parse_primary mallocs and reallocs while merging adjacent literals —
  becomes arena alloc + grow-copy), identifier names, route
  method/alias/path, tool name/desc, import path/ns, type_decl/enum field
  names, match/if-let bind names, local-fn cnames, capture names.
- Array shells: program/block stmts, list items, call args, map
  keys/vals, enum vnames/vfields, match arms, param vectors.
- Codegen `Asgs` name/slot strings (both emitters) — see Stage 3.

## What stays out (v1)

- The Type graph: `type_release_all` keeps its regime. Its skips of the
  mixed vectors (below) become unnecessary once the AST side stops freeing
  them; remove those skips with the suite as the judge.

## Lifetime hazards (enumerated before writing code)

1. **Source-buffer borrows.** Non-string `N_LITERAL`s and token text point
   into the source buffer. Already true today: the buffer must outlive the
   tree. Under the arena it must outlive the arena — same constraint, pin
   it with a comment at the reset points.
2. **`node_unjournal` ownership-transfer dances** (parser_expr.c:779, 902,
   1132, 1141): shells move fields between nodes and leave the journal
   first so the sweep cannot double-reach them. With no journal and no
   individual frees these calls become no-ops — delete them; the moved
   fields stay valid because arena memory is never freed mid-unit.
3. **Parse-error paths.** Today a failed parse sweeps the journal; under
   the arena the partial tree is simply abandoned. The invariant pinned in
   `parse_program`'s fatal-by-comment (never walk journaled children after
   a failed parse) dies with the journal — the hazard class the eight
   fuzz-bad fixtures cover becomes structurally unreachable. Keep the
   fixtures; they now assert clean abandonment.
4. **Mixed Type vectors — the one place both regimes touch one array.**
   `N_LET` `annot->types` (TY_FUNC annotation param vector), `param_types`,
   `letpat.types`, enum `vfield_types`: AST-owned arrays of Type* elements
   owned by `type_release_all`. Today `node_free_kids` frees the vectors
   (and NULLs annot->types so the Type sweep cannot reach them) while
   `type_release_all` skips them. Under the arena the vectors are arena
   memory: the frees and the NULL-stamps disappear, and type_release_all's
   skips degenerate. Flip both sides in one commit.
5. **Checker borrows.** The scope stores AST `char*` (pattern binds) and
   Type* by reference. Verify before Stage 2 that scope teardown frees
   nothing (expected: it frees nothing today); if any free exists it
   becomes a count reset.
6. **Allocation extends past parse.** The signature pass (codegen_sig.c)
   stamps `cname`/`cap_names` into AST nodes; emitters read the tree
   throughout. The arena must live until after signature pass + both
   emitters — satisfied by resetting at end-of-emission, never at
   end-of-parse.
7. **Loader holds many trees simultaneously** (pre-fork model). A single
   global arena cannot reclaim one module. v1 resets only where the whole
   set dies (watch reload, LSP version change, fuzz cycle, process exit).
   If an individual-module hot-unload path is ever needed, that is the
   trigger for per-module arena handles — not before.
8. **Interpreter reads the AST at request time** (workers interpret from
   the tree). Reset after the serve loop ends, never per request.
9. **Host sync debt.** parser.c is synced (auto-carries). typecheck.c is
   HOST_OWNED → the mixed-vector flip owes the host a hand-merge.
   loader.c/main.c host copies diverge → same edits by hand there.
10. **Double-copy trap.** The Asgs machinery has four copies: `asg_truncate`
    in codegen.c:72 and llvm_codegen.c:91, `arm_scope_truncate` in
    codegen_scan.c:66 and llvm_cg_stmt.inc:37. All four flip in one commit
    (case-parity rule).

## Stage plan (each stage gated by the full suite)

- **Stage 1 — parser.** Introduce the arena + `pstrdup`/`pgrow` helpers;
  redirect `nalloc` and parser strings; delete the journal and the
  `node_free` family; convert the five call sites to `arena_reset`.
  Gate: `make asan` zero leaks, all `*-test`, fuzz seed replay.
- **Stage 2 — checker coupling.** Remove the mixed-vector frees/skips
  (hazard 4/5). Gate: `make asan` + native-consistency.
- **Stage 3 — codegen strings.** Asgs strings → arena; the four truncate
  helpers degenerate to `a->n = n` in one commit (hazard 10). Gate:
  `tests/native_backends.sh` three legs + `make asan`.
- **Stage 4 — delete the free-path patches** `ecd7afc`/`7e5d2f8` (Done-when).

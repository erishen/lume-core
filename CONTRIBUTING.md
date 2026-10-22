# Contributing to lume-core

## Leak detection only works on Linux (read this before touching codegen)

The three backends (interpreter / text IR / LLVM) must stay byte-for-byte
consistent, and the native emitters (`src/codegen*.c`) plus `Val` ownership
are the parts most likely to introduce **memory leaks** — specifically in the
`--compile-text` / `--compile-llvm` paths, which allocate IR temporaries and
register names that must be freed.

**LSan (LeakSanitizer) is unavailable on macOS.** `make asan` there runs the
emitter paths but its leak detection is a silent no-op
(`detect_leaks is not supported on this platform`). A `--compile-text` leak
will pass locally on a Mac and only turn CI red on the `ubuntu-latest` `asan`
job. Do not trust a green local `make asan` as proof of leak-cleanliness.

### Before pushing changes that touch codegen*.c or Val ownership

Run the emitter/Val-ownership leak gate **on Linux**:

```sh
make asan-compile        # --compile-text + --compile-llvm + native_backends.sh under LSan
```

Pick one of:

- **CI** — the `asan` job (ubuntu) and the `test` job's Linux-only
  `make asan-compile` step both run it. Push to a branch / PR and watch.
- **Local Linux container** — `work/lume/lume-core` is mirrored to
  `../.linux-build` (see `scripts/rebuild-linux-build.sh`). Refresh it, then
  build and run inside `ubuntu:24.04` / `gcc:12`:

  ```sh
  bash scripts/rebuild-linux-build.sh --apply   # rsync source -> .linux-build
  git -C .linux-build fetch && git -C .linux-build reset --hard origin/main
  docker run --rm -v "$PWD/.linux-build:/src" -w /src gcc:12 \
      bash -c 'make -j4 asan && make asan-compile'
  ```

  The `rebuild-linux-build.sh` guard refuses to overwrite a dirty `.linux-build`
  (so it never clobbers in-progress fuzz work); use `--force` only to discard.

### The gate targets

- `make asan` — full ASan/UBSan + LSan: `--check` on every example, the
  interpreter unit tests, **the emitter/Val-ownership LSan gate**, and the
  smoke binary. The emitter gate is delegated to `asan-compile` so it is
  defined in exactly one place.
- `make asan-compile` — **just** the emitter/Val-ownership LSan gate (the part
  that only fires on Linux). Fast to re-run while iterating on emitters.

Keep `ASAN_BASE_OPTS` empty (the default) so LSan stays on; only set
`ASAN_BASE_OPTS="detect_leaks=0"` for a one-off local triage, never in CI.

## Three-backend consistency

Every language change must be verified across all three backends. `make test`
runs `tests/native_backends.sh`, which compiles each fixture with the text and
LLVM emitters, runs the interpreter, and diffs all three outputs against
`tests/*.expected`. A backend that compiles and exits 0 but prints the wrong
answer is caught there, not by the leak gate.

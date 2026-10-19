/* src/fuzz_target.c — parser fuzzing entry point.
 *
 * Feeds arbitrary bytes through the lexer → parse_program (including its
 * fail-path handling) → the type checker. No backend emission: the target
 * stays small and fast, and the corpus seeded from the hand-written
 * fixtures covers the parser/checker surface the negative fixtures probe.
 *
 * Two build shapes, one translation unit:
 *
 *   - libFuzzer (Linux clang): `LLVMFuzzerTestOneInput` is compiled in when
 *     __linux__ is defined. `-fsanitize=fuzzer` rejects the Windows GNU
 *     targets (x86_64-w64-windows-gnu), so the fuzzer binary is built by
 *     `make fuzz` on Linux only; CI runs this via the `fuzz` job in ci.yml.
 *
 *   - standalone driver: `-DLUME_FUZZ_DRIVER` compiles a main() that reads
 *     the files named on the command line and pushes each through the same
 *     path. Any compiler can build this shape, so the corpus smoke run works
 *     on Windows too (e.g. mingw gcc), with ASan/UBSan if the toolchain has
 *     them.
 */
#include <stdint.h>
#include <stddef.h>
#include <stdlib.h>
#include <string.h>
#include <stdio.h>

#include "lume.h"
#include "arena.h"

/* Shared core: one input → one parse+check cycle. Returns 1 when the input
 * parsed (regardless of whether the checker accepted it), 0 on a parse
 * failure. A crash/hang/leak here is the fuzz finding. */
static int lume_fuzz_one(const uint8_t *data, size_t size) {
    /* libFuzzer input is not NUL-terminated; the lexer reads a C string. */
    char *src = malloc(size + 1);
    if (!src) return 0;
    memcpy(src, data, size);
    src[size] = '\0';

    char err[512] = {0};
    Node *prog = parse_program(src, err, sizeof(err));
    if (prog) {
        type_check_program(prog, err, sizeof(err));
        arena_reset();     /* the parse/check cycle's only tree */
    }
    type_release_all();    /* Type graph from this cycle (idempotent) */
    free(src);
    return prog != NULL;
}

#ifdef __linux__
int LLVMFuzzerTestOneInput(const uint8_t *data, size_t size) {
    (void)lume_fuzz_one(data, size);
    return 0;
}
#endif

#ifdef LUME_FUZZ_DRIVER
int main(int argc, char **argv) {
    if (argc < 2) {
        fprintf(stderr, "usage: %s <file.lume> ...\n", argv[0]);
        return 2;
    }
    size_t total = 0, ok = 0;
    for (int i = 1; i < argc; i++) {
        FILE *f = fopen(argv[i], "rb");
        if (!f) {
            fprintf(stderr, "cannot open %s\n", argv[i]);
            return 2;
        }
        if (fseek(f, 0, SEEK_END) != 0) { fclose(f); return 2; }
        long sz = ftell(f);
        if (sz < 0 || fseek(f, 0, SEEK_SET) != 0) { fclose(f); return 2; }
        uint8_t *buf = malloc((size_t)sz + 1);
        if (!buf) { fclose(f); return 2; }
        size_t got = fread(buf, 1, (size_t)sz, f);
        fclose(f);
        if (got != (size_t)sz) { free(buf); return 2; }
        buf[sz] = 0;
        if (lume_fuzz_one(buf, (size_t)sz)) ok++;
        free(buf);
        total++;
    }
    fprintf(stderr, "fuzz-driver: %zu files, %zu parsed ok, %zu parse-fail\n",
            total, ok, total - ok);
    return 0;
}
#endif

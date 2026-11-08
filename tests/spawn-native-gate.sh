#!/bin/sh
# P14 emitter-gate regression — decision (a): spawn is interpreter-only.
#
# lume-core deliberately has NO native child-process surface. The front end
# registers spawn/proc_* (typecheck.c) and the interpreter implements them
# behind the --spawn + LUME_SPAWN_ALLOW gate, but the two native emitters and
# rt.c must stay free of them: a compiled binary has no VM policy to consult,
# so a helper there would be an ungated process primitive. See TODO.md P14
# (step-2 security finding) for the full argument.
#
# Wired into `make check` (Linux, macOS and Windows/mingw lanes). Three checks,
# any of which fails the gate:
#   1. static  — no spawn registration token in the emitters / rt.c
#   2. dynamic — --check accepts the fixture (typecheck knows spawn), while
#                --compile-text rejects it with "call to unknown function"
#   3. positive control (POSIX only) — the interpreter leg CAN still spawn
#                when the gate is opened, so 1+2 were not achieved by
#                removing the builtin from the language entirely.
# A self-check feeds a known-bad sample through the same static matcher, so a
# pattern that has gone blind cannot turn this gate green.

set -eu

cd "$(dirname "$0")/.."

BIN=./bin/lume-core
if [ ! -x "${BIN}" ]; then BIN=./bin/lume-core.exe; fi
if [ ! -x "${BIN}" ]; then
    echo "FAIL spawn-native-gate: build the binary first (make all)"
    exit 1
fi

STATIC_FILES="src/rt.c src/codegen_call.c src/llvm_codegen_internal.h src/codegen_scan.c src/llvm_cg_scan.inc"
SPAWN_TOKEN_RE='"(spawn|proc_read|proc_wait|proc_kill)"|native_spawn|b_spawn|lume_spawn'

# Keep every temp artefact in one directory, including the -o output of the
# failed native compile, so a previous run's binary can never make the gate
# look green (or red) by accident.
tmp=$(mktemp -d)
trap 'rm -rf "$tmp"' 0
trap 'exit 1' 1 2 3 15

# --- 0. self-check: the matcher must actually detect a registered spawn ----
printf '%s\n' 'static const Bi BUILTINS[] = { { "spawn", "lume_bi_spawn" } };' > "${tmp}/selfcheck.c"
if ! grep -Eq "${SPAWN_TOKEN_RE}" "${tmp}/selfcheck.c"; then
    echo "FAIL spawn-native-gate: static matcher is blind (self-check)"
    exit 1
fi
echo "ok   spawn-native-gate: static matcher self-check"

# --- 1. static: emitters + rt.c carry no spawn registration ---------------
for f in ${STATIC_FILES}; do
    if [ ! -f "${f}" ]; then
        echo "FAIL spawn-native-gate: missing ${f}"
        exit 1
    fi
    if grep -nE "${SPAWN_TOKEN_RE}" "${f}" > "${tmp}/hits" 2>/dev/null; then
        echo "FAIL spawn-native-gate: ${f} registers a spawn token:"
        cat "${tmp}/hits"
        echo "       decision (a) keeps spawn interpreter-only; see TODO.md P14"
        exit 1
    fi
done
echo "ok   spawn-native-gate: no spawn token in emitters / rt.c"

# --- 2. dynamic: front end knows spawn, the text emitter refuses it -------
FIXTURE=tests/spawn-native-denied.lume
if [ ! -f "${FIXTURE}" ]; then
    echo "FAIL spawn-native-gate: missing ${FIXTURE}"
    exit 1
fi

if ! "${BIN}" --check "${FIXTURE}" > "${tmp}/check.out" 2>&1; then
    echo "FAIL spawn-native-gate: --check rejected the fixture (typecheck lost spawn):"
    cat "${tmp}/check.out"
    exit 1
fi
echo "ok   spawn-native-gate: --check knows spawn"

if "${BIN}" --compile-text "${FIXTURE}" -o "${tmp}/denied" > "${tmp}/compile.out" 2>&1; then
    echo "FAIL spawn-native-gate: --compile-text ACCEPTED a spawn call"
    cat "${tmp}/compile.out"
    exit 1
fi
if ! grep -q "unknown function" "${tmp}/compile.out"; then
    echo "FAIL spawn-native-gate: native compile failed for the wrong reason:"
    cat "${tmp}/compile.out"
    exit 1
fi
if [ -e "${tmp}/denied" ] || [ -e "${tmp}/denied.exe" ]; then
    echo "FAIL spawn-native-gate: a native binary was still produced"
    exit 1
fi
echo "ok   spawn-native-gate: --compile-text refuses spawn (call to unknown function)"

# --- 3. positive control (POSIX only): interpreter leg still spawns -------
case "$(uname -s 2>/dev/null || echo unknown)" in
    MINGW*|MSYS*|CYGWIN*|Windows_NT)
        echo "skip spawn-native-gate: interpreter positive control (no POSIX spawn on Windows)"
        ;;
    *)
        # /bin/echo resolves inside /bin (macOS) or /usr/bin (usrmerge Linux);
        # both roots are listed, so the realpath boundary check passes on both.
        if ! env -u LUME_SPAWN -u LUME_SPAWN_ALLOW -u LUME_SPAWN_ROOT \
                LUME_SPAWN=1 LUME_SPAWN_ALLOW=/bin/echo \
                LUME_SPAWN_ROOT=/bin:/usr/bin \
                "${BIN}" tests/spawn-native-gate.lume > "${tmp}/interp.out" 2>&1; then
            echo "FAIL spawn-native-gate: interpreter positive control crashed:"
            cat "${tmp}/interp.out"
            exit 1
        fi
        if ! grep -q "PASS interpreter spawn available" "${tmp}/interp.out" || \
           grep -q FAIL "${tmp}/interp.out"; then
            echo "FAIL spawn-native-gate: interpreter positive control did not pass:"
            cat "${tmp}/interp.out"
            exit 1
        fi
        echo "ok   spawn-native-gate: interpreter leg still spawns (gated)"
        ;;
esac

echo "==> spawn-native-gate: decision (a) holds — interpreter-only, native closed"

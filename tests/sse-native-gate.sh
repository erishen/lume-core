#!/bin/sh
# P18 emitter-gate regression — server-side SSE push stays interpreter-only.
#
# sse_open/sse_send/sse_close are registered in typecheck.c and implemented by
# the interpreter (builtins_http.c), but they must NOT appear in either native
# emitter (codegen_call.c / llvm_codegen_internal.h) or rt.c. A compiled binary
# has no VM request connection to push onto, so an SSE primitive there would be
# an ungated streaming surface — the same rationale as the spawn gate (TODO.md
# P14, decision (a)) and the inbound http_stream_* gate (P17). See TODO.md P18.
#
# Wired into `make check`. Three checks, any of which fails the gate:
#   1. static  — no SSE registration token in the emitters / rt.c
#   2. dynamic — --check accepts the fixture (typecheck knows sse_open),
#                while --compile-text rejects it with "call to unknown function"
#   3. self-check — the static matcher must actually detect a registered SSE
#                token, so a blind pattern cannot turn this gate green.

set -eu

cd "$(dirname "$0")/.."

BIN=./bin/lume-core
if [ ! -x "${BIN}" ]; then BIN=./bin/lume-core.exe; fi
if [ ! -x "${BIN}" ]; then
    echo "FAIL sse-native-gate: build the binary first (make all)"
    exit 1
fi

STATIC_FILES="src/rt.c src/codegen_call.c src/llvm_codegen_internal.h src/codegen_scan.c src/llvm_cg_scan.inc"
SSE_TOKEN_RE='sse_open|sse_send|sse_close|native_sse|b_sse|lume_serve_sse'

tmp=$(mktemp -d)
trap 'rm -rf "$tmp"' 0
trap 'exit 1' 1 2 3 15

# --- 0. self-check: the matcher must actually detect a registered SSE token ---
printf '%s\n' 'static const Bi BUILTINS[] = { { "sse_open", "lume_bi_sse" } };' > "${tmp}/selfcheck.c"
if ! grep -Eq "${SSE_TOKEN_RE}" "${tmp}/selfcheck.c"; then
    echo "FAIL sse-native-gate: static matcher is blind (self-check)"
    exit 1
fi
echo "ok   sse-native-gate: static matcher self-check"

# --- 1. static: emitters + rt.c carry no SSE registration ----------------
for f in ${STATIC_FILES}; do
    if [ ! -f "${f}" ]; then
        echo "FAIL sse-native-gate: missing ${f}"
        exit 1
    fi
    if grep -nE "${SSE_TOKEN_RE}" "${f}" > "${tmp}/hits" 2>/dev/null; then
        echo "FAIL sse-native-gate: ${f} registers an SSE token:"
        cat "${tmp}/hits"
        echo "       P18 keeps sse_* interpreter-only; see TODO.md P18"
        exit 1
    fi
done
echo "ok   sse-native-gate: no SSE token in emitters / rt.c"

# --- 2. dynamic: front end knows sse_*, the text emitter refuses it --------
FIXTURE=tests/sse-native-denied.lume
if [ ! -f "${FIXTURE}" ]; then
    echo "FAIL sse-native-gate: missing ${FIXTURE}"
    exit 1
fi

if ! "${BIN}" --check "${FIXTURE}" > "${tmp}/check.out" 2>&1; then
    echo "FAIL sse-native-gate: --check rejected the fixture (typecheck lost sse_*):"
    cat "${tmp}/check.out"
    exit 1
fi
echo "ok   sse-native-gate: --check knows sse_open/sse_send/sse_close"

if "${BIN}" --compile-text "${FIXTURE}" -o "${tmp}/denied" > "${tmp}/compile.out" 2>&1; then
    echo "FAIL sse-native-gate: --compile-text ACCEPTED a sse_* call"
    cat "${tmp}/compile.out"
    exit 1
fi
if ! grep -q "unknown function" "${tmp}/compile.out"; then
    echo "FAIL sse-native-gate: native compile failed for the wrong reason:"
    cat "${tmp}/compile.out"
    exit 1
fi
if [ -e "${tmp}/denied" ] || [ -e "${tmp}/denied.exe" ]; then
    echo "FAIL sse-native-gate: a native binary was still produced"
    exit 1
fi
echo "ok   sse-native-gate: --compile-text refuses sse_* (call to unknown function)"

echo "==> sse-native-gate: P18 holds — interpreter-only, native closed"

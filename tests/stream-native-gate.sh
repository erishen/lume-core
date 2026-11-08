#!/bin/sh
# P17 emitter-gate regression — inbound SSE stays interpreter-only.
#
# http_stream_open/read/close are registered in typecheck.c and implemented by
# the interpreter (builtins_http.c), but they must NOT appear in either native
# emitter (codegen_call.c / llvm_codegen_internal.h) or rt.c. A compiled binary
# has no VM policy to consult, so a stream primitive there would be an ungated
# network surface — the same rationale as the spawn gate (TODO.md P14, decision
# (a)). See TODO.md P17 for the full argument.
#
# Wired into `make check`. Three checks, any of which fails the gate:
#   1. static  — no stream registration token in the emitters / rt.c
#   2. dynamic — --check accepts the fixture (typecheck knows http_stream_open),
#                while --compile-text rejects it with "call to unknown function"
#   3. self-check — the static matcher must actually detect a registered stream
#                token, so a blind pattern cannot turn this gate green.

set -eu

cd "$(dirname "$0")/.."

BIN=./bin/lume-core
if [ ! -x "${BIN}" ]; then BIN=./bin/lume-core.exe; fi
if [ ! -x "${BIN}" ]; then
    echo "FAIL stream-native-gate: build the binary first (make all)"
    exit 1
fi

STATIC_FILES="src/rt.c src/codegen_call.c src/llvm_codegen_internal.h src/codegen_scan.c src/llvm_cg_scan.inc"
STREAM_TOKEN_RE='"(http_stream_open|http_stream_read|http_stream_close)"|native_http_stream|b_http_stream|lume_stream'

tmp=$(mktemp -d)
trap 'rm -rf "$tmp"' 0
trap 'exit 1' 1 2 3 15

# --- 0. self-check: the matcher must actually detect a registered stream -----
printf '%s\n' 'static const Bi BUILTINS[] = { { "http_stream_open", "lume_bi_stream" } };' > "${tmp}/selfcheck.c"
if ! grep -Eq "${STREAM_TOKEN_RE}" "${tmp}/selfcheck.c"; then
    echo "FAIL stream-native-gate: static matcher is blind (self-check)"
    exit 1
fi
echo "ok   stream-native-gate: static matcher self-check"

# --- 1. static: emitters + rt.c carry no stream registration ---------------
for f in ${STATIC_FILES}; do
    if [ ! -f "${f}" ]; then
        echo "FAIL stream-native-gate: missing ${f}"
        exit 1
    fi
    if grep -nE "${STREAM_TOKEN_RE}" "${f}" > "${tmp}/hits" 2>/dev/null; then
        echo "FAIL stream-native-gate: ${f} registers a stream token:"
        cat "${tmp}/hits"
        echo "       P17 keeps http_stream interpreter-only; see TODO.md P17"
        exit 1
    fi
done
echo "ok   stream-native-gate: no stream token in emitters / rt.c"

# --- 2. dynamic: front end knows http_stream, the text emitter refuses it ----
FIXTURE=tests/stream-native-denied.lume
if [ ! -f "${FIXTURE}" ]; then
    echo "FAIL stream-native-gate: missing ${FIXTURE}"
    exit 1
fi

if ! "${BIN}" --check "${FIXTURE}" > "${tmp}/check.out" 2>&1; then
    echo "FAIL stream-native-gate: --check rejected the fixture (typecheck lost http_stream):"
    cat "${tmp}/check.out"
    exit 1
fi
echo "ok   stream-native-gate: --check knows http_stream_open"

if "${BIN}" --compile-text "${FIXTURE}" -o "${tmp}/denied" > "${tmp}/compile.out" 2>&1; then
    echo "FAIL stream-native-gate: --compile-text ACCEPTED a http_stream call"
    cat "${tmp}/compile.out"
    exit 1
fi
if ! grep -q "unknown function" "${tmp}/compile.out"; then
    echo "FAIL stream-native-gate: native compile failed for the wrong reason:"
    cat "${tmp}/compile.out"
    exit 1
fi
if [ -e "${tmp}/denied" ] || [ -e "${tmp}/denied.exe" ]; then
    echo "FAIL stream-native-gate: a native binary was still produced"
    exit 1
fi
echo "ok   stream-native-gate: --compile-text refuses http_stream (call to unknown function)"

echo "==> stream-native-gate: P17 holds — interpreter-only, native closed"

#!/bin/sh
# Windows/mingw regression for the LUME_HAS_HTTP=0 / LUME_HAS_SPAWN=0 lane.
#
# typecheck.c registers the HTTP and spawn names unconditionally, so a script
# that --check accepts must not die with `undefined variable` just because
# this build opted out of the implementations. interp.c seeds those names
# against a self-contained diagnostic stub instead (no external wrapper
# symbol, so the downstream link opt-out survives). See TODO.md P17,
# "Found during the Windows lane", and the LUME_HAS_* comment in interp.c.
#
# Wired into `make check` on the Windows lane only: POSIX builds have
# LUME_HAS_HTTP=1 / LUME_HAS_SPAWN=1 and carry the real implementations.

set -eu

cd "$(dirname "$0")/.."

BIN=./bin/lume-core
if [ ! -x "${BIN}" ]; then BIN=./bin/lume-core.exe; fi
if [ ! -x "${BIN}" ]; then
    echo "FAIL unavailable-builtins-gate: build the binary first (make all)"
    exit 1
fi

tmp=$(mktemp -d)
trap 'rm -rf "$tmp"' 0
trap 'exit 1' 1 2 3 15

# name / one-line call expression / fixed substring of the expected error
check_unavailable() {
    name=$1
    expr=$2
    expected=$3
    printf '%s\n' "let x = ${expr};" > "${tmp}/${name}.lume"

    if ! "${BIN}" --check "${tmp}/${name}.lume" > "${tmp}/${name}.check" 2>&1; then
        echo "FAIL unavailable-builtins-gate: --check rejected ${name} (typecheck lost the name):"
        cat "${tmp}/${name}.check"
        exit 1
    fi

    if "${BIN}" "${tmp}/${name}.lume" > "${tmp}/${name}.out" 2>&1; then
        echo "FAIL unavailable-builtins-gate: ${name} unexpectedly succeeded on this build:"
        cat "${tmp}/${name}.out"
        exit 1
    fi
    if grep -Fq 'undefined variable' "${tmp}/${name}.out"; then
        echo "FAIL unavailable-builtins-gate: ${name} fell back to 'undefined variable':"
        cat "${tmp}/${name}.out"
        exit 1
    fi
    if ! grep -Fq "${expected}" "${tmp}/${name}.out"; then
        echo "FAIL unavailable-builtins-gate: ${name} did not report the clear error:"
        cat "${tmp}/${name}.out"
        exit 1
    fi
    echo "ok   unavailable-builtins-gate: ${name}"
}

HTTP_MSG='unavailable: this build was compiled without LUME_HAS_HTTP'
SPAWN_MSG='unavailable: this build has no POSIX process support'

check_unavailable http_get    'http_get("http://127.0.0.1:1/")'            "${HTTP_MSG}"
check_unavailable http_stream 'http_stream_open("http://127.0.0.1:1/sse")' "${HTTP_MSG}"
check_unavailable http_post   'http_post("http://127.0.0.1:1/", "x")'      "${HTTP_MSG}"
check_unavailable spawn       'spawn(["/bin/echo", "x"])'                   "${SPAWN_MSG}"

echo "==> unavailable-builtins-gate: opted-out builtins fail clearly, not as undefined variables"

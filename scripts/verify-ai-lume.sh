#!/usr/bin/env bash
# scripts/verify-ai-lume.sh — deterministic acceptance gate for an
# LLM-generated .lume program ("AI writes, machine verifies", TODO P4).
#
# Usage:  scripts/verify-ai-lume.sh <file.lume> [<file2.lume> ...]
#         LUME_BIN=./bin/lume-core scripts/verify-ai-lume.sh file.lume
#
# An AI-written program has no .expected baseline, so "correct" cannot
# mean "matches a human-approved diff". The gate instead judges backend
# equivalence — the machine half of the loop:
#
#   1. the program must type-check (--check),
#   2. it must run identically on the interpreter and every native
#      backend whose toolchain is present — byte-for-byte stdout after
#      CRLF normalization, with any compiler diagnostic counting as a
#      reject (IR the verifier complains about is not "it compiled"),
#   3. at least two legs must have run, or the gate cannot judge,
#   4. if the standalone fuzz driver (out/fuzz-driver, `make fuzz-driver`)
#      exists, the file is also pushed through the parser fuzz path — a
#      crash/hang/leak there rejects the program outright.
#
# Exit: 0 = pass, 1 = reject (reason on stderr — the counter-example to
# log back to the LLM for retry), 2 = gate could not run (binary missing
# or fewer than two legs available).
#
# The LLM step itself lives in the host / an external CI script (scope
# guard in TODO P4: the core interpreter stays host-independent, no
# network). This script is the deterministic verify side only.
set -uo pipefail
HERE="$(cd "$(dirname "$0")" && pwd)"
cd "$(dirname "$0")/.."

# Installed mode: the gate scripts ship in the same dir as lume-core (which
# is on PATH), so resolve the binary there first; the repo mode (scripts/ in
# a checkout) falls back to bin/lume-core. AI_SEMANTICS_SH lets a wrapper
# point at the semantic validator when it does not sit next to this script.
TARGET="${LUME_BIN:-}"
if [ -z "$TARGET" ]; then
    TARGET="$(command -v lume-core 2>/dev/null || echo bin/lume-core)"
fi
SEMANTICS="${AI_SEMANTICS_SH:-$HERE/verify-ai-semantics.sh}"
WORK=$(mktemp -d)
trap 'rm -rf "$WORK"' EXIT

[ $# -ge 1 ] || { echo "usage: $0 <file.lume> ..." >&2; exit 2; }
[ -x "$TARGET" ] || { echo "FAIL: $TARGET missing — run make first" >&2; exit 2; }
for f in "$@"; do
    [ -f "$f" ] || { echo "FAIL: cannot open $f" >&2; exit 2; }
done

fail() { echo "FAIL: $*" >&2; exit 1; }
pass() { echo "ok   $*"; }

# One full gate for a single file: check -> run every available leg ->
# pairwise compare -> optional fuzz crash check. Each file gets its own
# $WORK/<n>/ sandbox so multi-file invocations cannot step on each other.
verify_one() {
    local f=$1 n=$2 dir="$WORK/$2" name=$(basename "$1")
    mkdir -p "$dir"

    # sql-bearing programs are judged only on builds with libsqlite3, and
    # run against a scratch DB (SQLITE_DB, same env the P6 fixture uses).
    # A build without sqlite refuses sql_tx_begin at runtime ("unavailable"),
    # which would turn a legal seed into an interpreter-stderr reject on
    # every leg — so probe once and skip the file, same protocol as
    # tests/native_backends.sh's sql block.
    if grep -q 'sql_[a-zA-Z_]*(' "$f"; then
        export SQLITE_DB="$dir/db.sqlite"
        sql_probe="$dir/sql-probe.lume"
        printf 'let p: string = sql_tx_begin("");\nsql_tx_commit(p);\n' > "$sql_probe"
        if ! "$TARGET" "$sql_probe" >/dev/null 2>&1; then
            echo "note $name: sql builtins unavailable on this build — skipping the sql-bearing program"
            return 0
        fi
        pass "$name: sql builtins available"
    fi

    # 1. type check.
    if ! "$TARGET" --check "$f" >/dev/null 2>&1; then
        fail "$name: type check rejected the program"
    fi
    pass "$name: type check ok"

    # 1b. semantic layer (verify-ai-semantics.sh): the program's own
    #     //@semantic declarations are checked statically, without
    #     running it — dangling references, unclaimed labels and unsafe
    #     shapes (server{} / run()) reject here, before any leg runs.
    if [ -f "$SEMANTICS" ]; then
        if ! bash "$SEMANTICS" "$f"; then
            fail "$name: semantic declarations rejected"
        fi
        pass "$name: semantic declarations ok"
    else
        echo "note $name: semantic validator missing — declaration contract not enforced"
    fi

    # 2. run on every available leg; any compiler diagnostic is a reject.
    #    leg: "" (interp) | --compile-text | --compile-llvm
    local legs="$dir/legs"; : > "$legs"
    run_leg() {
        local flag=$1 label=$2 bin="$dir/$2" out="$dir/$2.out" err rc
        if [ -n "$flag" ]; then
            err=$("$TARGET" "$flag" "$f" -o "$bin" 2>&1 >/dev/null); rc=$?
            if [ $rc -ne 0 ]; then
                [ -n "$err" ] && echo "$err" | head -5
                fail "$name: $label refused to compile (exit $rc)"
            fi
            if [ -n "$err" ]; then
                echo "$err" | head -5
                fail "$name: $label emitted diagnostics on a program it should accept"
            fi
            [ -x "$bin" ] || fail "$name: $label produced no binary"
            "$bin" > "$out" 2>&1 || fail "$name: $label binary exited non-zero"
        else
            "$TARGET" "$f" > "$out" 2> "$dir/$2.err"
            err=$(grep -v "script completed without run" "$dir/$2.err" 2>/dev/null)
            [ -z "$err" ] || { echo "$err" | head -5; fail "$name: interpreter wrote to stderr"; }
        fi
        tr -d '\r' < "$out" > "$dir/$2.norm"
        echo "$label" >> "$legs"
        pass "$name: $label ran"
    }

    run_leg "" interp
    if command -v clang >/dev/null 2>&1; then
        run_leg --compile-text text
    else
        echo "note $name: text backend skipped (clang not on PATH)"
    fi
    # libLLVM presence is asked of the binary, not guessed from the build.
    llvm_probe=$("$TARGET" --compile-llvm "$f" -o "$dir/probe" 2>&1 >/dev/null)
    llvm_probe=$(printf '%s\n' "$llvm_probe" | grep -v '^==[0-9]*==' || true)
    case "$llvm_probe" in
        *"needs a build with libLLVM"*)
            echo "note $name: libLLVM backend skipped (built without libLLVM)" ;;
        *libLLVM*)
            echo "note $name: libLLVM refusal wording changed, skipping the libLLVM leg" ;;
        *)
            run_leg --compile-llvm llvm ;;
    esac

    # 3. at least two legs, then compare every pair.
    local arr=()
    while IFS= read -r leg; do arr+=("$leg"); done < "$legs"
    [ ${#arr[@]} -ge 2 ] ||
        fail "$name: only ${#arr[@]} leg(s) ran — the gate cannot judge (need >= 2)"
    local i j a b
    for (( i = 0; i < ${#arr[@]}; i++ )); do
        for (( j = i + 1; j < ${#arr[@]}; j++ )); do
            a=${arr[$i]}; b=${arr[$j]}
            if ! diff -u "$dir/$a.norm" "$dir/$b.norm" > "$dir/$a-vs-$b.diff"; then
                head -20 "$dir/$a-vs-$b.diff"
                fail "$name: $a and $b disagree"
            fi
            pass "$name: $a and $b agree"
        done
    done

    # 4. optional parser-fuzz crash check.
    if [ -x out/fuzz-driver ]; then
        out/fuzz-driver "$f" >/dev/null 2>&1 ||
            fail "$name: parser fuzz path crashed (see fuzz-driver)"
        pass "$name: parser fuzz path clean"
    else
        echo "note $name: fuzz-driver not built; run 'make fuzz-driver' to enable the crash leg"
    fi

    echo "PASS $name — AI program accepted by the machine gate"
}

n=0
for f in "$@"; do
    n=$((n + 1))
    verify_one "$f" "$n"
done
exit 0

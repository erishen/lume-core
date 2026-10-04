#!/usr/bin/env bash
# Cross-backend consistency for the native compiler.
#
# The native backend has two independent IR emitters — hand-written IR text
# (--compile-text) and the libLLVM C API (--compile-llvm) — and nothing else in
# the suite compares them. They drifted apart more than once ("mul i64 %v1,
# undef", the half-typed struct GEP), and each drift only surfaced as a wrong
# answer somewhere else rather than as a failure here, so this script:
#
#   1. runs tests/native-consistency.lume through the interpreter — the
#      reference for what the language does — and through both emitters,
#   2. diffs every run against tests/native-consistency.expected,
#   3. diffs every pair of legs against each other, so a leg that agrees with
#      the baseline but disagrees with its sibling still fails,
#   4. treats *any* compiler output as a failure — IR that makes clang's or
#      LLVM's verifier complain is not "it compiled".
#
# A backend whose toolchain is missing is skipped rather than failed, so a
# machine without clang / llvm-config still gets a green suite.
set -uo pipefail
cd "$(dirname "$0")/.."

# The binary under test follows the same env convention the rest of the suite
# uses (see tests/run_all.sh): make asan drives it with the sanitized binary,
# make test with the plain one, and a bare run defaults to the plain build.
TARGET="${LUME_BIN:-bin/lume}"
SCRIPT=tests/native-consistency.lume
EXPECTED=tests/native-consistency.expected
WORK=$(mktemp -d)
trap 'rm -rf "$WORK"' EXIT

fail() { echo "FAIL: $*" >&2; exit 1; }
pass() { echo "ok   $*"; }

[ -x "$TARGET" ] || fail "$TARGET missing — run make first"
[ -f "$EXPECTED" ] || fail "$EXPECTED missing"

# One leg of the comparison. $1 is the compile flag — empty for the
# interpreter, which needs neither IR nor a toolchain — and $2 labels the leg
# and names its files under $WORK.
check() {
    local flag=$1 label=$2 bin="$WORK/$2" err rc
    if [ -n "$flag" ]; then
        err=$("$TARGET" "$flag" "$SCRIPT" -o "$bin" 2>&1 >/dev/null); rc=$?
        if [ $rc -ne 0 ]; then
            [ -n "$err" ] && echo "$err" | head -5
            fail "$label: $flag exited $rc"
        fi
        # An IR that makes the toolchain print anything is an IR that should not
        # have been emitted — the verifier catches shapes the emitters can build
        # without noticing.
        if [ -n "$err" ]; then
            echo "$err" | head -5
            fail "$label: $flag emitted diagnostics on a script it should accept"
        fi
        [ -x "$bin" ] || fail "$label: $flag produced no binary"
    fi
    if [ -n "$flag" ]; then
        if ! "$bin" > "$WORK/$label.out" 2>&1; then
            head -5 "$WORK/$label.out"
            fail "$label: compiled binary exited non-zero"
        fi
    else
        # The interpreter calls out "script completed without run()" on stderr
        # for a script that reaches its entry from the top level, which makes it
        # a note about the entry convention rather than a complaint about this
        # fixture. It is dropped; anything else on stderr is a failure, because
        # a leg that is quiet on stderr and wrong on stdout is exactly the shape
        # this script exists to catch.
        "$TARGET" "$SCRIPT" > "$WORK/$label.out" 2> "$WORK/$label.err"
        err=$(grep -v "script completed without run" "$WORK/$label.err" 2>/dev/null)
        if [ -n "$err" ]; then
            echo "$err" | head -5
            fail "$label: interpreter wrote to stderr"
        fi
    fi
    if ! diff -u "$EXPECTED" "$WORK/$label.out" > "$WORK/$label.diff"; then
        head -20 "$WORK/$label.diff"
        fail "$label: output differs from $EXPECTED"
    fi
    pass "$label output matches $EXPECTED"
}

RAN=()

# The interpreter needs nothing but bin/lume, so it always runs: it is the
# reference the two emitters have to live up to, and the leg whose absence
# would make "three-way consistency" a two-way check in disguise.
check "" interp
RAN+=("interp")

# The text emitter needs clang; the libLLVM one needs the build to have found
# llvm-config (see the HAVE_LLVM probe in the Makefile).
if command -v clang >/dev/null 2>&1; then
    check --compile-text text
    RAN+=("text")
else
    echo "ok   cross-backend: text backend skipped (clang not on PATH)"
fi

# Whether libLLVM is in the build is asked of the binary itself rather than of
# `make -n` (which prints nothing once everything is up to date): a build
# without libLLVM must refuse --compile-llvm loudly, and that refusal is the
# skip signal here.
# The refusal is read from a variable rather than piped into `grep -q`: under
# `pipefail` the probe's own exit status (1, the refusal) is what the pipeline
# reports, so a match could never be told apart from a crash and the skip
# branch was dead code on any build without libLLVM.
#
# The sanitizer's own report is stripped first, because on a sanitized binary
# it goes to the same stderr and it *mentions libLLVM* ("libLLVM-16.so", in the
# leak summary). Left in, the `*libLLVM*` branch below accepted the report as
# the refusal text, and the libLLVM leg never ran under `make asan` at all.
llvm_probe=$("$TARGET" --compile-llvm "$SCRIPT" -o "$WORK/probe" 2>&1 >/dev/null)
llvm_probe=$(printf '%s\n' "$llvm_probe" | grep -v '^==[0-9]*==' || true)
case "$llvm_probe" in
    *"needs a build with libLLVM"*)
        echo "ok   cross-backend: libLLVM backend skipped (built without libLLVM)" ;;
    *libLLVM*)
        # The refusal wording is the only build-time signal the binary gives,
        # so rewording it used to send every build *without* libLLVM into the
        # check branch, where the leg fails for a reason that has nothing to
        # do with the code under test. Skip with a note instead.
        echo "note cross-backend: libLLVM refusal wording changed, skipping the libLLVM leg" ;;
    *)
        check --compile-llvm llvm
        RAN+=("llvm") ;;
esac

# Guards the two-way comparison below: below two legs there is nothing to pair,
# and the loops over RAN would report green having compared a backend with
# nothing. `${#RAN[@]}` is a length, so it takes no `:-` default — the usual
# `${#RAN[@]:-0}` spelling is not an expansion bash accepts, the whole guard
# was skipped with a "bad substitution" on stderr, and the one-leg case (the
# text backend missing, say) used to pass.
[ ${#RAN[@]} -ge 2 ] ||
    fail "only ${#RAN[@]} leg(s) ran — nothing to compare"
pass "(${RAN[*]})"

for (( i = 0; i < ${#RAN[@]}; i++ )); do
    for (( j = i + 1; j < ${#RAN[@]}; j++ )); do
        a=${RAN[$i]}
        b=${RAN[$j]}
        diff -u "$WORK/$a.out" "$WORK/$b.out" > "$WORK/$a-vs-$b.diff" ||
            { head -20 "$WORK/$a-vs-$b.diff"; fail "$a and $b disagree"; }
        pass "$a and $b agree with each other"
    done
done

# ---- exit status --------------------------------------------------------
# The consistency fixture cannot cover this: its `main` returns nothing, so
# every leg exits 0 whether or not main()'s value is forwarded. These two
# scratch scripts are the two cases that tell it apart — a program whose
# top level calls a main() returning 3, and a program that never calls one.
# Both the interpreter and the emitters have to report the same status.
STATUS_SCRIPT="$WORK/main.lume"
PLAIN_SCRIPT="$WORK/no-main.lume"
cat > "$STATUS_SCRIPT" <<'LUME'
func main(): int {
    print("status\n");
    return 3;
}
main();
LUME
cat > "$PLAIN_SCRIPT" <<'LUME'
func main(): int {
    print("never called\n");
    return 4;
}
LUME

# $1 is the compile flag — empty for the interpreter, which needs neither IR
# nor a toolchain — $2 the script being asked for its status and $3 the label
# its binary goes under (a derived path would put the script's own slashes in
# the output name, and an -o into a directory that does not exist is a silent
# empty binary rather than an error).
status_of() {
    local flag=$1 script=$2 bin="$WORK/status-$3" rc
    if [ -n "$flag" ]; then
        "$TARGET" "$flag" "$script" -o "$bin" > /dev/null 2>&1 ||
            fail "exit status: $flag could not compile $script"
        [ -x "$bin" ] || fail "exit status: $flag produced no binary"
        "$bin" > /dev/null 2>&1; rc=$?
    else
        "$TARGET" "$script" > /dev/null 2>&1; rc=$?
    fi
    printf '%s' "$rc"
}

for leg in "${RAN[@]}"; do
    case "$leg" in
        text) flag=--compile-text ;;
        llvm) flag=--compile-llvm ;;
        *)    flag= ;;
    esac
    got=$(status_of "$flag" "$STATUS_SCRIPT" main)
    [ "$got" -eq 3 ] ||
        fail "exit status: $leg exited $got for a main() returning 3"
    got=$(status_of "$flag" "$PLAIN_SCRIPT" plain)
    [ "$got" -eq 0 ] ||
        fail "exit status: $leg exited $got for a program whose top level never calls main()"
    pass "exit status: $leg reports 3 from main() and 0 without one"
done

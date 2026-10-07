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
TARGET="${LUME_BIN:-bin/lume-core}"
SCRIPT=tests/native-consistency.lume
EXPECTED=tests/native-consistency.expected
WORK=$(mktemp -d)
trap 'rm -rf "$WORK"' EXIT

fail() { echo "FAIL: $*" >&2; exit 1; }
pass() { echo "ok   $*"; }

# The compiler's own diagnostic, with everything else on the same stream
# stripped. `make asan` points TARGET at a sanitized binary, and LSan writes
# its report to stderr -- so a capture of `2>&1` picks the leak report up
# alongside the message, and two backends that word the refusal identically
# still differ byte-for-byte because one of them leaked a different amount.
# That is a real difference in the *sanitizer's* output, not in the wording,
# and it made this file's wording assertion fail on Linux CI while passing
# everywhere else. Only the `lume:` line is the diagnostic.
#
# It takes the already-captured stream on stdin rather than running the
# compiler itself, so the caller keeps the compiler's own exit status (`$?`
# after a pipeline would be grep's) and there is only one place that knows
# the redirection order.
diag_line() { grep -m1 '^lume:'; }

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

# ---- null must not become a silent scalar ---------------------------
# The consistency fixture cannot cover this: it only diffs programs all three
# legs *accept*. `let x: int = null` is accepted by the type checker (every
# type is nullable) and by the interpreter, and it used to compile differently
# on the two emitters -- the text one had no IR edge and refused, the libLLVM
# one had a ptrtoint and printed a confident 0. That is a wrong answer rather
# than a rejected program, which is the worst shape a backend divergence can
# take, so it gets its own assertions.
#
# Asserted per leg rather than by diffing runs, because the point is that the
# program does not run at all. The two emitters must also *word* the refusal
# the same, so a future change that makes only one of them refuse is caught
# here rather than by a user.
NULL_SCALARS="$WORK/null-scalar.lume"
cat > "$NULL_SCALARS" <<'LUME'
let x: int = null;
print(x);
LUME

null_msg=""
for leg in "${RAN[@]}"; do
    case "$leg" in
        text) flag=--compile-text ;;
        llvm) flag=--compile-llvm ;;
        *)    continue ;;   # the interpreter accepts this one; nothing to assert
    esac
    msg=$("$TARGET" "$flag" "$NULL_SCALARS" -o "$WORK/null-$leg" 2>&1 >/dev/null)
    rc=$?
    msg=$(printf '%s\n' "$msg" | diag_line)
    [ $rc -ne 0 ] ||
        fail "null-as-scalar: $leg compiled \`let x: int = null\` (it must refuse)"
    case "$msg" in
        *"cannot use 'null' as a int value"*) ;;
        *) echo "$msg" | head -3
           fail "null-as-scalar: $leg refused with an unexpected message" ;;
    esac
    # First leg to get here sets the wording the other one has to match.
    if [ -z "$null_msg" ]; then
        null_msg=$msg
    elif [ "$msg" != "$null_msg" ]; then
        fail "null-as-scalar: text and llvm word the refusal differently"
    fi
done
[ -n "$null_msg" ] ||
    echo "ok   null-as-scalar: no emitter leg ran, nothing to assert"
[ -z "$null_msg" ] ||
    pass "null-as-scalar: every emitter refuses it, with the same message"

# A string annotation is the one null that stays legal natively (it travels as
# the same opaque pointer), and it has to *print* like the interpreter prints
# it. This one is about the emitted code, not the refusal, so it runs:
# rt.c's lume_print_str used to hand the null straight to snprintf's "%s",
# which is undefined behaviour -- glibc printed "(null)" and nothing promised
# that. Both emitters have to agree with the interpreter here.
NULL_STR="$WORK/null-str.lume"
cat > "$NULL_STR" <<'LUME'
let s: string = null;
print(s);
LUME
for leg in "${RAN[@]}"; do
    case "$leg" in
        text) flag=--compile-text ;;
        llvm) flag=--compile-llvm ;;
        *)    continue ;;
    esac
    "$TARGET" "$flag" "$NULL_STR" -o "$WORK/nullstr-$leg" >/dev/null 2>&1 ||
        fail "null-as-string: $leg refused a legal \`let s: string = null\`"
    got=$("$WORK/nullstr-$leg" 2>&1 | head -1)
    [ "$got" = "null" ] ||
        fail "null-as-string: $leg printed '$got', expected 'null'"
done
pass "null-as-string: every leg prints the bare word null"

# ---- `?` error propagation must agree across the backends ----------------
# This assertion used to require the opposite. `?` was unimplemented in both
# emitters -- propagate had no handling at all -- so it degraded into a plain
# call: the interpreter unwraps `ok` and returns the enclosing function early
# on `err`, while a compiled `?` printed the whole Result instead of the
# payload and, on the error path, ran the very statements the error was meant
# to skip. Refusing to compile was the only honest answer then, and this check
# pinned that.
#
# `?` is implemented now (the payload type comes from the callee's own
# `return { ok: X }` statements, recorded by the type checker's first pass), so
# the requirement flipped: every leg has to *run* it and produce what the
# interpreter produces. Both paths are checked, because they fail differently:
# the ok path used to print the whole Result, the err path used to run the
# skipped statements and then report success.
PROPAGATE="$WORK/propagate.lume"
cat > "$PROPAGATE" <<'LUME'
func divide(a: int, b: int): Result {
  if (b == 0) { return { err: "division by zero" }; }
  return { ok: a };
}
func ok_path(): Result {
  let v = divide(21, 7)?;
  print(v);
  return { ok: 0 };
}
func err_path(): Result {
  let v = divide(21, 0)?;
  print("must not be reached");
  return { ok: 0 };
}
print(ok_path());
print(err_path());
LUME
prop_want=$("$TARGET" "$PROPAGATE" 2>/dev/null)
[ "$prop_want" = "21
{\"ok\":0}
{\"err\":\"division by zero\"}" ] ||
    fail "propagate: the interpreter printed unexpected output:$(printf ' %s' "$prop_want")"
for leg in "${RAN[@]}"; do
    case "$leg" in
        text) flag=--compile-text ;;
        llvm) flag=--compile-llvm ;;
        *)    continue ;;
    esac
    "$TARGET" "$flag" "$PROPAGATE" -o "$WORK/prop-$leg" >/dev/null 2>&1 ||
        fail "propagate: $leg refused to compile a legal \`f()?\`"
    got=$("$WORK/prop-$leg" 2>/dev/null)
    [ "$got" = "$prop_want" ] || {
        echo "got: $got"
        fail "propagate: $leg disagrees with the interpreter on \`f()?\`"
    }
done
pass "propagate: every leg unwraps \`?\` and propagates \`err\` like the interpreter"

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
    local flag=$1 label=$2 script=${3:-$SCRIPT} expected=${4:-$EXPECTED}
    local bin="$WORK/$2" err rc
    if [ -n "$flag" ]; then
        err=$("$TARGET" "$flag" "$script" -o "$bin" 2>&1 >/dev/null); rc=$?
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
        "$TARGET" "$script" > "$WORK/$label.out" 2> "$WORK/$label.err"
        err=$(grep -v "script completed without run" "$WORK/$label.err" 2>/dev/null)
        if [ -n "$err" ]; then
            echo "$err" | head -5
            fail "$label: interpreter wrote to stderr"
        fi
    fi
    # Windows builds emit CRLF from the CRT's text-mode stdout, while the
    # committed baseline is LF, so normalize before diffing — the leg itself
    # is line-ending agnostic this way, and the step is a no-op on Linux.
    # The cross-leg comparison below uses the same normalized files, so a
    # mixed-mode pair cannot false-fail either.
    tr -d '\r' < "$WORK/$label.out" > "$WORK/$label.norm"
    if ! diff -u "$expected" "$WORK/$label.norm" > "$WORK/$label.diff"; then
        head -20 "$WORK/$label.diff"
        fail "$label: output differs from $expected"
    fi
    pass "$label output matches $expected"
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
        diff -u "$WORK/$a.norm" "$WORK/$b.norm" > "$WORK/$a-vs-$b.diff" ||
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

# A closure variable's type locks to the FIRST lambda's signature, every call
# lowers against it, and the runtime box holds whatever was assigned last
# (SPEC 8.1 #9). Reassigning a lambda of a DIFFERENT signature used to be a
# silent pass on the interpreter and a refusal on the native backends; the
# checker now refuses it at the assignment itself so all three legs agree.
CR="$WORK/closure-reassign.lume"
cat > "$CR" <<'LUME'
let f = (x: int) => x + 1;
f = (x: string) => x;
print(f(3));
LUME
cr_msg=""
for leg in "${RAN[@]}"; do
    case "$leg" in
        interp) msg=$("$TARGET" "$CR" 2>&1 >/dev/null); rc=$? ;;
        text)   flag=--compile-text ;;
        llvm)   flag=--compile-llvm ;;
        *)      continue ;;
    esac
    if [ "$leg" != "interp" ]; then
        err=$("$TARGET" $flag "$CR" -o "$WORK/cr-$leg" 2>&1 >/dev/null); rc=$?
        msg=$(printf '%s\n' "$err" | diag_line)
    fi
    [ $rc -ne 0 ] ||
        fail "closure-reassign: $leg accepted a lambda of a different signature"
    case "$msg" in
        *"assignment type mismatch for 'f'"*) ;;
        *) echo "$msg" | head -3
           fail "closure-reassign: $leg refused with an unexpected message" ;;
    esac
    if [ -z "$cr_msg" ]; then
        cr_msg=$msg
    elif [ "$msg" != "$cr_msg" ]; then
        fail "closure-reassign: legs word the refusal differently"
    fi
done
[ -n "$cr_msg" ] ||
    echo "ok   closure-reassign: no leg ran, nothing to assert"
[ -z "$cr_msg" ] ||
    pass "closure-reassign: every leg refuses a lambda of a different signature, with the same message"

# The OTHER half of SPEC 8.1 #9: a lambda of the SAME signature stays legal —
# the runtime box is re-bound while the type keeps the first signature. Small
# fixture on purpose: native compiles of large scripts are a separate known
# flake (see TODO.md), and this check is about closure-reassign, not about
# scale.
CRO="$WORK/closure-reassign-ok.lume"
cat > "$CRO" <<'LUME'
let g = (x) => x + 1;
g = (x) => x * 10;
print(map(g, [1, 2]));
print(g(5));
LUME
cro_msg=""
for leg in "${RAN[@]}"; do
    case "$leg" in
        interp) "$TARGET" "$CRO" > "$WORK/cro-$leg.out" 2>/dev/null; rc=$? ;;
        text)   err=$("$TARGET" --compile-text "$CRO" -o "$WORK/cro-$leg" 2>&1 >/dev/null); rc=$? ;;
        llvm)   err=$("$TARGET" --compile-llvm "$CRO" -o "$WORK/cro-$leg" 2>&1 >/dev/null); rc=$? ;;
        *)      continue ;;
    esac
    [ $rc -eq 0 ] ||
        fail "closure-reassign-ok: $leg refused a same-signature reassignment (rc=$rc)"
    if [ "$leg" = "interp" ]; then
        cro_out=$(cat "$WORK/cro-$leg.out")
    else
        [ -x "$WORK/cro-$leg" ] || fail "closure-reassign-ok: $leg produced no binary"
        cro_out=$("$WORK/cro-$leg" 2>&1)
    fi
    # Same CRLF normalization as check(): the interpreter's CRT text-mode
    # stdout ends lines with \r\n while the compiled binaries emit LF.
    cro_out=$(printf '%s' "$cro_out" | tr -d '\r')
    if [ -z "$cro_msg" ]; then
        cro_msg=$cro_out
    elif [ "$cro_msg" != "$cro_out" ]; then
        fail "closure-reassign-ok: legs disagree ($leg: $(printf '%s' "$cro_out"))"
    fi
done
[ -n "$cro_msg" ] ||
    echo "ok   closure-reassign-ok: no leg ran, nothing to assert"
[ -z "$cro_msg" ] ||
    pass "closure-reassign-ok: same-signature reassignment agrees on every leg"

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
prop_want=$("$TARGET" "$PROPAGATE" 2>/dev/null | tr -d '\r')
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
    got=$("$WORK/prop-$leg" 2>/dev/null | tr -d '\r')
    [ "$got" = "$prop_want" ] || {
        echo "got: $got"
        fail "propagate: $leg disagrees with the interpreter on \`f()?\`"
    }
done
pass "propagate: every leg unwraps \`?\` and propagates \`err\` like the interpreter"

# ---- ns module chain: entry → A → B (member call + member variable read) ----
# A module importing another module: A's own alias for B (`b`) must register
# B's functions and top-level bindings under "<ns>.<name>", not only the
# entry's aliases. Before the multi-alias fix both native backends refused
# `b.double` with "call to unknown function"; the interpreter and --check
# always accepted the chain, so a native-only divergence never surfaced in
# the single-file fixture. 5 * 2 + 7 = 17 on every leg.
NS_CHAIN=tests/ns-chain-main.lume
ns_want=$("$TARGET" "$NS_CHAIN" 2>/dev/null | tr -d '\r')
ns_ran=()
# Walk the legs that actually built (RAN), not a hardcoded list: on a runner
# built without libLLVM the llvm leg is unavailable and must be skipped, not
# failed — the same skip logic the propagate block already uses.
for leg in "${RAN[@]}"; do
    case "$leg" in
        text) flag=--compile-text ;;
        llvm) flag=--compile-llvm ;;
        *)    continue ;;
    esac
    "$TARGET" "$flag" "$NS_CHAIN" -o "$WORK/ns-chain-$leg" >/dev/null 2>&1 ||
        fail "ns-chain: $leg refused to compile a legal module chain (entry → A → B)"
    got=$("$WORK/ns-chain-$leg" 2>/dev/null | tr -d '\r')
    [ "$got" = "$ns_want" ] || {
        echo "got: $got"
        fail "ns-chain: $leg disagrees with the interpreter on the module chain"
    }
    ns_ran+=("$leg")
done
pass "ns-chain: entry → A → B member call / member read agrees with the interpreter on every leg"

# ---- sql cross-backend consistency (needs a HAVE_SQLITE build) -----------
# The sql builtins are the P6 surface: the native runtime and both emitters
# were added together, so they get the same three-way treatment as the main
# fixture — interpreter as reference, every available emitter compiled and
# diffed, and every pair cross-diffed. On a build without libsqlite3 the
# interpreter refuses sql_* ("unavailable") and the emitters refuse to emit,
# so a single probe decides the whole block.
#
# The fixture takes its database from SQLITE_DB (a bare "" falls back to it
# on all three backends), so the block exports one scratch DB per run and the
# expected file stays machine-independent.
SQL_SCRIPT=tests/native-sql-consistency.lume
SQL_EXPECTED=tests/native-sql-consistency.expected
[ -f "$SQL_EXPECTED" ] || fail "$SQL_EXPECTED missing (generate from the interpreter on a sqlite build)"

# Probe with a tx handle: tx_begin opens READWRITE|CREATE, so the probe both
# creates its scratch DB and would fail loudly on a build without sqlite.
SQL_PROBE="$WORK/sql-probe.lume"
cat > "$SQL_PROBE" <<'LUME'
let p: string = sql_tx_begin("");
sql_tx_commit(p);
LUME

sql_ran=()
if SQLITE_DB="$WORK/probe.db" "$TARGET" "$SQL_PROBE" >/dev/null 2>&1; then
    export SQLITE_DB="$WORK/ns-consistency.db"
    rm -f "$SQLITE_DB"
    check "" sql-interp "$SQL_SCRIPT" "$SQL_EXPECTED"
    sql_ran+=("sql-interp")
    for leg in "${RAN[@]}"; do
        case "$leg" in
            text) check --compile-text sql-text "$SQL_SCRIPT" "$SQL_EXPECTED"; sql_ran+=("sql-text") ;;
            llvm) check --compile-llvm sql-llvm "$SQL_SCRIPT" "$SQL_EXPECTED"; sql_ran+=("sql-llvm") ;;
        esac
    done
    for (( i = 0; i < ${#sql_ran[@]}; i++ )); do
        for (( j = i + 1; j < ${#sql_ran[@]}; j++ )); do
            a=${sql_ran[$i]}; b=${sql_ran[$j]}
            diff -u "$WORK/$a.norm" "$WORK/$b.norm" > "$WORK/sql-$a-vs-$b.diff" ||
                { head -20 "$WORK/sql-$a-vs-$b.diff"; fail "sql: $a and $b disagree"; }
            pass "sql: $a and $b agree with each other"
        done
    done
else
    echo "ok   sql cross-backend: skipped (build has no libsqlite3)"
fi

# ---- native serve: abandoned sql_tx handle reclaimed at request boundary ----
# The interpreter's tx handle is an OBJ_SQLTX with a GC finalizer; the native
# handle is a plain heap struct, so the serve loop rolls back + closes every
# still-open handle when a request ends (bridge_native.c
# lume_bi_sql_tx_cleanup). Without that reclaim, the first /open below leaves
# its transaction (and the BEGIN lock) open and the second /open's
# sql_tx_begin would time out on SQLITE_BUSY. Two sequential 200s with a
# fresh "ok" body prove the reclaim. Runs only when the sql probe passed
# (HAVE_SQLITE build); the port is fixed — the CI jobs that reach this leg
# run the suites serially on one runner.
if [ -n "${sql_ran[0]:-}" ]; then
    TX_SERVE_SRC=tests/native-sql-tx-serve.lume
    TX_SERVE_BIN="$WORK/tx-serve"
    if "$TARGET" --compile-text "$TX_SERVE_SRC" -o "$TX_SERVE_BIN" \
            >"$WORK/tx-serve-build.log" 2>&1; then
        rm -f /tmp/lume-tx-cleanup.db
        "$TX_SERVE_BIN" >"$WORK/tx-serve.log" 2>&1 &
        TX_PID=$!
        sleep 1
        r1=$(curl -s -o "$WORK/tx-r1.txt" -w '%{http_code}' \
             "http://127.0.0.1:18091/open" 2>/dev/null)
        r2=$(curl -s -o "$WORK/tx-r2.txt" -w '%{http_code}' \
             "http://127.0.0.1:18091/open" 2>/dev/null)
        kill "$TX_PID" 2>/dev/null
        wait "$TX_PID" 2>/dev/null
        if [ "$r1" = "200" ] && [ "$r2" = "200" ] \
           && grep -q "ok" "$WORK/tx-r1.txt" \
           && grep -q "ok" "$WORK/tx-r2.txt"; then
            pass "sql serve: abandoned tx handle reclaimed at request boundary"
        else
            head -20 "$WORK/tx-serve.log"
            fail "sql serve: tx handle reclaim (r1=$r1 r2=$r2)"
        fi
    else
        head -20 "$WORK/tx-serve-build.log"
        fail "sql serve: cannot compile $TX_SERVE_SRC"
    fi
fi

# ---- statistical builtins cross-backend consistency (P4 native gap) -------
# The interpreter always had tdist_cdf / normcdf / chisq_cdf / erf / gamma /
# lgamma / igamma / ibeta; the native emitters had the Bi table and rt.c
# helpers but NO inference entry, so `let p = tdist_cdf(...)` compiled only
# with an explicit `: float` annotation — the P4 counter-example that taught
# the LLM to annotate. Inference + the libLLVM emit leg were added together;
# this block pins all three legs on the unannotated form. The interpreter
# output is the reference; every available emitter must compile, run and
# match it (same shape as the propagate block above).
STATS_SCRIPT="$WORK/stats.lume"
cat > "$STATS_SCRIPT" <<'LUME'
let a = normcdf(1.5);
let b = erf(0.7);
let c = gamma(5.0);
let d = lgamma(10.0);
let e = igamma(2.0, 3.0);
let f = ibeta(2.0, 3.0, 0.5);
let g = tdist_cdf(1.1, 10.0);
let h = chisq_cdf(4.0, 5.0);
print(a);
print(b);
print(c);
print(d);
print(e);
print(f);
print(g);
print(h);
LUME
stats_want=$("$TARGET" "$STATS_SCRIPT" 2>/dev/null | tr -d '\r')
[ -n "$stats_want" ] ||
    fail "stats: the interpreter produced no output for the fixture"
stats_ran=()
for leg in "${RAN[@]}"; do
    case "$leg" in
        text) flag=--compile-text ;;
        llvm) flag=--compile-llvm ;;
        *)    continue ;;
    esac
    "$TARGET" "$flag" "$STATS_SCRIPT" -o "$WORK/stats-$leg" >/dev/null 2>&1 ||
        fail "stats: $leg refused the unannotated statistical builtins (inference gap)"
    got=$("$WORK/stats-$leg" 2>/dev/null | tr -d '\r')
    [ "$got" = "$stats_want" ] || {
        echo "got: $got"
        fail "stats: $leg disagrees with the interpreter on the statistical builtins"
    }
    stats_ran+=("$leg")
done
pass "stats: unannotated statistical builtins agree with the interpreter on every leg"

# ---- str_match cross-backend consistency (dynamic result shape, SPEC 6.2) ----
# str_match rows are anonymous maps boxed as OBJ slots whose shape exists only
# at runtime. They used to be interpreter-only: the native emitters had the
# lume_bi_str_match helper and the slot getters, but NO inference entry for
# `get(rows, 0)` (list index read) nor for `get(r, "text")` off an anon row,
# so any program touching a str_match result was refused at compile time
# (`cannot infer a type for 'r'`) — the last residual of the json(str)-style
# dynamic-shape boundary. Inference (list element read + anon-row read -> any)
# and the any-slot accessor (lume_list_at_slot) closed it; this block pins all
# three legs on the unannotated form, same shape as the stats block above.
SM_SCRIPT="$WORK/str-match.lume"
cat > "$SM_SCRIPT" <<'LUME'
let rows = str_match("(a+)(b+)", "xxaaabbbyy");
print(len(rows));
let r = get(rows, 0);
print(get(r, "text"));
print(get(r, "index"));
let g = get(r, "groups");
print(len(g));
print(get(g, 0));
LUME
sm_want=$("$TARGET" "$SM_SCRIPT" 2>/dev/null | tr -d '\r')
[ -n "$sm_want" ] ||
    fail "str-match: the interpreter produced no output for the fixture"
sm_ran=()
for leg in "${RAN[@]}"; do
    case "$leg" in
        text) flag=--compile-text ;;
        llvm) flag=--compile-llvm ;;
        *)    continue ;;
    esac
    "$TARGET" "$flag" "$SM_SCRIPT" -o "$WORK/str-match-$leg" >/dev/null 2>&1 ||
        fail "str-match: $leg refused the str_match row reads (inference gap)"
    got=$("$WORK/str-match-$leg" 2>/dev/null | tr -d '\r')
    [ "$got" = "$sm_want" ] || {
        echo "got: $got"
        fail "str-match: $leg disagrees with the interpreter on the str_match rows"
    }
    sm_ran+=("$leg")
done
pass "str-match: str_match rows agree with the interpreter on every leg"

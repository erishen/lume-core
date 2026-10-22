#!/usr/bin/env bash
# scripts/ai-verify-loop.sh — scripted entry point of the P4 AI
# self-verification loop (docs/ai-verify.md, "Scripted" wiring).
#
# The gate (verify-ai-lume.sh) judges ONE generated program. This loop
# is the CI/CD-side harness that feeds a *set* of LLM-produced programs
# into the gate and asserts the expected verdicts, then keeps every
# counter-example in a log ready to hand back to the model for retry.
#
#   scripts/ai-verify-loop.sh
#   AI_PASS_DIR=examples/ai-generated AI_REJECT_DIR=tests/ai-bad \
#       AI_REJECTS_LOG=out/ai-rejects.log scripts/ai-verify-loop.sh
#
# Verdict contract (same as the gate):
#   must-pass set  — every program must exit 0 (accepted).
#   must-reject set — every program must exit 1 (rejected with a reason);
#                     exit 0 here is the worst failure: the gate let a
#                     bad program through.
#   exit 2 (gate cannot judge) — infrastructure problem, counted as
#     SKIP, not a program failure; if every program in both sets SKIPs
#     the loop cannot exercise anything and exits 2 itself.
#
# Exit: 0 = all verdict assertions held, 1 = a verdict was violated
# (details appended to $AI_REJECTS_LOG, echoed to stderr — the
# counter-examples to feed the LLM for retry), 2 = the gate could not
# run here (binary missing / toolchain short of two legs everywhere).
#
# Scope guard (TODO P4): this is the machine-verify half. It reads
# files; it does not call an LLM, does not touch the network. The
# LLM → .lume producer stays in the host / an external tool, writing
# candidates into these directories (or any dir passed in).
set -uo pipefail
cd "$(dirname "$0")/.."

TARGET="${LUME_BIN:-bin/lume-core}"
GATE="${AI_VERIFY_SH:-scripts/verify-ai-lume.sh}"
PASS_DIR="${AI_PASS_DIR:-examples/ai-generated}"
REJECT_DIR="${AI_REJECT_DIR:-tests/ai-bad}"
REJECTS_LOG="${AI_REJECTS_LOG:-out/ai-rejects.log}"

[ -x "$TARGET" ] || { echo "FAIL: $TARGET missing — run make first" >&2; exit 2; }
[ -f "$GATE" ]   || { echo "FAIL: $GATE missing" >&2; exit 2; }

# The must-pass / must-reject seeds must exist and be non-empty — an
# empty set is a wiring error, not a pass.
pass_files=("$PASS_DIR"/*.lume)
reject_files=("$REJECT_DIR"/*.lume)
[ "${#pass_files[@]}" -ge 1 ] && [ -f "${pass_files[0]}" ] ||
    { echo "FAIL: no programs in must-pass dir $PASS_DIR" >&2; exit 2; }
[ "${#reject_files[@]}" -ge 1 ] && [ -f "${reject_files[0]}" ] ||
    { echo "FAIL: no programs in must-reject dir $REJECT_DIR" >&2; exit 2; }

mkdir -p "$(dirname "$REJECTS_LOG")"
: >> "$REJECTS_LOG"    # touch / keep history (CI checkouts start clean)
{
    echo "=============================================================="
    echo "== ai-verify-loop $(date -u '+%Y-%m-%d %H:%M:%S UTC')"
} >> "$REJECTS_LOG"

pass_n=0; reject_n=0; skip_n=0; fail_n=0
pass_skip=0; reject_skip=0

# verdict <expect rc|1|2> <label> <file> — run the gate on one file and
# check its exit code against the expectation; log counter-examples.
verdict() {
    local expect=$1 label=$2 file=$3 rc err
    err=$("$GATE" "$file" 2>&1); rc=$?
    if [ "$rc" -ne 0 ] && [ "$rc" -ne 1 ]; then
        skip_n=$((skip_n + 1))
        if [ "$label" = "must-pass" ]; then pass_skip=$((pass_skip + 1)); else reject_skip=$((reject_skip + 1)); fi
        echo "skip $label $(basename "$file") — gate returned rc=$rc (cannot judge)"
        return 0
    fi
    if [ "$rc" -eq "$expect" ]; then
        echo "ok   $label $(basename "$file") (rc=$rc)"
        return 0
    fi
    fail_n=$((fail_n + 1))
    echo "FAIL $label $(basename "$file") — expected rc=$expect, gate said $rc"
    {
        echo "---- counter-example: $file (expected rc=$expect, gate said $rc)"
        echo "$err" | sed 's/^/    /'
    } >> "$REJECTS_LOG"
    return 0
}

for f in "${pass_files[@]}"; do
    pass_n=$((pass_n + 1)); verdict 0 "must-pass" "$f"
done
for f in "${reject_files[@]}"; do
    reject_n=$((reject_n + 1)); verdict 1 "must-reject" "$f"
done

echo "ai-verify-loop: $pass_n must-pass, $reject_n must-reject, $skip_n skipped, $fail_n verdict violations"

if [ "$fail_n" -gt 0 ]; then
    echo "FAIL: $fail_n verdict violation(s) — counter-examples appended to $REJECTS_LOG" >&2
    exit 1
fi
if [ "$pass_skip" -eq "$pass_n" ] || [ "$reject_skip" -eq "$reject_n" ]; then
    echo "FAIL: the gate could not judge every program in at least one set (all skipped) — check the toolchain" >&2
    exit 2
fi
echo "PASS ai-verify-loop — machine gate agrees with every expected verdict"
exit 0

#!/usr/bin/env bash
# scripts/ai-produce.sh — the produce loop of the P4 AI self-verification
# pipeline (TODO P4): take producer output (scripted or, on the host, an
# LLM's tool-dispatch reply), run every candidate through the machine gate
# (scripts/verify-ai-lume.sh), and turn each verdict into an action.
#
#   scripts/ai-produce.sh                 # generate candidates, then gate them
#   AI_CANDIDATES_DIR=out/ai-candidates scripts/ai-produce.sh   # gate existing
#
# Per candidate the manifest (out/ai-candidates/manifest.txt, written by
# scripts/ai-gen-programs.sh; absent for externally dropped candidates,
# which default to accept) carries the producer's intended verdict:
#   accept:1  -> the gate must exit 0 — the candidate is READY to commit
#               into tests/ as a regression seed (make check + fuzz +
#               native-consistency all cover it from then on).
#   reject:0  -> the gate must exit 1 — the candidate is rejected, and the
#               reason is appended to $AI_REJECTS_LOG: the counter-example
#               to log back to the LLM for retry.
# Anything else is a verdict violation (log + exit 1) — a backend drift
# masquerading as a gate pass shows up here, not in a later test run.
#
# Exit: 0 = every candidate judged as the manifest expected, 1 = verdict
# violation(s) (counter-examples appended to $AI_REJECTS_LOG), 2 = the
# gate could not judge anything (binary missing / fewer than two legs).
#
# Scope guard (TODO P4): this is still the machine half — it calls no LLM
# and touches no network. The host's real tool dispatch can replace
# ai-gen-programs.sh; the contract it must honour is the manifest file.
set -uo pipefail
cd "$(dirname "$0")/.."

# Installed mode: resolve lume-core and the gate from PATH / this dir first
# (see verify-ai-lume.sh); repo mode keeps the checkout-relative defaults.
TARGET="${LUME_BIN:-}"
if [ -z "$TARGET" ]; then
    TARGET="$(command -v lume-core 2>/dev/null || echo bin/lume-core)"
fi
GATE="${AI_VERIFY_SH:-}"
if [ -z "$GATE" ]; then
    if [ -x "$(dirname "$0")/verify-ai-lume.sh" ]; then GATE="$(dirname "$0")/verify-ai-lume.sh"; else GATE="$(dirname "$0")/lume-ai-verify"; fi
fi
CAND_DIR="${AI_CANDIDATES_DIR:-out/ai-candidates}"
MANIFEST="$CAND_DIR/manifest.txt"
REJECTS_LOG="${AI_REJECTS_LOG:-out/ai-rejects.log}"

[ -x "$TARGET" ] || { echo "FAIL: $TARGET missing — run make first" >&2; exit 2; }
[ -f "$GATE" ]   || { echo "FAIL: $GATE missing" >&2; exit 2; }

# No manifest yet -> run the scripted producer (host dispatch writes the
# manifest itself; it should call ai-produce.sh with --no-gen in that case
# or simply drop candidates + manifest into $CAND_DIR).
if [ ! -f "$MANIFEST" ]; then
    "${AI_GEN_SH:-$(dirname "$0")/ai-gen-programs.sh}"
fi

# manifest rows: <name>|<expected: accept|reject>|<prompt>. Externally
# dropped candidates without a manifest row default to accept.
declare -A EXPECT
if [ -f "$MANIFEST" ]; then
    while IFS='|' read -r name expect _; do
        [ -n "$name" ] && EXPECT["$name"]="$expect"
    done < "$MANIFEST"
fi

mkdir -p "$(dirname "$REJECTS_LOG")"
: >> "$REJECTS_LOG"    # touch / keep history (CI checkouts start clean)
{
    echo "=============================================================="
    echo "== ai-produce $(date -u '+%Y-%m-%d %H:%M:%S UTC') ($CAND_DIR)"
} >> "$REJECTS_LOG"

ready_n=0; reject_n=0; skip_n=0; viol_n=0
files=("$CAND_DIR"/*.lume)
[ "${#files[@]}" -ge 1 ] && [ -f "${files[0]}" ] ||
    { echo "FAIL: no candidates in $CAND_DIR (run scripts/ai-gen-programs.sh)" >&2; exit 2; }

for f in "${files[@]}"; do
    name=$(basename "$f" .lume)
    err=$("$GATE" "$f" 2>&1); rc=$?
    expect="${EXPECT["$name"]:-accept}"
    if [ "$rc" -ne 0 ] && [ "$rc" -ne 1 ]; then
        skip_n=$((skip_n + 1))
        echo "skip  $name — gate returned rc=$rc (cannot judge)"
        continue
    fi
    case "$expect:$rc" in
        accept:0)
            ready_n=$((ready_n + 1))
            echo "READY $name — accepted, candidate for tests/" ;;
        reject:1)
            reject_n=$((reject_n + 1))
            echo "ok    $name — rejected as expected (counter-example logged)"
            {
                echo "---- counter-example: $f (expected $expect, gate said $rc)"
                echo "$err" | sed 's/^/    /'
            } >> "$REJECTS_LOG" ;;
        *)
            viol_n=$((viol_n + 1))
            echo "VIOLATE $name — expected $expect, gate said rc=$rc"
            {
                echo "---- counter-example: $f (expected $expect, gate said $rc)"
                echo "$err" | sed 's/^/    /'
            } >> "$REJECTS_LOG" ;;
    esac
done

echo "ai-produce: $ready_n ready, $reject_n rejected-as-expected, $skip_n skipped, $viol_n violations"

if [ "$viol_n" -gt 0 ]; then
    echo "FAIL: $viol_n verdict violation(s) — counter-examples appended to $REJECTS_LOG" >&2
    exit 1
fi
if [ "$ready_n" -eq 0 ] && [ "$reject_n" -eq 0 ]; then
    echo "FAIL: the gate could not judge any candidate (all skipped) — check the toolchain" >&2
    exit 2
fi
echo "PASS ai-produce — every candidate judged as the manifest intended; rejects are the LLM retry payload"
exit 0

#!/usr/bin/env bash
#
# Guard against the two native emitters drifting apart.
#
#   src/codegen*.c       text emitter   (LUME -> LLVM IR text -> cc -> binary)
#   src/llvm_codegen.c   libLLVM emitter (LUME -> LLVM IR -> libLLVM -> binary)
#
# Both sides dispatch on the AST with `switch (n->type) { case N_...: }`.
# A node kind handled in only one emitter is a silent divergence: the other
# backend falls through to `default:` and emits either broken IR or a runtime
# trip. This script diffs the two sides' case labels so a new node type has to
# be implemented twice in the same change.
#
# The text side is cut into one tu per section (see the header of src/codegen.c),
# so they are concatenated here: a label that lives in codegen_stmt.c is still a
# hit, not a divergence.
#
# Nodes that are deliberately absent from one side go in KNOWN_DIFF below.

set -u
cd "$(dirname "$0")/.." || exit 1

LEFT='src/codegen*.c'
LEFT_TEXT="$(cat src/codegen*.c)"
RIGHT=src/llvm_codegen.c
RIGHT_TEXT="$(cat "$RIGHT")"

# Lines that may legitimately differ (one emitter handles a node the other
# does not, or vice versa). Each entry keeps its comment.
KNOWN_DIFF=""

fail() {
    printf '%s\n' "FAIL backend parity: $*" >&2
    exit 1
}

# `node_kinds < stream>` -> sorted unique N_* labels handled by that stream.
node_kinds() {
    grep -oE 'case N_[A-Za-z_]+:' |
        sed -E 's/^case (N_[A-Za-z_]+):$/\1/' |
        sort -u
}

left=$(node_kinds <<<"$LEFT_TEXT")
right=$(node_kinds <<<"$RIGHT_TEXT")

[ -n "$left" ]  || fail "no AST case labels found in $LEFT (did the source layout change?)"
[ -n "$right" ] || fail "no AST case labels found in $RIGHT (did the source layout change?)"

# A silent regex that matches nothing is the failure mode this guard exists to
# catch, so make sure both sides really matched a whole dispatch table.
only_left=$(comm -23 <(printf '%s\n' "$left")  <(printf '%s\n' "$right"))
only_right=$(comm -13 <(printf '%s\n' "$left") <(printf '%s\n' "$right"))

left_n=$(printf '%s\n' "$left"  | grep -c .)
right_n=$(printf '%s\n' "$right" | grep -c .)

printf 'ok    %s (%s AST node kinds)\n' "$LEFT" "$left_n"
printf 'ok    %s (%s AST node kinds)\n' "$RIGHT" "$right_n"

if [ "$left_n" -eq 0 ] || [ "$right_n" -eq 0 ]; then
    fail "one emitter matched no cases - check the grep pattern above"
fi

while read -r kind; do
    [ -n "$kind" ] || continue
    case " $KNOWN_DIFF " in
        *" $kind "*) continue ;;
    esac
    fail "$kind handled by $LEFT but not $RIGHT"
done <<< "$only_left"

while read -r kind; do
    [ -n "$kind" ] || continue
    case " $KNOWN_DIFF " in
        *" $kind "*) continue ;;
    esac
    fail "$kind handled by $RIGHT but not $LEFT"
done <<< "$only_right"

printf 'ok    both emitters cover the same AST node kinds\n'
exit 0

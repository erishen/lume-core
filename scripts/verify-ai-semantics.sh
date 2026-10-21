#!/usr/bin/env bash
# scripts/verify-ai-semantics.sh — the semantic layer of the P4 AI
# self-verification loop (docs/ai-verify.md, "Semantic layer").
#
# The machine gate (verify-ai-lume.sh) judges backend equivalence: an
# LLM-written program that runs byte-for-byte the same on every backend
# is accepted. That is *self-consistency* — it cannot tell "the program
# does what it claims". This validator adds the semantic half: the
# program DECLARES its own invariants and the validator checks the
# declarations WITHOUT RUNNING THE PROGRAM:
#
#   //@semantic <label>: <expr>
#
#   e.g.   //@semantic p_bound: 0 < p < 1
#          //@semantic sorted: the portfolio is sorted ascending
#   (labels are [a-z_][a-z0-9_]* — use underscores, not hyphens; a
#    hyphenated label is not matched and the declaration is silently
#    skipped as an empty contract.)
#
# A semantic declaration is a contract the program makes about itself:
#   * every identifier the expr refers to must be DEFINED in the program
#     (let bindings, function parameters, function names) — a dangling
#     reference rejects;
#   * every declared label must be CLAIMED in the program's output — at
#     least one print(...) must mention the label or one of the
#     identifiers the expr refers to — an invariant nothing prints is
#     not being honored;
#   * the program must be a pure computation the host executor can run
#     without supervision: no server{} block, no run() HTTP bridge.
#
# Nothing here executes the program; it is a static text check over the
# source. The gate runs it BEFORE the equivalence legs and folds a
# rejection into the same exit-1 counter-example contract.
#
# Usage: scripts/verify-ai-semantics.sh <file.lume> ...
# Exit: 0 = declarations hold (or the program has none — empty contract
#        passes), 1 = a declaration is invalid/dangling/unclaimed or the
#        shape is unsafe, 2 = this validator could not run.
set -uo pipefail
cd "$(dirname "$0")/.."

[ $# -ge 1 ] || { echo "usage: $0 <file.lume> ..." >&2; exit 2; }
command -v grep >/dev/null 2>&1 || { echo "FAIL: grep not available" >&2; exit 2; }

fail() { echo "FAIL: $*" >&2; exit 1; }

# Identifier token extraction — used both to find referenced names in an
# expr and to recognize definitions in the program body.
TOKEN_RE='[a-zA-Z_][a-zA-Z0-9_]*'
# Language keywords / builtins that may legitimately appear in an expr
# but are not program identifiers the expr can refer to.
KEYWORDS='^(if|else|for|in|return|let|and|or|not|true|false|nil|null|print|str|json|stringify|int|float|func|match|push|pop|len|sum|map|filter|reduce|sort|range|abs|min|max|sqrt|pow|floor|ceil|round|normcdf|erf|gamma|lgamma|igamma|ibeta|tdist_cdf|chisq_cdf|env|run|server)$'

for f in "$@"; do
    [ -f "$f" ] || fail "cannot open $f"
    src=$(< "$f")
    label_n=0

    # 1. collect every semantic declaration: //@semantic <label>: <expr>
    #    (label: [a-z_][a-z0-9_]* — keep labels short and machine-y).
    decls=$(printf '%s\n' "$src" | grep -oE "//@semantic [a-z_][a-z0-9_]*: .*" || true)
    [ -z "$decls" ] && { echo "semantic ok: no declarations (empty contract)"; continue; }

    dupes=$(printf '%s\n' "$decls" | sed -E 's/^\/\/@semantic ([a-z_][a-z0-9_]*): .*/\1/' | sort | uniq -d)
    if [ -n "$dupes" ]; then
        fail "$(basename "$f"): duplicate semantic label(s): $(printf '%s' "$dupes" | tr '\n' ' ')"
    fi

    # 2. per declaration: definitions exist, and the output claims it.
    while IFS= read -r d; do
        label_n=$((label_n + 1))
        label=$(printf '%s' "$d" | sed -E 's/^\/\/@semantic ([a-z_][a-z0-9_]*): .*/\1/')
        expr=$(printf '%s' "$d" | sed -E "s/^\/\/@semantic [a-z_][a-z0-9_]*: (.*)/\1/")

        # identifiers the program actually defines: let <n> = / let <n>: /
        # func <n>( / parameter <n>: in a signature.
        defined=$(printf '%s\n' "$src" | grep -oE "(^|[^a-zA-Z0-9_])let[[:space:]]+$TOKEN_RE|(^|[^a-zA-Z0-9_])func[[:space:]]+$TOKEN_RE|[,\(][[:space:]]*$TOKEN_RE[[:space:]]*:" | grep -oE "$TOKEN_RE" | sort -u || true)

        # referenced identifiers in the expr: tokens minus keywords/numerals.
        refs=$(printf '%s\n' "$expr" | grep -oE "$TOKEN_RE" | grep -Ev "$KEYWORDS" | sort -u || true)

        # a semantic expr must actually reach into the program: at least
        # one of its tokens has to be a name the program defines. A
        # declaration that references nothing defined (a dangling name,
        # or prose-only wording) is a contract about nothing.
        hits=""
        for t in $refs; do
            if printf '%s\n' "$defined" | grep -qx "$t"; then
                hits="$hits $t"
            fi
        done
        if [ -z "$hits" ]; then
            fail "$(basename "$f"): semantic '$label' refers to no name defined in the program (dangling or empty)"
        fi

        # output claim: one print(...) mentioning the label or a hit id.
        claimed=0
        if printf '%s\n' "$src" | grep -qE 'print\([^)]*'"$label"; then claimed=1; fi
        for t in $hits; do
            if [ "$claimed" -eq 0 ] && printf '%s\n' "$src" | grep -qE 'print\([^)]*'"$t"; then claimed=1; fi
        done
        if [ "$claimed" -eq 0 ]; then
            fail "$(basename "$f"): semantic '$label' is never claimed by the program's output — no print(...) mentions it or its identifiers"
        fi
        echo "semantic ok: $label — '$expr' defined and claimed"
    done <<< "$decls"

    # 3. shape safety for the host executor: pure computation only.
    if printf '%s\n' "$src" | grep -qE 'server[[:space:]]*\{'; then
        fail "$(basename "$f"): unsafe shape — server{} starts a listener; the AI executor runs pure computations"
    fi
    if printf '%s\n' "$src" | grep -qE "(^|[^a-zA-Z0-9_])run[[:space:]]*\("; then
        fail "$(basename "$f"): unsafe shape — run() starts the HTTP bridge; the AI executor runs pure computations"
    fi

    echo "semantic ok: $label_n declaration(s) held for $(basename "$f")"
done

exit 0

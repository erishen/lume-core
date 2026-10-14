#!/bin/sh
# Case-parity tripwire for the two native emitters' private infer_node_type
# copies (and their str() branches).
#
# The two emitters are separate translation units, each with its own CG
# struct, so each carries a private copy of infer_node_type. History shows
# what drift costs: the struct-update round had to add N_STRUCT_UPDATE to
# both copies (one backend refused what the other accepted), and str() needed
# TY_STRING in the LLVM copy only -- the text backend already had it. A node
# or type kind handled in one copy but not the other is exactly the "one
# backend refuses what the other accepts" failure, and nothing else in the
# suite catches it until some script trips over it at runtime.
#
# This check diffs the case-label SETS of the two copies and fails on any
# asymmetric label. It cannot catch semantic drift *within* a shared label --
# that is what tests/native-consistency.lume is for (runtime three-way
# parity); this catches the missing-case class statically.
#
# The LLVM side is split into .inc fragments (llvm_codegen.c textually
# includes llvm_cg_expr/lit/call/stmt/scan.inc), so the extraction paths
# below point at the fragment that holds each function: infer_node_type
# lives in llvm_cg_scan.inc, the str() builtin branch in llvm_cg_call.inc,
# and the container helper tables in llvm_cg_lit.inc.

set -u
cd "$(dirname "$0")/.." || exit 1

fail=0

# body FILE DEF-PATTERN -- print the function body from its definition line
# to the first column-0 closing brace.
body() {
    start=$(grep -n "$2" "$1" | head -1 | cut -d: -f1)
    if [ -z "$start" ]; then
        echo "case-parity: definition not found in $1: $2" >&2
        exit 1
    fi
    sed -n "${start},/^\}$/p" "$1"
}

# labels TEXT -- the node/literal/type kinds a function body knows about.
# OP_* is excluded on purpose: the copies dispatch operators differently
# (codegen_scan.c uses an inner `case OP_...` switch, llvm_cg_scan.inc an if
# chain), and that is style, not missing-case drift.
labels() {
    printf '%s\n' "$1" | grep -oE 'case (N_|TY_|LIT_)[A-Z_0-9]+' | sort -u
}

# compare NAME LEFT RIGHT -- symmetric difference of two label sets.
compare() {
    name=$1
    d=$(diff <(printf '%s\n' "$2") <(printf '%s\n' "$3"))
    if [ -n "$d" ]; then
        echo "FAIL case-parity $name: the two emitters' copies disagree:"
        echo "$d" | sed 's/^/    /'
        echo "    (< is codegen_scan.c / codegen_expr.c, > is llvm_cg_scan.inc / llvm_cg_call.inc / llvm_cg_lit.inc)"
        echo "    a label known to one copy but not the other is the"
        echo "    missing-case drift this tripwire exists for -- add the"
        echo "    case to both or document why they must differ."
        fail=1
    else
        echo "ok   case-parity $name"
    fi
}

# ---- infer_node_type: both emitters' private copies -----------------------
# The `$` anchor skips the forward declaration (`...Node *n);`), which would
# otherwise be picked first and slice out the wrong region.
scan_infer=$(body src/codegen_scan.c 'infer_node_type(CG \*g, Node \*n)$')
llvm_infer=$(body src/llvm_cg_scan.inc 'static Type \*infer_node_type(CG \*g, Node \*n)$')
compare "infer_node_type case labels" \
        "$(labels "$scan_infer")" "$(labels "$llvm_infer")"

# ---- str() operand switch: both emitters' builtin-call branches -----------
# The branch starts at `if (strcmp(bname, "str") == 0) {` and ends where the
# int/float sibling begins.
str_branch() {
    awk '/if \(strcmp\(bname, "str"\) == 0\) \{/ { f = 1 }
         f { print }
         f && /\} else if \(strcmp\(bname, "int"\)/ { exit }' "$1"
}
compare "str() branch TY labels" \
        "$(labels "$(str_branch src/codegen_expr.c)")" \
        "$(labels "$(str_branch src/llvm_cg_call.inc)")"

# ---- container helper tables: list_push_fn / map_put_fn / map_get_fn ------
# Same dual-copy class as infer_node_type: each emitter picks the runtime
# helper by static type from its own private table. History: TY_BOOL used to
# share TY_INT's INT-tagged helper, which rendered container bools as 1/0;
# giving BOOL its own tag meant editing all four tables in lockstep -- exactly
# the drift this section now guards. The str() TY_BOOL note applies here too:
# map_get_fn still routes BOOL to get_i on purpose (reads come back as 0/1
# i64; the tag lives on the write side only), so a get-side split must happen
# in both copies at once or the tripwire fires.
for tbl in list_push_fn map_put_fn map_get_fn; do
    compare "$tbl TY labels" \
            "$(labels "$(body src/codegen_expr.c "$tbl(Type \\*ty)\$")")" \
            "$(labels "$(body src/llvm_cg_lit.inc "$tbl(Type \\*ty)\$")")"
done

exit $fail

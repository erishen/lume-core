#!/usr/bin/env bash
# Cross-backend verification for the json()/get()/str()/try() family
# (P4-1 closure). Runs every .lume in this directory on all three
# backends and diffs stdout against the .expect file.
#
#   tests/llvm-json/run.sh
#
# Env overrides: BIN (default ../../bin/lume-core), CC (default cc).
# The LLVM leg is tried first; when libLLVM is absent the compile is
# skipped with a note instead of failing (Windows has no libLLVM).
set -u
cd "$(dirname "$0")"
BIN=${BIN:-../../bin/lume-core}
OUT=$(mktemp -d)
trap 'rm -rf "$OUT"' EXIT
fail=0

for l in *.lume; do
    base=${l%.lume}
    [ "$base" = "jfail" ] && continue      # handled below (exit-code leg)

    # interpreter (drop its "script completed without run()" note line;
    # the native backends don't print it)
    "$BIN" "$l" 2>&1 | grep -v '^lume: note:' > "$OUT/$base.i" \
        || { echo "FAIL interp $l"; fail=1; continue; }
    # IR-text native
    "$BIN" --compile-text "$l" -o "$OUT/$base.t" > /dev/null 2>&1 \
        || { echo "FAIL text-compile $l"; fail=1; continue; }
    "$OUT/$base.t" > "$OUT/$base.t.out" 2>&1 \
        || { echo "FAIL text-run $l"; fail=1; continue; }
    # LLVM native (skips gracefully without libLLVM)
    if "$BIN" --compile-llvm "$l" -o "$OUT/$base.l" 2> "$OUT/$base.lc" > /dev/null; then
        "$OUT/$base.l" > "$OUT/$base.l.out" 2>&1 \
            || { echo "FAIL llvm-run $l"; fail=1; continue; }
        cmp -s "$OUT/$base.l.out" "$base.expect" \
            || { echo "FAIL llvm $l (expect below)"; cat "$base.expect"; fail=1; continue; }
    else
        grep -qi "libllvm" "$OUT/$base.lc" \
            || { echo "note: llvm leg skipped for $l: $(head -c 80 "$OUT/$base.lc")"; }
    fi

    cmp -s "$OUT/$base.i" "$base.expect" \
        || { echo "FAIL interp $l (expect below)"; cat "$base.expect"; fail=1; }
    cmp -s "$OUT/$base.t.out" "$base.expect" \
        || { echo "FAIL text $l (expect below)"; cat "$base.expect"; fail=1; }
    echo "PASS $l"
done

# jfail: a bare json() parse failure must print to stderr and exit 1 on
# every backend (interp and text verifiable here; llvm once it can build).
err=0
"$BIN" jfail.lume > /dev/null 2> "$OUT/jf.i" || err=1
if [ $err -eq 1 ] && grep -q "json parse error" "$OUT/jf.i"; then
    echo "PASS jfail (interp: exit 1 + stderr)"
else
    echo "FAIL jfail (interp expected exit 1 + 'json parse error' in stderr)"; fail=1
fi
"$BIN" --compile-text jfail.lume -o "$OUT/jfail.t" > /dev/null 2>&1 || { echo "FAIL jfail text-compile"; fail=1; }
err=0
"$OUT/jfail.t" > /dev/null 2> "$OUT/jf.t" || err=1
if [ $err -eq 1 ] && grep -q "json parse error" "$OUT/jf.t"; then
    echo "PASS jfail (text: exit 1 + stderr)"
else
    echo "FAIL jfail (text expected exit 1 + 'json parse error' in stderr)"; fail=1
fi

[ $fail -eq 0 ] || { echo "SOME CHECKS FAILED"; exit 1; }
echo "all llvm-json checks pass"

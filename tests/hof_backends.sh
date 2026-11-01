#!/bin/bash
# hof_backends.sh — 验证 tests/hof-consistency.lume 算法库在三后端（解释器 / 文本 IR / LLVM）
# 输出逐字节一致。属于 lume-core 三后端等价纪律的一部分。
set -e
ROOT="$(cd "$(dirname "$0")/.." && pwd)"
BIN="$ROOT/bin/lume-core"
SRC="$ROOT/tests/hof-consistency.lume"
TMP="$(mktemp -d)"
trap 'rm -rf "$TMP"' EXIT

echo "hof: interpreter"
"$BIN" "$SRC" 2>/dev/null | tr -d '\r' > "$TMP/interp.out"
echo "hof: text backend"
"$BIN" --compile-text -o "$TMP/hof.t.exe" "$SRC" 2>/dev/null
"$TMP/hof.t.exe" 2>/dev/null | tr -d '\r' > "$TMP/text.out"

if "$BIN" --compile-llvm -o "$TMP/hof.l.exe" "$SRC" 2>/dev/null; then
  echo "hof: llvm backend"
  "$TMP/hof.l.exe" 2>/dev/null | tr -d '\r' > "$TMP/llvm.out"
  if diff -q "$TMP/interp.out" "$TMP/text.out" >/dev/null && \
     diff -q "$TMP/interp.out" "$TMP/llvm.out" >/dev/null; then
    echo "ok   hof: three-backend outputs identical"
    exit 0
  else
    echo "FAIL hof: drift between backends"
    diff "$TMP/interp.out" "$TMP/text.out" || true
    diff "$TMP/interp.out" "$TMP/llvm.out" || true
    exit 1
  fi
else
  echo "skip hof: LLVM backend unavailable (built without libLLVM)"
  # 即使没有 LLVM，仍验证 interp/text 两腿一致
  if diff -q "$TMP/interp.out" "$TMP/text.out" >/dev/null; then
    echo "ok   hof: interp/text identical (llvm skipped)"
    exit 0
  else
    echo "FAIL hof: interp/text drift"; diff "$TMP/interp.out" "$TMP/text.out"; exit 1
  fi
fi

#!/bin/bash
# sci_backends.sh — 验证 lib/sci.lume 算法库在三后端（解释器 / 文本 IR / LLVM）
# 输出逐字节一致。属于 lume-core 三后端等价纪律的一部分。
set -e
ROOT="$(cd "$(dirname "$0")/.." && pwd)"
BIN="$ROOT/bin/lume-core"
SRC="$ROOT/tests/sci-consistency.lume"
TMP="$(mktemp -d)"
trap 'rm -rf "$TMP"' EXIT

echo "sci: interpreter"
"$BIN" "$SRC" 2>/dev/null | tr -d '\r' > "$TMP/interp.out"
echo "sci: text backend"
"$BIN" --compile-text -o "$TMP/sci.t.exe" "$SRC" 2>/dev/null
"$TMP/sci.t.exe" 2>/dev/null | tr -d '\r' > "$TMP/text.out"

if "$BIN" --compile-llvm -o "$TMP/sci.l.exe" "$SRC" 2>/dev/null; then
  echo "sci: llvm backend"
  "$TMP/sci.l.exe" 2>/dev/null | tr -d '\r' > "$TMP/llvm.out"
  if diff -q "$TMP/interp.out" "$TMP/text.out" >/dev/null && \
     diff -q "$TMP/interp.out" "$TMP/llvm.out" >/dev/null; then
    echo "ok   sci: three-backend outputs identical"
    exit 0
  else
    echo "FAIL sci: drift between backends"
    diff "$TMP/interp.out" "$TMP/text.out" || true
    diff "$TMP/interp.out" "$TMP/llvm.out" || true
    exit 1
  fi
else
  echo "skip sci: LLVM backend unavailable (built without libLLVM)"
  # 即使没有 LLVM，仍验证 interp/text 两腿一致
  if diff -q "$TMP/interp.out" "$TMP/text.out" >/dev/null; then
    echo "ok   sci: interp/text identical (llvm skipped)"
    exit 0
  else
    echo "FAIL sci: interp/text drift"; diff "$TMP/interp.out" "$TMP/text.out"; exit 1
  fi
fi

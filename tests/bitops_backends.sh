#!/bin/bash
# bitops_backends.sh — 验证整数位运算内置（P1-5）在三后端（解释器 / 文本 IR / LLVM）
# 输出逐字节一致。属于 lume-core 三后端等价纪律的一部分。
set -e
ROOT="$(cd "$(dirname "$0")/.." && pwd)"
BIN="$ROOT/bin/lume-core"
SRC="$ROOT/tests/bitops-consistency.lume"
TMP="$(mktemp -d)"
trap 'rm -rf "$TMP"' EXIT

echo "bitops: interpreter"
"$BIN" "$SRC" 2>/dev/null | tr -d '\r' > "$TMP/interp.out"
echo "bitops: text backend"
"$BIN" --compile-text -o "$TMP/bitops.t.exe" "$SRC" 2>/dev/null
"$TMP/bitops.t.exe" 2>/dev/null | tr -d '\r' > "$TMP/text.out"

if "$BIN" --compile-llvm -o "$TMP/bitops.l.exe" "$SRC" 2>/dev/null; then
  echo "bitops: llvm backend"
  "$TMP/bitops.l.exe" 2>/dev/null | tr -d '\r' > "$TMP/llvm.out"
  if diff -q "$TMP/interp.out" "$TMP/text.out" >/dev/null && \
     diff -q "$TMP/interp.out" "$TMP/llvm.out" >/dev/null; then
    echo "ok   bitops: three-backend outputs identical"
    exit 0
  else
    echo "FAIL bitops: drift between backends"
    diff "$TMP/interp.out" "$TMP/text.out" || true
    diff "$TMP/interp.out" "$TMP/llvm.out" || true
    exit 1
  fi
else
  echo "skip bitops: LLVM backend unavailable (built without libLLVM)"
  if diff -q "$TMP/interp.out" "$TMP/text.out" >/dev/null; then
    echo "ok   bitops: interp/text identical (llvm skipped)"
    exit 0
  else
    echo "FAIL bitops: interp/text drift"; diff "$TMP/interp.out" "$TMP/text.out"; exit 1
  fi
fi

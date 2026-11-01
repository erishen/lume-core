#!/bin/bash
# p1-2_backends.sh — 验证数组字面量元素类型通用推断（P1-2）在三后端
# （解释器 / 文本 IR / LLVM）输出逐字节一致。属于 lume-core 三后端等价纪律的一部分。
set -e
ROOT="$(cd "$(dirname "$0")/.." && pwd)"
BIN="$ROOT/bin/lume-core"
SRC="$ROOT/tests/p1-2-consistency.lume"
TMP="$(mktemp -d)"
trap 'rm -rf "$TMP"' EXIT

echo "p1-2: interpreter"
"$BIN" "$SRC" 2>/dev/null | tr -d '\r' > "$TMP/interp.out"
echo "p1-2: text backend"
"$BIN" --compile-text -o "$TMP/p12.t.exe" "$SRC" 2>/dev/null
"$TMP/p12.t.exe" 2>/dev/null | tr -d '\r' > "$TMP/text.out"

if "$BIN" --compile-llvm -o "$TMP/p12.l.exe" "$SRC" 2>/dev/null; then
  echo "p1-2: llvm backend"
  "$TMP/p12.l.exe" 2>/dev/null | tr -d '\r' > "$TMP/llvm.out"
  if diff -q "$TMP/interp.out" "$TMP/text.out" >/dev/null && \
     diff -q "$TMP/interp.out" "$TMP/llvm.out" >/dev/null; then
    echo "ok   p1-2: three-backend outputs identical"
    exit 0
  else
    echo "FAIL p1-2: drift between backends"
    diff "$TMP/interp.out" "$TMP/text.out" || true
    diff "$TMP/interp.out" "$TMP/llvm.out" || true
    exit 1
  fi
else
  echo "skip p1-2: LLVM backend unavailable (built without libLLVM)"
  if diff -q "$TMP/interp.out" "$TMP/text.out" >/dev/null; then
    echo "ok   p1-2: interp/text identical (llvm skipped)"
    exit 0
  else
    echo "FAIL p1-2: interp/text drift"; diff "$TMP/interp.out" "$TMP/text.out"; exit 1
  fi
fi

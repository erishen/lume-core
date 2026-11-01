#!/bin/bash
# p1-8_backends.sh — 验证顶层读内置数值常量 pi/e（P1-8）在三后端
# （解释器 / 文本 IR / LLVM）输出逐字节一致。属于 lume-core 三后端等价纪律的一部分。
set -e
ROOT="$(cd "$(dirname "$0")/.." && pwd)"
BIN="$ROOT/bin/lume-core"
SRC="$ROOT/tests/p1-8-consistency.lume"
TMP="$(mktemp -d)"
trap 'rm -rf "$TMP"' EXIT

echo "p1-8: interpreter"
"$BIN" "$SRC" 2>/dev/null | tr -d '\r' > "$TMP/interp.out"
echo "p1-8: text backend"
"$BIN" --compile-text -o "$TMP/p18.t.exe" "$SRC" 2>/dev/null
"$TMP/p18.t.exe" 2>/dev/null | tr -d '\r' > "$TMP/text.out"

if "$BIN" --compile-llvm -o "$TMP/p18.l.exe" "$SRC" 2>/dev/null; then
  echo "p1-8: llvm backend"
  "$TMP/p18.l.exe" 2>/dev/null | tr -d '\r' > "$TMP/llvm.out"
  if diff -q "$TMP/interp.out" "$TMP/text.out" >/dev/null && \
     diff -q "$TMP/interp.out" "$TMP/llvm.out" >/dev/null; then
    echo "ok   p1-8: three-backend outputs identical"
    exit 0
  else
    echo "FAIL p1-8: drift between backends"
    diff "$TMP/interp.out" "$TMP/text.out" || true
    diff "$TMP/interp.out" "$TMP/llvm.out" || true
    exit 1
  fi
else
  # 无 libLLVM 的构建：退化为解释器 vs 文本 IR 双后端
  echo "p1-8: llvm backend unavailable, two-backend mode"
  if diff -q "$TMP/interp.out" "$TMP/text.out" >/dev/null; then
    echo "ok   p1-8: interpreter/text outputs identical"
    exit 0
  else
    echo "FAIL p1-8: drift between interpreter and text backend"
    diff "$TMP/interp.out" "$TMP/text.out" || true
    exit 1
  fi
fi

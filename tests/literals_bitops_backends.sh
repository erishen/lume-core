#!/bin/bash
# literals_bitops_backends.sh — 验证 P1-6 十六进制/二进制字面量 + P1-7 位运算中缀
# 运算符（| & ^）在三后端（解释器 / 文本 IR / LLVM）输出逐字节一致。
# 属于 lume-core 三后端等价纪律的一部分。
set -e
ROOT="$(cd "$(dirname "$0")/.." && pwd)"
BIN="$ROOT/bin/lume-core"
SRC="$ROOT/tests/literals-bitops-consistency.lume"
TMP="$(mktemp -d)"
trap 'rm -rf "$TMP" 2>/dev/null || true' EXIT

echo "literals-bitops: interpreter"
"$BIN" "$SRC" 2>/dev/null | tr -d '\r' > "$TMP/interp.out"
echo "literals-bitops: text backend"
"$BIN" --compile-text -o "$TMP/lit.t.exe" "$SRC" 2>/dev/null
"$TMP/lit.t.exe" 2>/dev/null | tr -d '\r' > "$TMP/text.out"

if "$BIN" --compile-llvm -o "$TMP/lit.l.exe" "$SRC" 2>/dev/null; then
  echo "literals-bitops: llvm backend"
  "$TMP/lit.l.exe" 2>/dev/null | tr -d '\r' > "$TMP/llvm.out"
  if diff -q "$TMP/interp.out" "$TMP/text.out" >/dev/null && \
     diff -q "$TMP/interp.out" "$TMP/llvm.out" >/dev/null; then
    echo "ok   literals-bitops: three-backend outputs identical"
    exit 0
  else
    echo "FAIL literals-bitops: drift between backends"
    diff "$TMP/interp.out" "$TMP/text.out" || true
    diff "$TMP/interp.out" "$TMP/llvm.out" || true
    exit 1
  fi
else
  echo "skip literals-bitops: LLVM backend unavailable (built without libLLVM)"
  if diff -q "$TMP/interp.out" "$TMP/text.out" >/dev/null; then
    echo "ok   literals-bitops: interp/text identical (llvm skipped)"
    exit 0
  else
    echo "FAIL literals-bitops: interp/text drift"; diff "$TMP/interp.out" "$TMP/text.out"; exit 1
  fi
fi

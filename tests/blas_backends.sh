#!/bin/bash
# blas_backends.sh — 验证 lib/blas.lume（C kernel matmul 快路径）在三后端
# （解释器 / 文本 IR / LLVM）输出逐字节一致。属于 lume-core 三后端等价纪律的一部分。
set -e
ROOT="$(cd "$(dirname "$0")/.." && pwd)"
BIN="$ROOT/bin/lume-core"
SRC="$ROOT/tests/blas-consistency.lume"
TMP="$(mktemp -d)"
trap 'rm -rf "$TMP" 2>/dev/null || true' EXIT

echo "blas: interpreter"
"$BIN" "$SRC" 2>/dev/null | tr -d '\r' > "$TMP/interp.out"
echo "blas: text backend"
"$BIN" --compile-text -o "$TMP/blas.t.exe" "$SRC" 2>/dev/null
"$TMP/blas.t.exe" 2>/dev/null | tr -d '\r' > "$TMP/text.out"

if "$BIN" --compile-llvm -o "$TMP/blas.l.exe" "$SRC" 2>/dev/null; then
  echo "blas: llvm backend"
  "$TMP/blas.l.exe" 2>/dev/null | tr -d '\r' > "$TMP/llvm.out"
  if diff -q "$TMP/interp.out" "$TMP/text.out" >/dev/null && \
     diff -q "$TMP/interp.out" "$TMP/llvm.out" >/dev/null; then
    echo "ok   blas: three-backend outputs identical"
    exit 0
  else
    echo "FAIL blas: drift between backends"
    diff "$TMP/interp.out" "$TMP/text.out" || true
    diff "$TMP/interp.out" "$TMP/llvm.out" || true
    exit 1
  fi
else
  echo "skip blas: LLVM backend unavailable (built without libLLVM)"
  if diff -q "$TMP/interp.out" "$TMP/text.out" >/dev/null; then
    echo "ok   blas: interp/text identical (llvm skipped)"
    exit 0
  else
    echo "FAIL blas: interp/text drift"; diff "$TMP/interp.out" "$TMP/text.out"; exit 1
  fi
fi

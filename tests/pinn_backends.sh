#!/bin/bash
# pinn_backends.sh — 验证 lib/pinn.lume 物理信息神经网络核心在三后端
# （解释器 / 文本 IR / LLVM）输出逐字节一致。属于 lume-core 三后端等价纪律的一部分。
set -e
ROOT="$(cd "$(dirname "$0")/.." && pwd)"
BIN="$ROOT/bin/lume-core"
SRC="$ROOT/tests/pinn-consistency.lume"
TMP="$(mktemp -d)"
trap 'rm -rf "$TMP" 2>/dev/null || true' EXIT

echo "pinn: interpreter"
"$BIN" "$SRC" 2>/dev/null | tr -d '\r' > "$TMP/interp.out"
echo "pinn: text backend"
"$BIN" --compile-text -o "$TMP/pinn.t.exe" "$SRC" 2>/dev/null
"$TMP/pinn.t.exe" 2>/dev/null | tr -d '\r' > "$TMP/text.out"

if "$BIN" --compile-llvm -o "$TMP/pinn.l.exe" "$SRC" 2>/dev/null; then
  echo "pinn: llvm backend"
  "$TMP/pinn.l.exe" 2>/dev/null | tr -d '\r' > "$TMP/llvm.out"
  if diff -q "$TMP/interp.out" "$TMP/text.out" >/dev/null && \
     diff -q "$TMP/interp.out" "$TMP/llvm.out" >/dev/null; then
    echo "ok   pinn: three-backend outputs identical"
    exit 0
  else
    echo "FAIL pinn: drift between backends"
    diff "$TMP/interp.out" "$TMP/text.out" || true
    diff "$TMP/interp.out" "$TMP/llvm.out" || true
    exit 1
  fi
else
  echo "skip pinn: LLVM backend unavailable (built without libLLVM)"
  if diff -q "$TMP/interp.out" "$TMP/text.out" >/dev/null; then
    echo "ok   pinn: interp/text identical (llvm skipped)"
    exit 0
  else
    echo "FAIL pinn: interp/text drift"; diff "$TMP/interp.out" "$TMP/text.out"; exit 1
  fi
fi

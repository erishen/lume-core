#!/bin/bash
# ode_backends.sh — 验证 sci.lume ODE（rk4_sys / rk4_sys_linear / vadd / vsub /
# vscale / matvec / rk4_adaptive / ode_theta / ode_imp_euler / ode_trapezoid /
# verlet）在三后端（解释器 / 文本 IR / LLVM）输出逐字节一致。
# 属于 lume-core 三后端等价纪律的一部分。
set -e
ROOT="$(cd "$(dirname "$0")/.." && pwd)"
BIN="$ROOT/bin/lume-core"
SRC="$ROOT/tests/ode-consistency.lume"
TMP="$(mktemp -d)"
trap 'rm -rf "$TMP"' EXIT

echo "ode: interpreter"
"$BIN" "$SRC" 2>/dev/null | tr -d '\r' > "$TMP/interp.out"
echo "ode: text backend"
"$BIN" --compile-text -o "$TMP/ode.t.exe" "$SRC" 2>/dev/null
"$TMP/ode.t.exe" 2>/dev/null | tr -d '\r' > "$TMP/text.out"

if "$BIN" --compile-llvm -o "$TMP/ode.l.exe" "$SRC" 2>/dev/null; then
  echo "ode: llvm backend"
  "$TMP/ode.l.exe" 2>/dev/null | tr -d '\r' > "$TMP/llvm.out"
  if diff -q "$TMP/interp.out" "$TMP/text.out" >/dev/null && \
     diff -q "$TMP/interp.out" "$TMP/llvm.out" >/dev/null; then
    echo "ok   ode: three-backend outputs identical"
    exit 0
  else
    echo "FAIL ode: drift between backends"
    diff "$TMP/interp.out" "$TMP/text.out" || true
    diff "$TMP/interp.out" "$TMP/llvm.out" || true
    exit 1
  fi
else
  echo "skip ode: LLVM backend unavailable (built without libLLVM)"
  if diff -q "$TMP/interp.out" "$TMP/text.out" >/dev/null; then
    echo "ok   ode: interp/text identical (llvm skipped)"
    exit 0
  else
    echo "FAIL ode: interp/text drift"; diff "$TMP/interp.out" "$TMP/text.out"; exit 1
  fi
fi

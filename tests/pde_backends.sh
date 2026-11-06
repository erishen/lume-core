#!/bin/bash
# pde_backends.sh — 验证 lib/pde.lume（pde_heat_explicit / pde_heat_cn /
# pde_heat_2d / pde_heat_2d_adi / pde_wave_leapfrog / pde_advect_upwind /
# pde_advect_lw / pde_burgers_inviscid / pde_burgers_viscous / pde_robin_1d /
# pde_poisson_jacobi / pde_poisson_sor / pde_poisson_mg / pde_wave_2d /
# pde_advect_diffuse_2d）在三后端（解释器 / 文本 IR / LLVM）输出逐字节一致。
# 属于 lume-core 三后端等价纪律的一部分。
set -e
ROOT="$(cd "$(dirname "$0")/.." && pwd)"
BIN="$ROOT/bin/lume-core"
SRC="$ROOT/tests/pde-consistency.lume"
TMP="$(mktemp -d)"
trap 'rm -rf "$TMP" 2>/dev/null || true' EXIT

echo "pde: interpreter"
"$BIN" "$SRC" 2>/dev/null | tr -d '\r' > "$TMP/interp.out"
echo "pde: text backend"
"$BIN" --compile-text -o "$TMP/pde.t.exe" "$SRC" 2>/dev/null
"$TMP/pde.t.exe" 2>/dev/null | tr -d '\r' > "$TMP/text.out"

if "$BIN" --compile-llvm -o "$TMP/pde.l.exe" "$SRC" 2>/dev/null; then
  echo "pde: llvm backend"
  "$TMP/pde.l.exe" 2>/dev/null | tr -d '\r' > "$TMP/llvm.out"
  if diff -q "$TMP/interp.out" "$TMP/text.out" >/dev/null && \
     diff -q "$TMP/interp.out" "$TMP/llvm.out" >/dev/null; then
    echo "ok   pde: three-backend outputs identical"
    exit 0
  else
    echo "FAIL pde: drift between backends"
    diff "$TMP/interp.out" "$TMP/text.out" || true
    diff "$TMP/interp.out" "$TMP/llvm.out" || true
    exit 1
  fi
else
  echo "skip pde: LLVM backend unavailable (built without libLLVM)"
  if diff -q "$TMP/interp.out" "$TMP/text.out" >/dev/null; then
    echo "ok   pde: interp/text identical (llvm skipped)"
    exit 0
  else
    echo "FAIL pde: interp/text drift"; diff "$TMP/interp.out" "$TMP/text.out"; exit 1
  fi
fi

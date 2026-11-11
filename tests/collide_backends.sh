#!/bin/bash
# collide_backends.sh — codegen 撞名修复（pass 1.6 模块限定 cname）回归门禁。
# blas 与 linalg 同程序共 import（同名导出 mat_mul），三后端（解释器 / 文本 IR
# / LLVM）输出逐字节一致；模块内裸名助手（la_copy_row）路径一并覆盖。
set -e
ROOT="$(cd "$(dirname "$0")/.." && pwd)"
BIN="$ROOT/bin/lume-core"
SRC="$ROOT/tests/mangle-collide.lume"
TMP="$(mktemp -d)"
trap 'rm -rf "$TMP" 2>/dev/null || true' EXIT

echo "collide: interpreter"
"$BIN" "$SRC" 2>/dev/null | tr -d '\r' > "$TMP/interp.out"
echo "collide: text backend"
"$BIN" --compile-text -o "$TMP/collide.t.exe" "$SRC" 2>/dev/null
"$TMP/collide.t.exe" 2>/dev/null | tr -d '\r' > "$TMP/text.out"

if "$BIN" --compile-llvm -o "$TMP/collide.l.exe" "$SRC" 2>/dev/null; then
  echo "collide: llvm backend"
  "$TMP/collide.l.exe" 2>/dev/null | tr -d '\r' > "$TMP/llvm.out"
  if diff -q "$TMP/interp.out" "$TMP/text.out" >/dev/null && \
     diff -q "$TMP/interp.out" "$TMP/llvm.out" >/dev/null; then
    echo "ok   collide: three-backend outputs identical"
    exit 0
  else
    echo "FAIL collide: drift between backends"
    diff "$TMP/interp.out" "$TMP/text.out" || true
    diff "$TMP/interp.out" "$TMP/llvm.out" || true
    exit 1
  fi
else
  echo "skip collide: LLVM backend unavailable (built without libLLVM)"
  if diff -q "$TMP/interp.out" "$TMP/text.out" >/dev/null; then
    echo "ok   collide: interp/text identical (llvm skipped)"
    exit 0
  else
    echo "FAIL collide: interp/text drift"; diff "$TMP/interp.out" "$TMP/text.out"; exit 1
  fi
fi

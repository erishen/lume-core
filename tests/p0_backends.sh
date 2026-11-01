#!/bin/bash
# p0_backends.sh — 验证语言演进提案 P0 功能（div/mod、方法关键字绑定名、
# fn 类型值的间接调用）在三后端输出逐字节一致。
set -e
ROOT="$(cd "$(dirname "$0")/.." && pwd)"
BIN="$ROOT/bin/lume-core"
SRC="$ROOT/tests/p0-consistency.lume"
TMP="$(mktemp -d)"
trap 'rm -rf "$TMP"' EXIT

echo "p0: interpreter"
"$BIN" "$SRC" 2>/dev/null | tr -d '\r' > "$TMP/interp.out"
echo "p0: text backend"
"$BIN" --compile-text -o "$TMP/p0.t.exe" "$SRC" 2>/dev/null
"$TMP/p0.t.exe" 2>/dev/null | tr -d '\r' > "$TMP/text.out"

if "$BIN" --compile-llvm -o "$TMP/p0.l.exe" "$SRC" 2>/dev/null; then
  echo "p0: llvm backend"
  "$TMP/p0.l.exe" 2>/dev/null | tr -d '\r' > "$TMP/llvm.out"
  if diff -q "$TMP/interp.out" "$TMP/text.out" >/dev/null && \
     diff -q "$TMP/interp.out" "$TMP/llvm.out" >/dev/null; then
    echo "ok   p0: three-backend outputs identical"
    exit 0
  else
    echo "FAIL p0: drift between backends"
    diff "$TMP/interp.out" "$TMP/text.out" || true
    diff "$TMP/interp.out" "$TMP/llvm.out" || true
    exit 1
  fi
else
  echo "skip p0: LLVM backend unavailable (built without libLLVM)"
  if diff -q "$TMP/interp.out" "$TMP/text.out" >/dev/null; then
    echo "ok   p0: interp/text identical (llvm skipped)"
    exit 0
  else
    echo "FAIL p0: interp/text drift"; diff "$TMP/interp.out" "$TMP/text.out"; exit 1
  fi
fi

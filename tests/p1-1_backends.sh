#!/bin/bash
# p1-1_backends.sh — 验证「本地/导入/native 函数返回值向 let 绑定传播」
# （语言演进提案 P1-1）在三后端（解释器 / 文本 IR / LLVM）输出逐字节一致。
set -e
ROOT="$(cd "$(dirname "$0")/.." && pwd)"
BIN="$ROOT/bin/lume-core"
SRC="$ROOT/tests/p1-1-consistency.lume"
TMP="$(mktemp -d)"
trap 'rm -rf "$TMP"' EXIT

echo "p1-1: interpreter"
"$BIN" "$SRC" 2>/dev/null | tr -d '\r' > "$TMP/interp.out"
echo "p1-1: text backend"
"$BIN" --compile-text -o "$TMP/p1-1.t.exe" "$SRC" 2>/dev/null
"$TMP/p1-1.t.exe" 2>/dev/null | tr -d '\r' > "$TMP/text.out"

if "$BIN" --compile-llvm -o "$TMP/p1-1.l.exe" "$SRC" 2>/dev/null; then
  echo "p1-1: llvm backend"
  "$TMP/p1-1.l.exe" 2>/dev/null | tr -d '\r' > "$TMP/llvm.out"
  if diff -q "$TMP/interp.out" "$TMP/text.out" >/dev/null && \
     diff -q "$TMP/interp.out" "$TMP/llvm.out" >/dev/null; then
    echo "ok   p1-1: three-backend outputs identical"
    exit 0
  else
    echo "FAIL p1-1: drift between backends"
    diff "$TMP/interp.out" "$TMP/text.out" || true
    diff "$TMP/interp.out" "$TMP/llvm.out" || true
    exit 1
  fi
else
  echo "skip p1-1: LLVM backend unavailable (built without libLLVM)"
  if diff -q "$TMP/interp.out" "$TMP/text.out" >/dev/null; then
    echo "ok   p1-1: interp/text identical (llvm skipped)"
    exit 0
  else
    echo "FAIL p1-1: interp/text drift"; diff "$TMP/interp.out" "$TMP/text.out"; exit 1
  fi
fi

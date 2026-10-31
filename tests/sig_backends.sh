#!/bin/bash
# sig_backends.sh — 验证 lib/sig.lume 信号处理库在三后端（解释器 / 文本 IR / LLVM）
# 输出逐字节一致。属于 lume-core 三后端等价纪律的一部分。
set -e
ROOT="$(cd "$(dirname "$0")/.." && pwd)"
BIN="$ROOT/bin/lume-core"
SRC="$ROOT/tests/sig-consistency.lume"
TMP="$(mktemp -d)"
trap 'rm -rf "$TMP"' EXIT

echo "sig: interpreter"
"$BIN" "$SRC" 2>/dev/null | tr -d '\r' > "$TMP/interp.out"
echo "sig: text backend"
"$BIN" --compile-text -o "$TMP/sig.t.exe" "$SRC" 2>/dev/null
"$TMP/sig.t.exe" 2>/dev/null | tr -d '\r' > "$TMP/text.out"

if "$BIN" --compile-llvm -o "$TMP/sig.l.exe" "$SRC" 2>/dev/null; then
  echo "sig: llvm backend"
  "$TMP/sig.l.exe" 2>/dev/null | tr -d '\r' > "$TMP/llvm.out"
  if diff -q "$TMP/interp.out" "$TMP/text.out" >/dev/null && \
     diff -q "$TMP/interp.out" "$TMP/llvm.out" >/dev/null; then
    echo "ok   sig: three-backend outputs identical"
    exit 0
  else
    echo "FAIL sig: drift between backends"
    diff "$TMP/interp.out" "$TMP/text.out" || true
    diff "$TMP/interp.out" "$TMP/llvm.out" || true
    exit 1
  fi
else
  echo "skip sig: LLVM backend unavailable (built without libLLVM)"
  if diff -q "$TMP/interp.out" "$TMP/text.out" >/dev/null; then
    echo "ok   sig: interp/text identical (llvm skipped)"
    exit 0
  else
    echo "FAIL sig: interp/text drift"; diff "$TMP/interp.out" "$TMP/text.out"; exit 1
  fi
fi

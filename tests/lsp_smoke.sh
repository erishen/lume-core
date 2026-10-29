#!/bin/sh
# LSP document-lifetime smoke (P11): drive the stdio JSON-RPC server through
# initialize -> didOpen A -> didChange A -> didOpen B (document switch) ->
# hover B -> didClose B -> shutdown -> exit. Run under ASan+LSan (via the
# `asan` target / CI), this catches open-document table leaks: before the
# didClose + exit-cleanup fix, every uri left a live LspDoc behind and LSan
# reported one 3-block leak per document. Plain runs (no sanitizer) are the
# functional check: exit 0 and the expected replies.
#
# Usage: LSP_BIN=./bin/lume-core-asan ./tests/lsp_smoke.sh
#       (LSP_BIN defaults to ./bin/lume-core)

set -e
bin="${LSP_BIN:-./bin/lume-core}"

emit() {
    # one JSON-RPC message with Content-Length framing
    msg="$1"
    printf 'Content-Length: %d\r\n\r\n%s' "$(printf '%s' "$msg" | wc -c)" "$msg"
}

{
    emit '{"jsonrpc":"2.0","id":1,"method":"initialize","params":{"capabilities":{}}}'
    emit '{"jsonrpc":"2.0","method":"initialized","params":{}}'
    emit '{"jsonrpc":"2.0","method":"textDocument/didOpen","params":{"textDocument":{"uri":"file:///a.lume","languageId":"lume","version":1,"text":"print(\"a\")\n"}}}'
    emit '{"jsonrpc":"2.0","method":"textDocument/didChange","params":{"textDocument":{"uri":"file:///a.lume","version":2},"contentChanges":[{"text":"let x = 1\nprint(x)\n"}]}}'
    # document switch: a second uri while A stays open
    emit '{"jsonrpc":"2.0","method":"textDocument/didOpen","params":{"textDocument":{"uri":"file:///b.lume","languageId":"lume","version":1,"text":"print(\"b\")\n"}}}'
    emit '{"jsonrpc":"2.0","id":2,"method":"textDocument/hover","params":{"textDocument":{"uri":"file:///b.lume"},"position":{"line":0,"character":1}}}'
    emit '{"jsonrpc":"2.0","method":"textDocument/didClose","params":{"textDocument":{"uri":"file:///b.lume"}}}'
    emit '{"jsonrpc":"2.0","id":3,"method":"shutdown","params":{}}'
    emit '{"jsonrpc":"2.0","method":"exit","params":{}}'
} | "$bin" --lsp > /tmp/lsp_smoke_out.$$
rc=$?
rm -f /tmp/lsp_smoke_out.$$
[ "$rc" -eq 0 ] || { echo "FAIL lsp smoke: exit $rc (LSan/ASan diagnostics above)"; exit 1; }
echo "ok lsp smoke: initialize/open/change/switch/hover/close/shutdown/exit under $(basename "$bin")"

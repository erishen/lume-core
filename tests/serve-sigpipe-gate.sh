#!/bin/sh
# POSIX regression for the 2026-11-09 SIGPIPE finding.
#
# serve() is single-threaded and writes each response as two sends (header,
# then body). With the default SIGPIPE disposition, a client that connects
# and disconnects without sending a request -- exactly what
# tests/wait_port.py does -- made the 400 response's body write kill the
# whole server. The port stayed connectable to the readiness probe, then
# every later curl saw code=000. Windows has no SIGPIPE, which is why this
# passed locally and only surfaced in the gcc:13-bookworm CI lane (and,
# historically, as a flaky macOS route-cap failure: ci 42558781265).
#
# This gate reproduces the abort deterministically with SO_LINGER(1,0)
# (RST instead of a benign FIN), then asserts the server is still alive
# and still answers a real request. On Windows the abort is harmless, so
# the gate degenerates into a smoke test there; on Linux/macOS it is a
# real gate. See TODO.md P17, "bookworm lane".

set -eu

cd "$(dirname "$0")/.."

BIN=./bin/lume-core
if [ ! -x "${BIN}" ]; then BIN=./bin/lume-core.exe; fi
if [ ! -x "${BIN}" ]; then
    echo "FAIL serve-sigpipe-gate: build the binary first (make all)"
    exit 1
fi

PORT=18896
SRV_LOG=/tmp/lume-serve-sigpipe-srv.log
"${BIN}" tests/serve-sigpipe-srv.lume > "${SRV_LOG}" 2>&1 &
SRV=$!
trap 'kill "${SRV}" 2>/dev/null || true; exit 1' 1 2 3 15
trap 'kill "${SRV}" 2>/dev/null || true' 0

if ! python3 tests/wait_port.py "${PORT}" 15; then
    echo "FAIL serve-sigpipe-gate: server never listened on ${PORT}"
    cat "${SRV_LOG}"
    exit 1
fi

# Connect three times and abort each time without sending a single request
# byte. SO_LINGER(1,0) forces RST on close so the server's response write
# reliably hits EPIPE/SIGPIPE rather than a well-behaved FIN.
python3 - "${PORT}" <<'PY'
import socket, struct, sys, time
port = int(sys.argv[1])
for _ in range(3):
    s = socket.create_connection(("127.0.0.1", port), 1.0)
    try:
        s.setsockopt(socket.SOL_SOCKET, socket.SO_LINGER,
                     struct.pack("ii", 1, 0))
    except OSError:
        pass
    s.close()
    time.sleep(0.05)
PY

sleep 0.2

if ! kill -0 "${SRV}" 2>/dev/null; then
    echo "FAIL serve-sigpipe-gate: server exited after an aborted client (SIGPIPE not silenced)"
    cat "${SRV_LOG}"
    exit 1
fi

out=$(curl -sS --noproxy '*' --max-time 5 -w '\n%{http_code}' \
      "http://127.0.0.1:${PORT}/ping") || true
code=$(printf '%s\n' "${out}" | tail -1)
body=$(printf '%s\n' "${out}" | sed '$d')
if [ "${code}" != "200" ]; then
    echo "FAIL serve-sigpipe-gate: /ping returned code=${code} after aborted clients"
    cat "${SRV_LOG}"
    exit 1
fi
if ! printf '%s' "${body}" | grep -q '"ok"'; then
    echo "FAIL serve-sigpipe-gate: /ping body missing ok"
    printf '%s\n' "${body}"
    exit 1
fi

echo "ok   serve-sigpipe-gate: server survives aborted clients and still serves /ping"

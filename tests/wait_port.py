#!/usr/bin/env python3
"""tests/wait_port.py — 阻塞直到 TCP 端口可连接(或超时退出 1)。

用法: python3 tests/wait_port.py <port> [timeout_seconds=10]
用于 make http-noproxy-test 起本地 server 后等就绪: 盲 sleep 在慢 runner
(python3 冷启动)上会竞态——lume 连接时 server 还没监听, 10s 超时假失败。
"""
import socket
import sys
import time

port = int(sys.argv[1])
timeout = float(sys.argv[2]) if len(sys.argv) > 2 else 10.0
deadline = time.time() + timeout
while time.time() < deadline:
    try:
        socket.create_connection(("127.0.0.1", port), 0.5).close()
        sys.exit(0)
    except OSError:
        time.sleep(0.1)
sys.exit(1)

#!/usr/bin/env python3
"""tests/http_stream_srv.py — SSE 测试用的本地 server。

用法: python3 tests/http_stream_srv.py <port>
对 GET 返回 text/event-stream，立即推 'data: FIRST\\n\\n'，睡 1s，再推
'data: LAST\\n\\n'，再睡 0.5s 后关闭。用于验证 http_stream_read 是真流式
（第一次读只拿到 FIRST、不含 LAST），而非收尾一次性 collect。

仅绑 127.0.0.1（私有地址，lume 侧需 allow_private:true 才放行）。
"""
import sys
import time
from http.server import BaseHTTPRequestHandler, HTTPServer
import socketserver


class Srv(HTTPServer):
    def server_bind(self):
        # 跳过 HTTPServer 默认的 getfqdn 反向解析：macOS Sequoia 上对 127.0.0.1
        # 反解会触发「Local Network」权限、挂 30-35s（见 http_noproxy_srv.py）。
        socketserver.TCPServer.server_bind(self)
        self.server_name = "127.0.0.1"
        self.server_port = self.server_address[1]


class H(BaseHTTPRequestHandler):
    def do_GET(self):
        self.send_response(200)
        self.send_header("Content-Type", "text/event-stream")
        self.end_headers()
        # 第一个事件：立即推送并 flush，确保对端第一拍就能读到。
        self.wfile.write(b"data: FIRST\n\n")
        self.wfile.flush()
        # 关键间隔：FIRST 与 LAST 之间留出 1s，证明对端是边跑边读，
        # 而非服务端退出后一次性把两端都收走。
        time.sleep(1.0)
        self.wfile.write(b"data: LAST\n\n")
        self.wfile.flush()
        time.sleep(0.5)

    def log_message(self, *a):
        pass


def main():
    port = int(sys.argv[1]) if len(sys.argv) > 1 else 18895
    Srv(("127.0.0.1", port), H).serve_forever()


if __name__ == "__main__":
    main()

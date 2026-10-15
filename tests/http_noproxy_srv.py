#!/usr/bin/env python3
"""tests/http_noproxy_srv.py — NO_PROXY 测试的本地 server。

用法: python3 tests/http_noproxy_srv.py <port>
对任意 GET 返回 200 + body "ok"。仅绑定 127.0.0.1,用于验证 lume 出站
HTTP 在 NO_PROXY 命中时直连本机、绕过代理。
"""
import sys
from http.server import BaseHTTPRequestHandler, HTTPServer
import socketserver


class Srv(HTTPServer):
    def server_bind(self):
        # HTTPServer.server_bind() 额外做一次 socket.getfqdn(host)。在 macOS
        # Sequoia 上对 127.0.0.1 反向解析会触发「Local Network」权限、挂
        # 30-35 秒才超时(GitHub/Cirrus macOS runner 实测, 见
        # cirruslabs/cirrus-ci-docs#1302)——期间端口已 bind 未 listen,
        # connect 全部悬挂, 客户端只能等超时。这里跳过该解析: 本测试只
        # 需要"listen 后立刻可连", server_name 无关紧要。
        socketserver.TCPServer.server_bind(self)
        self.server_name = "127.0.0.1"
        self.server_port = self.server_address[1]


class H(BaseHTTPRequestHandler):
    def do_GET(self):
        body = b"ok"
        self.send_response(200)
        self.send_header("Content-Length", str(len(body)))
        self.send_header("Content-Type", "text/plain")
        self.end_headers()
        self.wfile.write(body)

    def log_message(self, *a):
        pass


def main():
    port = int(sys.argv[1]) if len(sys.argv) > 1 else 18891
    Srv(("127.0.0.1", port), H).serve_forever()


if __name__ == "__main__":
    main()

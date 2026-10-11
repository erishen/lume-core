#!/usr/bin/env python3
"""tests/http_noproxy_srv.py — NO_PROXY 测试的本地 server。

用法: python3 tests/http_noproxy_srv.py <port>
对任意 GET 返回 200 + body "ok"。仅绑定 127.0.0.1,用于验证 lume 出站
HTTP 在 NO_PROXY 命中时直连本机、绕过代理。
"""
import sys
from http.server import BaseHTTPRequestHandler, HTTPServer


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
    HTTPServer(("127.0.0.1", port), H).serve_forever()


if __name__ == "__main__":
    main()

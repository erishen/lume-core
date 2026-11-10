#!/usr/bin/env python3
"""tests/sse_stream_srv.py — P18 服务端 SSE 推送的客户端（消费者）验证。

用法: python3 tests/sse_stream_srv.py <port>

作为 SSE 客户端连到 lume 起的本地 server（tests/sse-stream.lume），请求
GET /progress 并带 `Accept: text/event-stream`，然后增量读取事件流并断言：

  - 响应是 200 + Content-Type: text/event-stream（服务端真发了 SSE 头）；
  - 依次收到 step 0..4 共 5 个 `data: {...}` 事件，JSON 内容与顺序正确；
  - 最后一个事件后连接被服务端关闭（EOF），证明事件是「边算边推」而非
    收尾一次性 dump。

任何失败打印 FAIL 并以退出码 1 结束；全部通过打印 PASS 行并以 0 结束。
仅绑 127.0.0.1。
"""
import json
import socket
import sys

EXPECTED_STEPS = list(range(5))  # step 0..4, total 5


def recv_until(sock, terminator, timeout, buf):
    """把 sock 读到出现 terminator 为止，返回累积到的 bytes；失败(超时或对端
    提前关闭)返回 None。注意: bytes 不可变, 这里必须返回新累积值, 不能在调用方
    原地修改——之前误用 `buf += chunk`(只重绑了局部变量)导致调用方 buffer 始终
    为空, 触发 line 70 `bytes.index` 抛 ValueError: subsection not found。"""
    sock.settimeout(timeout)
    while terminator not in buf:
        try:
            chunk = sock.recv(4096)
        except socket.timeout:
            return None
        if chunk == b"":
            return None  # 对端提前关闭
        buf += chunk
    return buf


def main():
    port = int(sys.argv[1]) if len(sys.argv) > 1 else 18907
    rc = 0

    def check(name, cond):
        nonlocal rc
        if cond:
            print("PASS " + name)
        else:
            print("FAIL " + name)
            rc = 1

    try:
        sock = socket.create_connection(("127.0.0.1", port), 5.0)
    except OSError as e:
        print("FAIL sse: cannot connect to 127.0.0.1:%d (%s)" % (port, e))
        return 1

    req = (
        "GET /progress HTTP/1.1\r\n"
        "Host: 127.0.0.1:%d\r\n" % port +
        "Accept: text/event-stream\r\n"
        "Connection: close\r\n"
        "\r\n"
    )
    sock.sendall(req.encode("utf-8"))

    # --- 读 headers 直到 \r\n\r\n ---
    header_buf = recv_until(sock, b"\r\n\r\n", 10.0, b"")
    if header_buf is None:
        print("FAIL sse: never received response headers")
        return 1
    header_end = header_buf.index(b"\r\n\r\n")
    head = header_buf[:header_end].decode("utf-8", "replace")
    body = header_buf[header_end + 4:]

    # --- 断言 SSE 头 ---
    first_line = head.split("\r\n", 1)[0]
    check("sse: HTTP status is 200", first_line.strip().endswith("200"))
    check("sse: Content-Type is text/event-stream",
          "text/event-stream" in head.lower())

    # --- 增量读取 body，按 \n\n 切分事件 ---
    # 一直读到对端关闭连接(EOF)为止: 服务端在发完最后一个事件、handler 返回后
    # 会主动关闭连接。这样 eof 一定能被观察到, 证明事件是「边算边推」而非收尾
    # 一次性 dump 后才关。12s 超时是兜底(正常几毫秒内就 EOF)。
    events = []
    eof = False
    sock.settimeout(12.0)
    while True:
        # 先看看有没有完整事件
        while "\n\n" in body:
            ev, body = body.split("\n\n", 1)
            # 去掉每个 data: 前缀行（可能多行）
            payload = []
            for line in ev.split("\n"):
                if line.startswith("data:"):
                    payload.append(line[5:].lstrip())
            if payload:
                joined = "\n".join(payload)
                try:
                    events.append(json.loads(joined))
                except ValueError:
                    print("FAIL sse: event payload is not JSON: %r" % joined)
                    return 1
        try:
            chunk = sock.recv(4096)
        except socket.timeout:
            break
        if chunk == b"":
            eof = True  # EOF：服务端在最后一个事件后关闭了连接
            break
        body += chunk.decode("utf-8", "replace")

    # 把 body 里残留的已完整事件也收进来
    while "\n\n" in body:
        ev, body = body.split("\n\n", 1)
        payload = []
        for line in ev.split("\n"):
            if line.startswith("data:"):
                payload.append(line[5:].lstrip())
        if payload:
            joined = "\n".join(payload)
            try:
                events.append(json.loads(joined))
            except ValueError:
                pass

    # --- 断言事件内容 ---
    got_steps = [e.get("step") for e in events if isinstance(e, dict)]
    check("sse: received exactly %d events" % len(EXPECTED_STEPS),
          len(events) == len(EXPECTED_STEPS))
    check("sse: event steps are 0..4 in order", got_steps == EXPECTED_STEPS)
    check("sse: every event has total=5",
          all(isinstance(e, dict) and e.get("total") == 5 for e in events))

    # 连接应在最后一个事件后由服务端关闭（我们读到 EOF 才跳出），证明事件是
    # 「边算边推」、而非收尾一次性 dump 后立刻关。
    check("sse: stream ended with connection close (EOF observed)", eof)

    sock.close()
    return rc


if __name__ == "__main__":
    sys.exit(main())

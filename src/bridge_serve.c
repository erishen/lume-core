/* bridge_serve.c — in-tree HTTP server for the standalone lume-core.
 *
 * The host tree (lume) serves routes through agent-httpd;
 * this tree has no agent-httpd, so bridge_run() implements a minimal
 * single-threaded HTTP server directly on sockets and dispatches requests
 * through vm->routes, exactly like the host bridge's shape:
 *
 *   server { host = "..."; port = N; }   -> vm->server_config
 *   get "/p", handler                    -> vm->routes
 *   run()                                -> bridge_run(vm)
 *
 * Responses: string -> text/html, map -> application/json, null -> 204,
 * unmatched -> 404, handler error -> 500. req carries path (query
 * stripped), method, query, query_params (URL-decoded), body, label.
 *
 * POSIX edition. The Windows/mingw port compiles this file through
 * net_compat.h so the two platforms share one bridge_run() body.
 */

#include "lume.h"
#include "sbuf.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

#if defined(_WIN32)
/* winnt.h's _TOKEN_INFORMATION_CLASS has an enum member TokenType, which
 * clashes with lume.h's typedef name; shim it away for the windows headers. */
#define TokenType LumeWinTokTypeShim
#include <winsock2.h>
#include <ws2tcpip.h>
#undef TokenType
#include <process.h>
#else
#include <errno.h>
#include <sys/types.h>
#include <sys/socket.h>
#include <netdb.h>
#include <netinet/in.h>
#include <unistd.h>
#include <sys/time.h>
#endif

/* ---- platform socket glue (shared shape with bridge_native.c) ---- */
#if defined(_WIN32)
typedef SOCKET SockFd;
#define SOCK_BADP(f) ((f) == INVALID_SOCKET)
#define sock_close(f) closesocket(f)
#define sock_err() WSAGetLastError()
#define SOCK_EINTR WSAEINTR
#define sock_send(f, b, n) send((f), (const char *)(b), (int)(n), 0)
#define sock_recv(f, b, n) recv((f), (char *)(b), (int)(n), 0)
#define lume_getpid() _getpid()
static int g_wsock_started = 0;
static int wsock_start(void)
{
    if (!g_wsock_started) {
        WSADATA wd;
        if (WSAStartup(MAKEWORD(2, 2), &wd) != 0) return 0;
        g_wsock_started = 1;
    }
    return 1;
}

/* gettimeofday is not in the windows CRT; the only use is a monotonic-ish
 * timestamp, _ftime covers it. */
#include <sys/timeb.h>
static int lume_gettimeofday(struct timeval *tv, void *tz)
{
    struct _timeb tb;
    (void)tz;
    _ftime(&tb);
    tv->tv_sec = (long)tb.time;
    tv->tv_usec = tb.millitm * 1000;
    return 0;
}
#define gettimeofday lume_gettimeofday
#else
typedef int SockFd;
#define SOCK_BADP(f) ((f) < 0)
#define sock_close(f) close(f)
#define sock_err() errno
#define SOCK_EINTR EINTR
#define sock_send(f, b, n) send((f), (b), (n), 0)
#define sock_recv(f, b, n) recv((f), (b), (n), 0)
#define lume_getpid() getpid()
#endif

#define SERVE_BACKLOG 16
#define HEAD_BUF_MAX 16384
#define REQ_BUF_MAX (1 << 20)

typedef struct {
    char method[16];
    char path[1024];       /* path only, query stripped (URL-encoded) */
    char query[1024];      /* raw query string without '?' */
    char cookie[512];      /* raw Cookie header value (lume_sid=...), or "" */
    /* SSE: 路由可以返回 { sse: [...] } 走流式; 但只有客户端 Accept 里明确
     * 接受 text/event-stream 时才这么回 —— 否则同样的路由给浏览器(过代理、
     * 中间层会缓冲)会卡住不显示。 */
    char accept[256];      /* raw Accept header value, or "" */
    char body[REQ_BUF_MAX];
    size_t body_len;
    const char *label;     /* route label, or NULL */
} HttpReq;

static long serve_now_ms(void)
{
    struct timeval tv;
    gettimeofday(&tv, NULL);
    return (long)tv.tv_sec * 1000 + tv.tv_usec / 1000;
}

static int hexval(int c)
{
    if (c >= '0' && c <= '9') return c - '0';
    if (c >= 'a' && c <= 'f') return c - 'a' + 10;
    if (c >= 'A' && c <= 'F') return c - 'A' + 10;
    return -1;
}

static void url_decode(const char *src, char *dst, size_t cap)
{
    size_t o = 0;
    for (const char *p = src; *p && o + 1 < cap; p++) {
        if (*p == '+') {
            dst[o++] = ' ';
        } else if (*p == '%' && p[1] && p[2]) {
            int hi = hexval((unsigned char)p[1]);
            int lo = hexval((unsigned char)p[2]);
            if (hi >= 0 && lo >= 0) {
                dst[o++] = (char)(hi * 16 + lo);
                p += 2;
            } else {
                dst[o++] = *p;
            }
        } else {
            dst[o++] = *p;
        }
    }
    dst[o] = '\0';
}

/* ---- JSON ---- */

static void json_escape(sbuf *b, const char *s, size_t n)
{
    sb_chr(b, '"');
    for (size_t i = 0; i < n; i++) {
        unsigned char c = (unsigned char)s[i];
        switch (c) {
        case '"': sb_str(b, "\\\""); break;
        case '\\': sb_str(b, "\\\\"); break;
        case '\n': sb_str(b, "\\n"); break;
        case '\r': sb_str(b, "\\r"); break;
        case '\t': sb_str(b, "\\t"); break;
        case '\b': sb_str(b, "\\b"); break;
        case '\f': sb_str(b, "\\f"); break;
        default:
            if (c < 0x20) {
                char tmp[8];
                snprintf(tmp, sizeof tmp, "\\u%04x", c);
                sb_str(b, tmp);
            } else {
                sb_chr(b, (char)c);
            }
        }
    }
    sb_chr(b, '"');
}

static void value_to_json(VM *vm, sbuf *b, Value v)
{
    switch (v.type) {
    case VAL_NULL: sb_str(b, "null"); break;
    case VAL_BOOL: sb_str(b, v.as.b ? "true" : "false"); break;
    case VAL_INT: {
        char tmp[32];
        snprintf(tmp, sizeof tmp, "%lld", (long long)v.as.i);
        sb_str(b, tmp);
        break;
    }
    case VAL_FLOAT: {
        char tmp[48];
        /* %g, matching value.c's json_append_value (and the host bridge):
         * %.17g round-trips exactly but prints 1234.56 as
         * 1234.5599999999999, which broke response parity with the host. */
        snprintf(tmp, sizeof tmp, "%g", v.as.n);
        sb_str(b, tmp);
        break;
    }
    case VAL_OBJ: {
        Obj *o = v.as.o;
        switch (o->type) {
        case OBJ_STRING:
            json_escape(b, obj_string(o), obj_string_len(o));
            break;
        case OBJ_MAP: {
            sb_chr(b, '{');
            for (int i = 0; i < o->as.map.count; i++) {
                if (i) sb_chr(b, ',');
                json_escape(b, o->as.map.keys[i], strlen(o->as.map.keys[i]));
                sb_chr(b, ':');
                value_to_json(vm, b, o->as.map.vals[i]);
            }
            sb_chr(b, '}');
            break;
        }
        case OBJ_LIST: {
            sb_chr(b, '[');
            for (int i = 0; i < o->as.list.count; i++) {
                if (i) sb_chr(b, ',');
                value_to_json(vm, b, o->as.list.items[i]);
            }
            sb_chr(b, ']');
            break;
        }
        default:
            sb_str(b, "null");
        }
        break;
    }
    default:
        sb_str(b, "null");
    }
    (void)vm;
}

/* ---- request object ---- */

/* Match a route pattern against a request path, agent-httpd style: literal
 * segments compare exactly, a ":name" segment captures the corresponding
 * request segment (URL-decoded). Returns a params map ({id: "1"} for
 * "/api/user/:id" vs "/api/user/1") or VAL_NULL when the pattern does not
 * match. The map is rooted while being filled. */
static Value match_dynamic_route(VM *vm, const char *pattern, const char *path)
{
    char pc[1024], qc[1024];
    snprintf(pc, sizeof pc, "%s", pattern);
    snprintf(qc, sizeof qc, "%s", path);

    Value pm = make_map(vm);
    vm_push(vm, pm);
    Obj *po = AS_OBJ(pm);
    int ok = 1;

    char *psave = NULL, *qsave = NULL;
#if defined(_WIN32)
    char *pt = strtok_s(pc, "/", &psave);
    char *qt = strtok_s(qc, "/", &qsave);
#else
    char *pt = strtok_r(pc, "/", &psave);
    char *qt = strtok_r(qc, "/", &qsave);
#endif
    while (ok) {
        if (!pt && !qt) break;              /* both exhausted: match */
        if (!pt || !qt) { ok = 0; break; }  /* segment count differs */
        if (pt[0] == ':') {
            char decoded[512];
            url_decode(qt, decoded, sizeof decoded);
            map_set(vm, po, pt + 1, make_string_cstr(vm, decoded));
        } else if (strcmp(pt, qt) != 0) {
            ok = 0;
            break;
        }
#if defined(_WIN32)
        pt = strtok_s(NULL, "/", &psave);
        qt = strtok_s(NULL, "/", &qsave);
#else
        pt = strtok_r(NULL, "/", &psave);
        qt = strtok_r(NULL, "/", &qsave);
#endif
    }
    vm_pop(vm);
    if (!ok) return val_null();
    return pm;
}

static Value make_req_map(VM *vm, const HttpReq *r, Value route_params)
{
    Value m = make_map(vm);
    Obj *o = AS_OBJ(m);
    map_set(vm, o, "method", make_string_cstr(vm, r->method));
    map_set(vm, o, "path", make_string_cstr(vm, r->path));
    map_set(vm, o, "query", make_string_cstr(vm, r->query));

    Value qp = make_map(vm);
    Obj *qpo = AS_OBJ(qp);
    if (r->query[0]) {
        char *copy = strdup(r->query);
        char *save = NULL;
#if defined(_WIN32)
        for (char *tok = strtok_s(copy, "&", &save); tok;
             tok = strtok_s(NULL, "&", &save)) {
#else
        for (char *tok = strtok_r(copy, "&", &save); tok;
             tok = strtok_r(NULL, "&", &save)) {
#endif
            char kb[1024], vb[1024];
            char *eq = strchr(tok, '=');
            if (eq) {
                *eq = '\0';
                url_decode(tok, kb, sizeof kb);
                url_decode(eq + 1, vb, sizeof vb);
            } else {
                url_decode(tok, kb, sizeof kb);
                vb[0] = '\0';
            }
            map_set(vm, qpo, kb, make_string_cstr(vm, vb));
        }
        free(copy);
    }
    map_set(vm, o, "query_params", qp);

    /* Dynamic route captures (:id etc.); an empty map on literal routes so
     * req.params always exists, mirroring the host bridge's shape. */
    if (IS_OBJ(route_params) && AS_OBJ(route_params)->type == OBJ_MAP)
        map_set(vm, o, "params", route_params);
    else {
        Value empty = make_map(vm);
        map_set(vm, o, "params", empty);
    }

    if (r->body_len)
        map_set(vm, o, "body", make_string(vm, r->body, r->body_len));
    if (r->label)
        map_set(vm, o, "label", make_string_cstr(vm, r->label));

    /* ---- sessions ----
     * Every request gets a session: reuse the one named by `lume_sid=<id>`
     * in the Cookie header when it exists in the table, otherwise create a
     * fresh id + empty session map. A fresh session sets vm->session_new so
     * the response can carry Set-Cookie. The table is VM-owned, so session
     * data persists across requests (and is GC-rooted via vm->session_table). */
    {
        const char *sid = NULL;
        char sidbuf[64] = {0};
        if (r->cookie[0]) {
            const char *p = strstr(r->cookie, "lume_sid=");
            if (p) {
                p += 9; /* "lume_sid=" */
                size_t n = strcspn(p, ";");
                if (n > 0 && n < sizeof sidbuf) {
                    memcpy(sidbuf, p, n);
                    sidbuf[n] = '\0';
                    sid = sidbuf;
                }
            }
        }

        if (sid && vm->session_table) {
            int found = 0;
            Value existing = map_get(vm, vm->session_table, sid, &found);
            if (found && IS_OBJ(existing) && AS_OBJ(existing)->type == OBJ_MAP) {
                map_set(vm, o, "session_id", make_string_cstr(vm, sid));
                map_set(vm, o, "session", existing);
                vm->session_new = false;
                snprintf(vm->session_id, sizeof vm->session_id, "%s", sid);
                return m;
            }
        }

        /* fresh session */
        if (!vm->session_table) {
            Value t = make_map(vm);
            vm_push(vm, t);
            vm->session_table = AS_OBJ(t);
            vm_pop(vm);
        }
        static long s_sess_counter;
        snprintf(vm->session_id, sizeof vm->session_id, "ls%lx%ld",
                 (unsigned long)serve_now_ms(),
                 (long)++s_sess_counter);
        Value fresh = make_map(vm);
        vm_push(vm, fresh);
        map_set(vm, vm->session_table, vm->session_id, fresh);
        vm_pop(vm);
        map_set(vm, o, "session_id", make_string_cstr(vm, vm->session_id));
        map_set(vm, o, "session", fresh);
        vm->session_new = true;
    }

    return m;
}

/* ---- HTTP plumbing ---- */

static const char *status_text(int code)
{
    switch (code) {
    case 200: return "OK";
    case 204: return "No Content";
    case 400: return "Bad Request";
    case 404: return "Not Found";
    case 500: return "Internal Server Error";
    default:  return "OK";
    }
}

/* CSP policy from `server { csp = "..." }`, CR/LF-validated at ingest
 * (a newline would split the response header = injection). Empty string
 * = built-in default "default-src 'self'" (send_resp). */
static char g_serve_csp[256];

static void send_resp(SockFd fd, int status, const char *ctype,
                      const char *body, size_t blen, const char *set_cookie)
{
    /* Default security headers on every response: serve() is a public-facing
     * HTTP surface, so nosniff / frame-deny / referrer-policy cost nothing
     * and harden every content type; CSP is harmless on API responses
     * (browsers only enforce it on documents) and right on HTML.
     * `server { csp = "..." }` replaces the CSP line (the host's
     * agent-httpd has the same knob on its agenthttpd_config; both reject
     * CR/LF at ingest so the value cannot split the header). */
    char hdr[2048];
    int n = snprintf(hdr, sizeof hdr,
                     "HTTP/1.1 %d %s\r\n"
                     "Content-Type: %s\r\n"
                     "Content-Length: %zu\r\n"
                     "Connection: close\r\n"
                     "X-Content-Type-Options: nosniff\r\n"
                     "X-Frame-Options: DENY\r\n"
                     "Referrer-Policy: no-referrer\r\n"
                     "Content-Security-Policy: %s\r\n",
                     status, status_text(status), ctype, blen,
                     g_serve_csp[0] ? g_serve_csp : "default-src 'self'");
    if (n > 0 && set_cookie && set_cookie[0]) {
        int n2 = snprintf(hdr + n, (size_t)sizeof hdr - (size_t)n,
                          "Set-Cookie: %s\r\n", set_cookie);
        if (n2 > 0) n += n2;
    }
    if (n > 0 && (size_t)n + 2 < sizeof hdr) {
        memcpy(hdr + n, "\r\n", 3);
        n += 2;
    }
    if (n > 0) sock_send(fd, hdr, (size_t)n);
    if (body && blen) sock_send(fd, body, blen);
}

/* ---- SSE (text/event-stream) -----------------------------------------
 * 为什么单独一个函数: SSE **不能有 Content-Length** —— 长度未知, 连接靠
 * "服务端写完后关闭" 或客户端断开结束。send_resp 写死了 Content-Length 与
 * Connection: close, 直接复用它会让客户端一直等一个永远读不满的 body。
 *
 * 这里发头之后立刻返回, 由调用方逐块写入(每块一次 send, 即一次 flush ——
 * TCP 上小包会被 Nagle 合并, 但 Nagle 最多延迟 40ms, 对"看到进度"足够了)。
 *
 * 安全头与 send_resp 保持一致: nosniff / frame-deny / referrer-policy。
 * 不设 CSP —— 事件流不是文档, CSP 对它无意义。
 * 额外加 X-Accel-Buffering: no, 让 nginx 不缓冲(否则整个流会被攒起来,
 * 等于没流式)。 */
static void send_sse_head(SockFd fd, const char *set_cookie)
{
    char hdr[1024];
    int n = snprintf(hdr, sizeof hdr,
                     "HTTP/1.1 200 OK\r\n"
                     "Content-Type: text/event-stream; charset=utf-8\r\n"
                     "Cache-Control: no-cache\r\n"
                     "X-Accel-Buffering: no\r\n"
                     "Connection: close\r\n"
                     "X-Content-Type-Options: nosniff\r\n"
                     "X-Frame-Options: DENY\r\n"
                     "Referrer-Policy: no-referrer\r\n");
    if (n > 0 && set_cookie && set_cookie[0]) {
        int n2 = snprintf(hdr + n, (size_t)sizeof hdr - (size_t)n,
                          "Set-Cookie: %s\r\n", set_cookie);
        if (n2 > 0) n += n2;
    }
    if (n > 0 && (size_t)n + 2 < sizeof hdr) {
        memcpy(hdr + n, "\r\n", 3);
        n += 2;
    }
    if (n > 0) sock_send(fd, hdr, (size_t)n);
}

/* 发一个 SSE 事件: "data: <json>\n\n"。json 里的换行必须已被序列化器转义
 * (
 变成两个字符), 否则一个事件会被拆成多行 —— 客户端解析出错。
 * 这里再做一次防御: 若真的含裸换行, 拆成多个 data: 行(RFC 允许)。 */
static void send_sse_event(SockFd fd, const char *data, size_t len)
{
    static const char prefix[] = "data: ";
    static const char nl = '\n';
    size_t start = 0;
    while (start <= len) {
        size_t end = start;
        while (end < len && data[end] != '\n') end++;
        sock_send(fd, prefix, sizeof prefix - 1);
        if (end > start) sock_send(fd, data + start, end - start);
        sock_send(fd, &nl, 1);
        sock_send(fd, &nl, 1);
        if (end >= len) break;
        start = end + 1;
    }
}

/* case-insensitive substring search (Content-Length lookup) */
static const char *ci_find(const char *hay, size_t hlen, const char *needle)
{
    size_t nlen = strlen(needle);
    if (nlen == 0 || hlen < nlen) return NULL;
    for (size_t i = 0; i + nlen <= hlen; i++) {
        size_t j = 0;
        while (j < nlen) {
            char a = hay[i + j], b = needle[j];
            if (a >= 'A' && a <= 'Z') a = (char)(a - 'A' + 'a');
            if (b >= 'A' && b <= 'Z') b = (char)(b - 'A' + 'a');
            if (a != b) break;
            j++;
        }
        if (j == nlen) return hay + i;
    }
    return NULL;
}

/* returns 0 on success; fills req. head may already contain part of body. */
static int parse_request(const char *head, size_t hlen, HttpReq *req)
{
    /* request line: METHOD SP PATH SP HTTP/1.1 CRLF */
    const char *sp1 = strchr(head, ' ');
    if (!sp1 || sp1 - head >= (long)sizeof req->method) return -1;
    size_t mlen = (size_t)(sp1 - head);
    memcpy(req->method, head, mlen);
    req->method[mlen] = '\0';

    const char *sp2 = strchr(sp1 + 1, ' ');
    if (!sp2) return -1;
    size_t plen = (size_t)(sp2 - (sp1 + 1));
    if (plen >= sizeof req->path) plen = sizeof req->path - 1;
    memcpy(req->path, sp1 + 1, plen);
    req->path[plen] = '\0';

    char *q = strchr(req->path, '?');
    if (q) {
        strncpy(req->query, q + 1, sizeof req->query - 1);
        req->query[sizeof req->query - 1] = '\0';
        *q = '\0';
    }

    /* Content-Length (headers end at the CRLFCRLF inside hlen) */
    const char *hdr_end = NULL;
    for (size_t i = 0; i + 3 < hlen; i++) {
        if (head[i] == '\r' && head[i + 1] == '\n' &&
            head[i + 2] == '\r' && head[i + 3] == '\n') {
            hdr_end = head + i;
            break;
        }
    }
    if (!hdr_end) return -1;
    size_t body_off = (size_t)(hdr_end - head) + 4;

    /* Accept header: 只用于判断能否回 SSE。取前 255 字节足够判前缀。 */
    const char *acc = ci_find(head, body_off, "accept:");
    if (acc) {
        acc += 7;
        while (*acc == ' ' || *acc == '\t') acc++;
        size_t alen = strcspn(acc, "\r\n");
        if (alen >= sizeof req->accept) alen = sizeof req->accept - 1;
        memcpy(req->accept, acc, alen);
        req->accept[alen] = '\0';
    } else {
        req->accept[0] = '\0';
    }

    /* Cookie header: raw value, capped (only lume_sid= is consumed later). */
    const char *ck = ci_find(head, body_off, "cookie:");
    if (ck) {
        ck += 7;
        while (*ck == ' ' || *ck == '\t') ck++;
        size_t cklen = strcspn(ck, "\r\n");
        if (cklen >= sizeof req->cookie) cklen = sizeof req->cookie - 1;
        memcpy(req->cookie, ck, cklen);
        req->cookie[cklen] = '\0';
    }

    const char *cl = ci_find(head, body_off, "content-length:");
    long clen = -1;
    if (cl) {
        cl += 15;
        while (*cl == ' ' || *cl == '\t') cl++;
        clen = strtol(cl, NULL, 10);
        if (clen < 0 || clen > (long)REQ_BUF_MAX) return -1;
    }
    if (clen > 0) {
        size_t have = hlen > body_off ? hlen - body_off : 0;
        if (have > (size_t)clen) have = (size_t)clen;
        memcpy(req->body, head + body_off, have);
        req->body_len = have;
    }
    return 0;
}

/* ---- static files (docroot) ---- */

/* extension compare, case-insensitive, no strcasecmp (mingw availability) */
static int ext_eq(const char *a, const char *b)
{
    while (*a && *b) {
        char ca = *a, cb = *b;
        if (ca >= 'A' && ca <= 'Z') ca = (char)(ca - 'A' + 'a');
        if (cb >= 'A' && cb <= 'Z') cb = (char)(cb - 'A' + 'a');
        if (ca != cb) return 0;
        a++; b++;
    }
    return *a == *b;
}

static const char *mime_of(const char *path)
{
    static const struct { const char *ext, *mime; } tab[] = {
        {".html", "text/html; charset=utf-8"},
        {".htm",  "text/html; charset=utf-8"},
        {".css",  "text/css; charset=utf-8"},
        {".js",   "text/javascript; charset=utf-8"},
        {".mjs",  "text/javascript; charset=utf-8"},
        {".json", "application/json; charset=utf-8"},
        {".svg",  "image/svg+xml"},
        {".png",  "image/png"},
        {".jpg",  "image/jpeg"},
        {".jpeg", "image/jpeg"},
        {".gif",  "image/gif"},
        {".ico",  "image/x-icon"},
        {".txt",  "text/plain; charset=utf-8"},
        {".xml",  "application/xml"},
        {".pdf",  "application/pdf"},
        {".woff", "font/woff"},
        {".woff2","font/woff2"},
        {".wasm", "application/wasm"},
        {NULL, NULL},
    };
    const char *dot = strrchr(path, '.');
    if (!dot) return "application/octet-stream";
    for (int i = 0; tab[i].ext; i++)
        if (ext_eq(dot, tab[i].ext)) return tab[i].mime;
    return "application/octet-stream";
}

#define STATIC_MAX (64 * 1024 * 1024)

/* Answer a route-miss GET from docroot. Returns 1 when the request has been
 * answered (200 with the file, or a 404: traversal attempt / missing file),
 * 0 when docroot is unset so the caller can emit its plain route 404.
 * The request path is URL-decoded BEFORE the ".." check, so %2e%2e cannot
 * slip past the traversal guard. Directory-shaped paths (trailing '/') map
 * to index.html inside that directory. */
static int serve_static(SockFd fd, const char *docroot, const char *path)
{
    if (!docroot || !docroot[0]) return 0;

    char rel[1024];
    url_decode(path, rel, sizeof rel);
    if (strstr(rel, "..")) { /* traversal: hard 404, never resolved */
        send_resp(fd, 404, "text/plain; charset=utf-8", "not found", 9, NULL);
        return 1;
    }
    size_t rlen = strlen(rel);
    if (rlen > 0 && rel[rlen - 1] == '/' && rlen + 11 < sizeof rel)
        strcpy(rel + rlen, "index.html");
    if (strcmp(rel, "/") == 0)
        snprintf(rel, sizeof rel, "/index.html");

    char full[2048];
    snprintf(full, sizeof full, "%s%s", docroot, rel);

    FILE *fp = fopen(full, "rb");
    if (!fp) {
        send_resp(fd, 404, "text/plain; charset=utf-8", "not found", 9, NULL);
        return 1;
    }
    if (fseek(fp, 0, SEEK_END) != 0) { fclose(fp); return 1; }
    long sz = ftell(fp);
    if (sz < 0 || sz > STATIC_MAX || fseek(fp, 0, SEEK_SET) != 0) {
        fclose(fp);
        send_resp(fd, 404, "text/plain; charset=utf-8", "not found", 9, NULL);
        return 1;
    }
    char *buf = malloc(sz > 0 ? (size_t)sz : 1);
    if (!buf) {
        fclose(fp);
        send_resp(fd, 500, "text/plain; charset=utf-8", "out of memory", 13,
                  NULL);
        return 1;
    }
    size_t got = fread(buf, 1, (size_t)sz, fp);
    fclose(fp);
    if (got != (size_t)sz) {
        free(buf);
        send_resp(fd, 404, "text/plain; charset=utf-8", "not found", 9, NULL);
        return 1;
    }
    send_resp(fd, 200, mime_of(full), buf, got, NULL);
    free(buf);
    return 1;
}

/* ---- entry point ---- */

void bridge_run(VM *vm)
{
    const char *host = "127.0.0.1";
    long port = 8082;
    const char *docroot = NULL;

    if (vm->server_config) {
        Obj *cfg = vm->server_config;
        for (int i = 0; i < cfg->as.map.count; i++) {
            const char *k = cfg->as.map.keys[i];
            Value v = cfg->as.map.vals[i];
            if (strcmp(k, "host") == 0 && IS_OBJ(v) &&
                AS_OBJ(v)->type == OBJ_STRING)
                host = obj_string(AS_OBJ(v));
            else if (strcmp(k, "port") == 0 && IS_INT(v))
                port = (long)v.as.i;
            else if (strcmp(k, "docroot") == 0 && IS_OBJ(v) &&
                     AS_OBJ(v)->type == OBJ_STRING)
                docroot = obj_string(AS_OBJ(v));
            else if (strcmp(k, "csp") == 0 && IS_OBJ(v) &&
                     AS_OBJ(v)->type == OBJ_STRING) {
                const char *c = obj_string(AS_OBJ(v));
                if (c[0] && !strpbrk(c, "\r\n"))
                    snprintf(g_serve_csp, sizeof g_serve_csp, "%s", c);
            }
        }
    }

    char portstr[16];
    snprintf(portstr, sizeof portstr, "%ld", port);

    struct addrinfo hints, *res = NULL;
    memset(&hints, 0, sizeof hints);
    hints.ai_family = AF_UNSPEC;
    hints.ai_socktype = SOCK_STREAM;
    hints.ai_flags = AI_PASSIVE;
#if defined(_WIN32)
    if (!wsock_start()) {
        fprintf(stderr, "lume: run(): WSAStartup failed\n");
        exit(1);
    }
#endif

    if (getaddrinfo(host, portstr, &hints, &res) != 0 || !res) {
        fprintf(stderr, "lume: run(): cannot resolve %s:%s\n", host, portstr);
        exit(1);
    }

#if defined(_WIN32)
    SockFd listener = INVALID_SOCKET;
#else
    SockFd listener = -1;
#endif
    for (struct addrinfo *r = res; r; r = r->ai_next) {
        listener = socket(r->ai_family, r->ai_socktype, r->ai_protocol);
        if (SOCK_BADP(listener)) continue;
        int one = 1;
        setsockopt(listener, SOL_SOCKET, SO_REUSEADDR,
                   (const char *)&one, sizeof one);
#if defined(_WIN32)
        if (bind(listener, r->ai_addr, (int)r->ai_addrlen) == 0 &&
#else
        if (bind(listener, r->ai_addr, (socklen_t)r->ai_addrlen) == 0 &&
#endif
            listen(listener, SERVE_BACKLOG) == 0)
            break;
        sock_close(listener);
#if defined(_WIN32)
        listener = INVALID_SOCKET;
#else
        listener = -1;
#endif
    }
    freeaddrinfo(res);

    if (SOCK_BADP(listener)) {
        fprintf(stderr, "lume: run(): cannot listen on %s:%ld (errno %d)\n",
                host, port, sock_err());
        exit(1);
    }
    fprintf(stderr, "lume: serving http://%s:%ld (pid %ld)\n",
            host, port, (long)lume_getpid());

    for (;;) {
        SockFd cfd = accept(listener, NULL, NULL);
        if (SOCK_BADP(cfd)) {
            if (sock_err() == SOCK_EINTR) continue;
            break;
        }

        HttpReq req;
        memset(&req, 0, sizeof req);
        vm->session_new = false; /* stale flag must not leak across requests */

        char head[HEAD_BUF_MAX];
        size_t hlen = 0;
        size_t scanned = 0;          /* bytes already checked for the terminator */
        int hdr_done = 0;
        while (hlen + 1 < sizeof head) {
            int n = sock_recv(cfd, head + hlen, sizeof head - hlen - 1);
            if (n <= 0) break;
            hlen += (size_t)n;
            head[hlen] = '\0';
            /* scan the new region for the header terminator; the marker may
             * sit in the middle of the buffer when a body follows it */
            size_t i = scanned;
            for (; i + 3 < hlen; i++) {
                if (head[i] == '\r' && head[i + 1] == '\n' &&
                    head[i + 2] == '\r' && head[i + 3] == '\n') {
                    hdr_done = 1;
                    break;
                }
            }
            scanned = hlen > 3 ? hlen - 3 : 0;
            if (hdr_done) break;
        }

        if (!hdr_done || parse_request(head, hlen, &req) != 0) {
            send_resp(cfd, 400, "text/plain; charset=utf-8",
                      "bad request", 11, NULL);
            sock_close(cfd);
            continue;
        }

        /* finish reading body if Content-Length exceeds what we already have */
        {
            long clen = -1;
            const char *cl = ci_find(head, hlen, "content-length:");
            if (cl) {
                cl += 15;
                while (*cl == ' ' || *cl == '\t') cl++;
                clen = strtol(cl, NULL, 10);
            }
            if (clen > 0 && req.body_len < (size_t)clen &&
                (size_t)clen <= sizeof req.body) {
                while (req.body_len < (size_t)clen) {
                    int n = sock_recv(cfd, req.body + req.body_len,
                                     REQ_BUF_MAX - req.body_len);
                    if (n <= 0) break;
                    req.body_len += (size_t)n;
                }
            }
        }

        RouteRec *match = NULL;
        for (int i = 0; i < vm->route_count && !match; i++) {
            RouteRec *r = &vm->routes[i];
            if (strcmp(r->method, req.method) == 0 &&
                strcmp(r->path, req.path) == 0)
                match = r;
        }

        /* Literal miss: retry with dynamic patterns ("/api/u/:id"). Exact
         * matches above always win; patterns are tried in registration
         * order, agent-httpd style. */
        Value route_params = val_null();
        if (!match) {
            for (int i = 0; i < vm->route_count && !match; i++) {
                RouteRec *r = &vm->routes[i];
                if (strcmp(r->method, req.method) != 0) continue;
                if (strchr(r->path, ':') == NULL) continue;
                Value pm = match_dynamic_route(vm, r->path, req.path);
                if (!IS_NULL(pm)) {
                    match = r;
                    route_params = pm;
                }
            }
        }

        if (!match) {
            /* Route miss: fall back to docroot static files for GET (the
             * host's agent-httpd docroot behaviour, now in-tree). */
            if (strcmp(req.method, "GET") == 0 &&
                serve_static(cfd, docroot, req.path)) {
                sock_close(cfd);
                continue;
            }
            send_resp(cfd, 404, "text/plain; charset=utf-8",
                      "not found", 9, NULL);
            sock_close(cfd);
            continue;
        }

        req.label = match->label;
        Value reqv = make_req_map(vm, &req, route_params);

        vm->error = false;
        vm->error_msg[0] = '\0';
        vm_push(vm, match->handler);
        vm_push(vm, reqv);
        call_function(vm, match->handler, 1);
        Value res = vm_pop(vm);

        if (vm->error) {
            send_resp(cfd, 500, "text/plain; charset=utf-8",
                      vm->error_msg[0] ? vm->error_msg : "internal error",
                      strlen(vm->error_msg[0] ? vm->error_msg : "internal error"),
                      NULL);
            vm->error = false;
            sock_close(cfd);
            continue;
        }

        /* A fresh session created by make_req_map must be handed to the
         * browser: Set-Cookie carries the new id so the next request can
         * present it and resume the session map. */
        const char *set_cookie = NULL;
        if (vm->session_new && vm->session_id[0]) {
            static char ckbuf[160];
            snprintf(ckbuf, sizeof ckbuf, "lume_sid=%s; Path=/; HttpOnly; SameSite=Lax",
                     vm->session_id);
            set_cookie = ckbuf;
        }

        /* ---- SSE: 路由返回 { sse: [...] } 且客户端接受 text/event-stream ----
         * 形状: { sse: [ "data-one", {k:v}, ... ] }
         *   - 数组每一项是一个事件; 字符串原样作为 data, 其它值 JSON 序列化
         *   - 框架逐条发送, 全部发完即关闭连接(SSE 靠关闭表示结束)
         *
         * 为什么要有前提条件: 同样的路由给浏览器(不带 Accept)或过会缓冲的
         * 中间层时, 流式反而会让页面"一直空白到结束"。所以只在客户端明确
         * 声明接受 text/event-stream 时才流; 否则退回普通 JSON 一次性返回,
         * 让调用方至少拿到完整数据而不是半个流。
         */
        int wants_sse = 0;
        const char *acc = req.accept;
        for (const char *p2 = acc; *p2; p2++) {
            if (ci_find(p2, strlen(p2), "text/event-stream")) { wants_sse = 1; break; }
        }
        Value sse_items = val_null();
        int has_sse = 0;
        if (IS_OBJ(res) && AS_OBJ(res)->type == OBJ_MAP) {
            int found = 0;
            sse_items = map_get(vm, AS_OBJ(res), "sse", &found);
            if (found && IS_OBJ(sse_items) && AS_OBJ(sse_items)->type == OBJ_LIST)
                has_sse = 1;
        }

        /* 流式分支: 发头 -> 逐条写事件 -> 关闭。之后不再走普通序列化。 */
        if (has_sse && wants_sse) {
            Obj *list = AS_OBJ(sse_items);
            send_sse_head(cfd, set_cookie);
            for (int i = 0; i < list->as.list.count; i++) {
                Value item = list->as.list.items[i];
                if (IS_OBJ(item) && AS_OBJ(item)->type == OBJ_STRING) {
                    Obj *it = AS_OBJ(item);
                    send_sse_event(cfd, obj_string(it), obj_string_len(it));
                } else {
                    sbuf jb = {0};
                    value_to_json(vm, &jb, item);
                    send_sse_event(cfd, jb.p ? jb.p : "null",
                                   jb.p ? jb.len : 4);
                    free(jb.p);
                }
            }
            sock_close(cfd);
            (void)g_serve_csp;
            continue;
        }

        sbuf b = {0};
        if (IS_OBJ(res) && AS_OBJ(res)->type == OBJ_STRING) {
            Obj *s = AS_OBJ(res);
            send_resp(cfd, 200, "text/html; charset=utf-8",
                      obj_string(s), obj_string_len(s), set_cookie);
        } else if (IS_NULL(res)) {
            send_resp(cfd, 204, "text/plain; charset=utf-8", NULL, 0, set_cookie);
        } else {
            value_to_json(vm, &b, res);
            send_resp(cfd, 200, "application/json; charset=utf-8",
                      b.p ? b.p : "null", b.len ? b.len : 4, set_cookie);
            free(b.p);
        }
        sock_close(cfd);
        (void)serve_now_ms;
    }

    sock_close(listener);
}

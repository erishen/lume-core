/* bridge_native.c — VM-free HTTP server runtime for the native backend.
 *
 * The interpreter serves routes through bridge_run() (bridge_serve.c),
 * which drives the whole VM. A natively-compiled program has no VM, so the
 * codegen lowers `server {}` / `get "..", handler` / `run()` onto this file:
 *
 *   lume_srv_init(host, port)         — server { host=..; port=.. }
 *   lume_srv_route(method, path, fn)  — get/post/... "path", handler
 *   lume_srv_run()                    — run(): blocking accept loop
 *
 * A handler is compiled by the codegen with a fixed FFI signature
 * (see codegen_stmt.c): void h(const SrvReq *req, char **out,
 * size_t *out_len, int *is_json). The codegen serialises string returns as
 * plain text (is_json=0) and map returns as JSON (is_json=1) by emitting
 * calls to the lume_srv_json_* helpers below; query params are looked up
 * through lume_srv_qp().
 *
 * SrvReq's field layout is a contract with the IR-side struct definition:
 * four i8* pointers followed by one i64, in this order.
 */

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

#if defined(_WIN32)
#include <winsock2.h>
#include <ws2tcpip.h>
#else
#include <errno.h>
#include <sys/types.h>
#include <sys/socket.h>
#include <netdb.h>
#include <netinet/in.h>
#include <unistd.h>
#endif

/* ---- platform socket glue: one SockFd type and a few macros so the accept
 * loop below reads the same on POSIX and winsock. winsock wants int buffer
 * lengths (our buffers are far below INT_MAX), reports errors through
 * WSAGetLastError(), and needs one WSAStartup before anything. ---- */
#if defined(_WIN32)
typedef SOCKET SockFd;
#define SOCK_BADP(f) ((f) == INVALID_SOCKET)
#define sock_close(f) closesocket(f)
#define sock_err() WSAGetLastError()
#define SOCK_EINTR WSAEINTR
#define sock_send(f, b, n) send((f), (const char *)(b), (int)(n), 0)
#define sock_recv(f, b, n) recv((f), (char *)(b), (int)(n), 0)
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
#else
typedef int SockFd;
#define SOCK_BADP(f) ((f) < 0)
#define sock_close(f) close(f)
#define sock_err() errno
#define SOCK_EINTR EINTR
#define sock_send(f, b, n) send((f), (b), (n), 0)
#define sock_recv(f, b, n) recv((f), (b), (n), 0)
#endif

#define NATIVE_ROUTES_MAX 32
#define REQ_BUF_MAX (1 << 20)
#define HEAD_BUF_MAX 16384
#define SERVE_BACKLOG 16

typedef struct {
    const char *method;
    const char *path;
    const char *query;
    const char *body;
    size_t body_len;
} SrvReq;

typedef void (*SrvHandler)(const SrvReq *req, char **out, size_t *out_len,
                           int *is_json);

typedef struct {
    char *method;
    char *path;
    SrvHandler handler;
} NativeRoute;

static NativeRoute g_routes[NATIVE_ROUTES_MAX];
static int g_route_count = 0;
static char g_host[256] = "127.0.0.1";
static long g_port = 8082;

void lume_srv_init(const char *host, long long port)
{
    if (host) {
        snprintf(g_host, sizeof g_host, "%s", host);
    }
    g_port = (long)port;
}

void lume_srv_route(const char *method, const char *path, SrvHandler h)
{
    if (g_route_count >= NATIVE_ROUTES_MAX || !method || !path) return;
    g_routes[g_route_count].method = strdup(method);
    g_routes[g_route_count].path = strdup(path);
    g_routes[g_route_count].handler = h;
    g_route_count++;
}

/* ---------------- URL decoding / query params ---------------- */

static int hexv(int c)
{
    if (c >= '0' && c <= '9') return c - '0';
    if (c >= 'a' && c <= 'f') return c - 'a' + 10;
    if (c >= 'A' && c <= 'F') return c - 'A' + 10;
    return -1;
}

static void urldecode(const char *src, char *dst, size_t cap)
{
    size_t o = 0;
    for (const char *p = src; *p && o + 1 < cap; p++) {
        if (*p == '+') {
            dst[o++] = ' ';
        } else if (*p == '%' && p[1] && p[2]) {
            int hi = hexv((unsigned char)p[1]);
            int lo = hexv((unsigned char)p[2]);
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

static char g_qpbuf[4096];
static char g_qpval[4096];

/* atoi for native handlers: parse an i8* as a base-10 integer. */
long long lume_srv_atoi(const char *s)
{
    if (!s) return 0;
    return (long long)strtoll(s, NULL, 10);
}

/* now(): current local time as a string, in a static buffer. */
const char *lume_bi_now(void)
{
    static char buf[64];
    time_t t = time(NULL);
    struct tm tmv;
#if defined(_WIN32)
    localtime_s(&tmv, &t);
#else
    localtime_r(&t, &tmv);
#endif
    strftime(buf, sizeof buf, "%Y-%m-%d %H:%M:%S", &tmv);
    return buf;
}

/* Look a key up in a raw query string ("a=1&b=x"). Single-threaded: the
 * result lives in a static buffer that the next call overwrites. */
const char *lume_srv_qp(const char *query, const char *key, const char *def)
{
    if (!query || !key) return def ? def : "";
    strncpy(g_qpbuf, query, sizeof g_qpbuf - 1);
    g_qpbuf[sizeof g_qpbuf - 1] = '\0';
    char *save = NULL;
#if defined(_WIN32)
    for (char *tok = strtok_s(g_qpbuf, "&", &save); tok;
         tok = strtok_s(NULL, "&", &save)) {
#else
    for (char *tok = strtok_r(g_qpbuf, "&", &save); tok;
         tok = strtok_r(NULL, "&", &save)) {
#endif
        char *eq = strchr(tok, '=');
        if (eq) *eq = '\0';
        if (strcmp(tok, key) == 0) {
            urldecode(eq ? eq + 1 : "", g_qpval, sizeof g_qpval);
            return g_qpval;
        }
    }
    return def ? def : "";
}

/* ---------------- JSON accumulation into *out ---------------- */

static void json_buf_grow(char **out, size_t *len, size_t *cap, size_t need)
{
    if (*cap && *cap - *len - 1 >= need) return;
    size_t nc = *cap ? *cap : 64;
    while (nc - *len - 1 < need) nc *= 2;
    char *np = (char *)realloc(*out, nc);
    if (!np) return;                    /* leak-safe enough for a demo server */
    *out = np;
    *cap = nc;
}

static void json_raw(char **out, size_t *len, size_t *cap,
                     const char *s, size_t n)
{
    json_buf_grow(out, len, cap, n);
    memcpy(*out + *len, s, n);
    *len += n;
    (*out)[*len] = '\0';
}

static void json_esc(char **out, size_t *len, size_t *cap, const char *s)
{
    json_buf_grow(out, len, cap, strlen(s) + 2);
    char *p = *out + *len;
    *p++ = '"';
    for (const char *q = s; *q; q++) {
        unsigned char c = (unsigned char)*q;
        switch (c) {
        case '"':  *p++ = '\\'; *p++ = '"';  break;
        case '\\': *p++ = '\\'; *p++ = '\\'; break;
        case '\n': *p++ = '\\'; *p++ = 'n';  break;
        case '\r': *p++ = '\\'; *p++ = 'r';  break;
        case '\t': *p++ = '\\'; *p++ = 't';  break;
        default:
            if (c < 0x20) {
                p += sprintf((char *)p, "\\u%04x", c);
            } else {
                *p++ = (char)c;
            }
        }
    }
    *p++ = '"';
    *len = (size_t)(p - *out);
    (*out)[*len] = '\0';
}

/* State lives in *out and *out_len; *cap is the grow bookkeeping. */
void lume_srv_json_begin(char **out, size_t *out_len)
{
    *out = (char *)malloc(2);
    if (*out) {
        (*out)[0] = '{';
        (*out)[1] = '\0';
    }
    *out_len = *out ? 1 : 0;
}

void lume_srv_json_key(char **out, size_t *out_len, const char *key)
{
    size_t cap = *out ? *out_len + 1 : 0;
    size_t len = *out_len;
    if (*out && len > 0 && (*out)[len - 1] != '{') {
        json_raw(out, &len, &cap, ",", 1);
    }
    json_esc(out, &len, &cap, key);
    json_raw(out, &len, &cap, ":", 1);
    *out_len = len;
    (void)out; (void)out_len;
}

void lume_srv_json_str(char **out, size_t *out_len, const char *v)
{
    size_t cap = *out ? *out_len + 1 : 0;
    size_t len = *out_len;
    json_esc(out, &len, &cap, v ? v : "");
    *out_len = len;
}

void lume_srv_json_int(char **out, size_t *out_len, long long v)
{
    size_t cap = *out ? *out_len + 1 : 0;
    size_t len = *out_len;
    char buf[32];
    int n = snprintf(buf, sizeof buf, "%lld", v);
    json_raw(out, &len, &cap, buf, (size_t)n);
    *out_len = len;
}

void lume_srv_json_bool(char **out, size_t *out_len, int v)
{
    size_t cap = *out ? *out_len + 1 : 0;
    size_t len = *out_len;
    json_raw(out, &len, &cap, v ? "true" : "false", v ? 4 : 5);
    *out_len = len;
}

void lume_srv_json_end(char **out, size_t *out_len)
{
    size_t cap = *out ? *out_len + 1 : 0;
    size_t len = *out_len;
    json_raw(out, &len, &cap, "}", 1);
    *out_len = len;
}

/* ---------------- HTTP plumbing ---------------- */

static const char *status_text(int code)
{
    switch (code) {
    case 200: return "OK";
    case 204: return "No Content";
    case 400: return "Bad Request";
    case 404: return "Not Found";
    default:  return "OK";
    }
}

static void send_resp(SockFd fd, int status, const char *ctype,
                      const char *body, size_t blen)
{
    char hdr[512];
    int n = snprintf(hdr, sizeof hdr,
                     "HTTP/1.1 %d %s\r\n"
                     "Content-Type: %s\r\n"
                     "Content-Length: %zu\r\n"
                     "Connection: close\r\n"
                     "\r\n",
                     status, status_text(status), ctype, blen);
    if (n > 0) sock_send(fd, hdr, (size_t)n);
    if (body && blen) sock_send(fd, body, blen);
}

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

void lume_srv_run(void)
{
#if defined(_WIN32)
    if (!wsock_start()) {
        fprintf(stderr, "lume: run(): WSAStartup failed\n");
        exit(1);
    }
#endif

    char portstr[16];
    snprintf(portstr, sizeof portstr, "%ld", g_port);

    struct addrinfo hints, *res = NULL;
    memset(&hints, 0, sizeof hints);
    hints.ai_family = AF_UNSPEC;
    hints.ai_socktype = SOCK_STREAM;
    hints.ai_flags = AI_PASSIVE;
    if (getaddrinfo(g_host, portstr, &hints, &res) != 0 || !res) {
        fprintf(stderr, "lume: run(): cannot resolve %s:%s\n", g_host, portstr);
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
                g_host, g_port, sock_err());
        exit(1);
    }
    fprintf(stderr, "lume: serving http://%s:%ld (native)\n", g_host, g_port);

    for (;;) {
        SockFd cfd = accept(listener, NULL, NULL);
        if (SOCK_BADP(cfd)) {
            if (sock_err() == SOCK_EINTR) continue;
            break;
        }

        char head[HEAD_BUF_MAX];
        size_t hlen = 0;
        size_t scanned = 0;
        int hdr_done = 0;
        while (hlen + 1 < sizeof head) {
            int n = sock_recv(cfd, head + hlen, sizeof head - hlen - 1);
            if (n <= 0) break;
            hlen += (size_t)n;
            head[hlen] = '\0';
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
        if (!hdr_done) {
            send_resp(cfd, 400, "text/plain; charset=utf-8", "bad request", 11);
            sock_close(cfd);
            continue;
        }

        /* request line: METHOD SP PATH SP HTTP/1.1 */
        char method[16] = {0}, path[1024] = {0}, query[1024] = {0};
        const char *sp1 = strchr(head, ' ');
        const char *sp2 = sp1 ? strchr(sp1 + 1, ' ') : NULL;
        if (!sp1 || !sp2) {
            send_resp(cfd, 400, "text/plain; charset=utf-8", "bad request", 11);
            sock_close(cfd);
            continue;
        }
        size_t mlen = (size_t)(sp1 - head);
        if (mlen >= sizeof method) mlen = sizeof method - 1;
        memcpy(method, head, mlen);
        method[mlen] = '\0';
        size_t plen = (size_t)(sp2 - (sp1 + 1));
        if (plen >= sizeof path) plen = sizeof path - 1;
        memcpy(path, sp1 + 1, plen);
        path[plen] = '\0';
        char *q = strchr(path, '?');
        if (q) {
            strncpy(query, q + 1, sizeof query - 1);
            query[sizeof query - 1] = '\0';
            *q = '\0';
        }

        /* body per Content-Length */
        static char body[REQ_BUF_MAX];
        size_t body_len = 0;
        size_t body_off = 0;
        for (size_t i = 0; i + 3 < hlen; i++) {
            if (head[i] == '\r' && head[i + 1] == '\n' &&
                head[i + 2] == '\r' && head[i + 3] == '\n') {
                body_off = i + 4;
                break;
            }
        }
        const char *cl = ci_find(head, body_off, "content-length:");
        long clen = -1;
        if (cl) {
            cl += 15;
            while (*cl == ' ' || *cl == '\t') cl++;
            clen = strtol(cl, NULL, 10);
            if (clen < 0 || clen > (long)REQ_BUF_MAX) clen = -1;
        }
        if (clen > 0) {
            size_t have = hlen > body_off ? hlen - body_off : 0;
            if (have > (size_t)clen) have = (size_t)clen;
            memcpy(body, head + body_off, have);
            body_len = have;
            while (body_len < (size_t)clen) {
                int n = sock_recv(cfd, body + body_len,
                                  (size_t)clen - body_len);
                if (n <= 0) break;
                body_len += (size_t)n;
            }
        }

        /* route match */
        SrvHandler h = NULL;
        for (int i = 0; i < g_route_count && !h; i++) {
            if (strcmp(g_routes[i].method, method) == 0 &&
                strcmp(g_routes[i].path, path) == 0)
                h = g_routes[i].handler;
        }
        if (!h) {
            send_resp(cfd, 404, "text/plain; charset=utf-8", "not found", 9);
            sock_close(cfd);
            continue;
        }

        SrvReq req;
        req.method = method;
        req.path = path;
        req.query = query;
        req.body = body;
        req.body_len = body_len;

        char *out = NULL;
        size_t out_len = 0;
        int is_json = 0;
        h(&req, &out, &out_len, &is_json);

        if (out) {
            size_t blen = out_len;
            if (out_len == (size_t)-1) {
                /* text return: codegen leaves the length to the runtime */
                blen = strlen(out);
                is_json = 0;
            }
            send_resp(cfd, 200,
                      is_json ? "application/json; charset=utf-8"
                              : "text/html; charset=utf-8",
                      out, blen);
            /* out is heap-allocated only for JSON buffers; text returns point
             * at string constants in the binary. */
            if (is_json) free(out);
        } else {
            send_resp(cfd, 204, "text/plain; charset=utf-8", NULL, 0);
        }
        sock_close(cfd);
    }

    sock_close(listener);
}

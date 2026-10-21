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
 * four i8* pointers (method, path, query, body), one i64 (body_len), then an
 * i8* user slot for the authenticated subject. The native server does not
 * authenticate yet, so user stays NULL; the slot exists so a handler that
 * reads req.user (abac.lume's route handlers do) compiles and runs the same
 * shape the interpreter gives it — an unauthenticated request is a NULL
 * subject, which the policy rejects by default.
 */

#include <ctype.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

#if defined(_WIN32)
#include <winsock2.h>
#include <ws2tcpip.h>
#include <fcntl.h>
#include <io.h>
#include <process.h>
#include <sys/stat.h>
#else
#include <errno.h>
#include <sys/file.h>
#include <sys/types.h>
#include <sys/socket.h>
#include <netdb.h>
#include <netinet/in.h>
#include <unistd.h>
#endif

/* ---- file-lock shims ---- */
/* Self-contained: the --compile-* link line pulls build/bridge_native.o in
 * but not os_win32.o, so lume_flock has to live here (LockFileEx on
 * Windows, flock(2) on POSIX). os_win32.c's copy stays for builtins_fs.c. */
#if defined(_WIN32)
#ifndef LOCK_SH
#define LOCK_SH 1
#define LOCK_EX 2
#define LOCK_UN 8
#define LOCK_NB 4
#endif
#include <windows.h>
static int lume_flock(int fd, int op)
{
    HANDLE h = (HANDLE)_get_osfhandle(fd);
    if (h == INVALID_HANDLE_VALUE) return -1;
    OVERLAPPED ov;
    memset(&ov, 0, sizeof ov);
    if (op & LOCK_UN)
        return UnlockFileEx(h, 0, MAXDWORD, MAXDWORD, &ov) ? 0 : -1;
    DWORD flags = (op & LOCK_EX) ? LOCKFILE_EXCLUSIVE_LOCK : 0;
    if (op & LOCK_NB) flags |= LOCKFILE_FAIL_IMMEDIATELY;
    if (LockFileEx(h, flags, 0, MAXDWORD, MAXDWORD, &ov)) return 0;
    if ((op & LOCK_NB) && GetLastError() == ERROR_LOCK_VIOLATION) return -1;
    return -1;
}
#else
static int lume_flock(int fd, int op) { return flock(fd, op); }
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

/* Compiled-backend route cap. Kept in lockstep with the interpreter's
 * MAX_AL_ROUTES (lume.h): a generated app that registers fine interpreted but
 * silently loses routes once compiled is the worst version of this bug. */
#define NATIVE_ROUTES_MAX 128
#define REQ_BUF_MAX (1 << 20)
#define HEAD_BUF_MAX 16384
#define SERVE_BACKLOG 16

typedef struct {
    const char *method;
    const char *path;
    const char *query;
    const char *body;
    size_t body_len;
    const char *user;   /* authenticated subject, NULL until native auth exists */
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
    /* Overflow used to be a silent drop: the route simply never matched, and
     * the only symptom was a 404 on a handler that plainly exists in source.
     * Warn on stderr instead — registration happens before the accept loop,
     * so this surfaces at startup, not mid-request. */
    if (g_route_count >= NATIVE_ROUTES_MAX) {
        fprintf(stderr,
                "lume: route table full (%d routes, NATIVE_ROUTES_MAX=%d); "
                "dropping %s %s\n",
                g_route_count, NATIVE_ROUTES_MAX,
                method ? method : "?", path ? path : "?");
        return;
    }
    if (!method || !path) return;
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

/* now(): epoch seconds, matching the interpreter's native_now (builtins.c).
 * The native backends emit this as an i64 — SPEC §6.2 spells now() as the
 * current time in seconds since the epoch, and abac's policy computes a
 * Beijing-hour gate from it. */
long long lume_bi_now(void)
{
    return (long long)time(NULL);
}

/* env(): host environment variable as a string; unset or credential-masked
 * names read as NULL. The masking promise mirrors the interpreter's
 * native_env (builtins_fs.c) so all three backends agree — kept in lockstep
 * with that file's keyword list. */
static int native_env_is_sensitive(const char *k) {
    static const char *const KW[] = {
        "API_KEY", "SECRET", "PASSWORD", "PASSWD", "TOKEN", "CREDENTIAL",
    };
    for (size_t i = 0; i < sizeof KW / sizeof KW[0]; i++) {
        size_t hn = strlen(k), nn = strlen(KW[i]);
        if (nn > hn) continue;
        for (size_t j = 0; j + nn <= hn; j++) {
            if (j > 0 && isalnum((unsigned char)k[j - 1])) continue;
            size_t m = 0;
            for (; m < nn; m++)
                if (tolower((unsigned char)k[j + m]) !=
                    tolower((unsigned char)KW[i][m]))
                    break;
            if (m == nn && (j + nn >= hn || !isalnum((unsigned char)k[j + nn])))
                return 1;
        }
    }
    return 0;
}

const char *lume_bi_env(const char *key)
{
    if (!key) return NULL;
    if (native_env_is_sensitive(key)) return NULL;
    return getenv(key);
}

/* ---- file builtins (SPEC §6.2 / interpreter parity with builtins_fs.c) ---- */

/* read_file(path): whole file as a string, or NULL when missing/unreadable.
 * 16 MiB cap, mirroring native_read_file. The result lives in a static
 * buffer: a second call overwrites it — the DSL reads one file at a time
 * (the audit trail in abac's policy is the canonical user). */
char *lume_bi_read_file(const char *path)
{
    static char buf[16u << 20];
    if (!path) return NULL;
    FILE *f = fopen(path, "rb");
    if (!f) return NULL;
    size_t n = fread(buf, 1, sizeof buf, f);
    fclose(f);
    if (n >= sizeof buf) return NULL;   /* too large, same as the interpreter */
    buf[n] = '\0';
    return buf;
}

/* write_file(path, data): atomically replace (tmp + rename); true on
 * success. Mirror of native_write_file, including the Windows rename quirk
 * (MSVCRT refuses to replace an existing file). */
long long lume_bi_write_file(const char *path, const char *data)
{
    if (!path || !data) return 0;
    char tmp[4096];
    if (strlen(path) + 32 >= sizeof tmp) return 0;
    snprintf(tmp, sizeof tmp, "%s.tmp.%d", path, (int)getpid());
    FILE *f = fopen(tmp, "wb");
    if (!f) return 0;
    size_t len = strlen(data);
    size_t wrote = len ? fwrite(data, 1, len, f) : 0;
    int ok = (fclose(f) == 0) && (wrote == len);
    if (ok) {
#if defined(_WIN32)
        remove(path);                  /* MSVCRT rename refuses to replace */
        ok = rename(tmp, path) == 0;
#else
        ok = rename(tmp, path) == 0;
#endif
    }
    if (!ok) remove(tmp);
    return ok ? 1 : 0;
}

/* Advisory file lock, one lock per process — the same contract as
 * native_lock_file: flock(2)-based on POSIX, LockFileEx via lume_flock on
 * Windows. Acquiring again replaces the previously-held lock; the lock dies
 * with the process (no stale lock files). */
static int g_lock_fd = -1;

long long lume_bi_lock_file(const char *path, long long wait_ms)
{
    if (!path) return 0;
    if (wait_ms < 0) wait_ms = 0;
    if (wait_ms > 30000) wait_ms = 30000;
    if (g_lock_fd >= 0) {
        lume_flock(g_lock_fd, LOCK_UN);
        close(g_lock_fd);
        g_lock_fd = -1;
    }
#if defined(_WIN32)
    int fd = _open(path, _O_CREAT | _O_RDWR, _S_IREAD | _S_IWRITE);
#else
    int fd = open(path, O_CREAT | O_RDWR, 0600);
#endif
    if (fd < 0) return 0;
    time_t t0 = time(NULL);
    for (;;) {
        if (lume_flock(fd, LOCK_EX | LOCK_NB) == 0) break;
#if defined(_WIN32)
        Sleep(25);   /* _sleep is deprecated under mingw-w64 */
#else
        usleep(25000);
#endif
        if (time(NULL) - t0 >= wait_ms / 1000 + 1) { close(fd); return 0; }
    }
    g_lock_fd = fd;
    return 1;
}

void lume_bi_unlock_file(void)
{
    if (g_lock_fd >= 0) {
        lume_flock(g_lock_fd, LOCK_UN);
        close(g_lock_fd);
        g_lock_fd = -1;
    }
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

void lume_srv_json_float(char **out, size_t *out_len, double v)
{
    size_t cap = *out ? *out_len + 1 : 0;
    size_t len = *out_len;
    char buf[64];
    int n = snprintf(buf, sizeof buf, "%g", v);
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

/* JSON array framing: the DSL emits lume_srv_json_arr_sep before each
 * element and the separator decides commas by an element counter. The
 * single-threaded server writes one response at a time; nested arrays
 * (depth > 1) are not produced by the DSL yet, so a plain counter is
 * enough. */
static int g_arr_items = 0;

void lume_srv_json_arr_begin(char **out, size_t *out_len)
{
    size_t cap = *out ? *out_len + 1 : 0;
    size_t len = *out_len;
    json_raw(out, &len, &cap, "[", 1);
    *out_len = len;
    g_arr_items = 0;
}

void lume_srv_json_arr_sep(char **out, size_t *out_len)
{
    if (g_arr_items++ == 0) return;     /* first element: nothing before it */
    size_t cap = *out ? *out_len + 1 : 0;
    size_t len = *out_len;
    json_raw(out, &len, &cap, ",", 1);
    *out_len = len;
}

void lume_srv_json_arr_end(char **out, size_t *out_len)
{
    size_t cap = *out ? *out_len + 1 : 0;
    size_t len = *out_len;
    json_raw(out, &len, &cap, "]", 1);
    *out_len = len;
    g_arr_items = 0;
}

/* Handler-less routes (`write "/items";`, `get "/x";`) acknowledge the
 * request as { action, method, got } — the native mirror of builtins.c's
 * native_default_route. action follows the write-group labels; a route
 * whose group carries no label (read) yields null. The native backends
 * reference this symbol directly when a route has no handler. */
void lume_srv_default_handler(const SrvReq *req, char **out, size_t *out_len,
                              int *is_json)
{
    const char *method = req && req->method ? req->method : "GET";
    const char *body = req && req->body ? req->body : "";
    const char *label = NULL;
    if (strcmp(method, "POST") == 0) label = "created";
    else if (strcmp(method, "PUT") == 0) label = "replaced";
    else if (strcmp(method, "PATCH") == 0) label = "patched";
    else if (strcmp(method, "DELETE") == 0) label = "deleted";

    *is_json = 1;
    lume_srv_json_begin(out, out_len);
    lume_srv_json_key(out, out_len, "action");
    if (label) {
        lume_srv_json_str(out, out_len, label);
    } else {
        /* No json helper for null; append it directly. */
        size_t cap = *out ? *out_len + 1 : 0;
        size_t len = *out_len;
        json_raw(out, &len, &cap, "null", 4);
        *out_len = len;
    }
    lume_srv_json_key(out, out_len, "method");
    lume_srv_json_str(out, out_len, method);
    lume_srv_json_key(out, out_len, "got");
    lume_srv_json_str(out, out_len, body);
    lume_srv_json_end(out, out_len);
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
    /* AF_INET, not AF_UNSPEC: on Windows, getaddrinfo with AI_PASSIVE +
     * AF_UNSPEC can hand back an IPv6 socket first, and binding the
     * IPv4 127.0.0.1 against it fails with WSAENOTSOCK (errno 10038).
     * IPv4-only keeps the accept loop's SrvReq contract unchanged. */
    hints.ai_family = AF_INET;
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
        req.user = NULL;   /* no native auth yet: an unauthenticated subject */

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

/* ---------------- SQLite (P6 three-backend parity, 2026-10-20) ----------------
 *
 * Native sql_query / sql_write / sql_tx_* — compiled only when the lume-core
 * build probed libsqlite3 (backend.c passes -DLUME_HAS_SQLITE=1 and the
 * SQLITE_PREFIX include path). Same semantics as the interpreter's
 * builtins_sql.c: query returns a list of row maps (rows capped at 200);
 * write goes through the same single-statement whitelist (sql_guard.h,
 * shared with the interpreter) and returns the affected count; tx_begin
 * opens READWRITE|CREATE + BEGIN and returns an opaque handle.
 *
 * Differences that are structural, not bugs: the handle is a plain heap
 * struct (no GC finalizer on the native side — commit/rollback free it; an
 * abandoned open transaction leaks until process exit, documented in
 * TODO.md P6). Errors flow through lume_error_set, the native twin of
 * vm_set_error: inside try() they become {ok: null, err: "..."} and the
 * sql builtin returns null/-1; outside try() they print and exit. */

#if LUME_HAS_SQLITE
#include <sqlite3.h>
#include <stdarg.h>
#include <stdint.h>
#include "sql_guard.h"

/* rt.c container/error helpers referenced by the native sql builtins. */
extern int64_t lume_list_len(const void *l);
extern int64_t lume_list_push_i(void *l, int64_t v);
extern int64_t lume_list_push_s(void *l, const char *v);
extern int64_t lume_list_push_f(void *l, double v);
extern int64_t lume_list_push_obj(void *l, void *v);
extern void *lume_list_new(void);
extern void *lume_map_new(void);
extern int64_t lume_map_put_i(void *m, const char *k, int64_t v);
extern int64_t lume_map_put_s(void *m, const char *k, const char *v);
extern int64_t lume_map_put_f(void *m, const char *k, double v);
extern int64_t lume_map_put_obj(void *m, const char *k, void *v);
extern const char *lume_list_param_cstr(const void *l, int64_t i);
extern void lume_error_set(const char *msg);

/* lume_error_set takes one plain string; the sql builtins want formatted
 * messages (the interpreter's vm_set_error does the formatting). */
static void lume_sql_errf(const char *fmt, ...)
{
    char buf[512];
    va_list ap;
    va_start(ap, fmt);
    vsnprintf(buf, sizeof buf, fmt, ap);
    va_end(ap);
    lume_error_set(buf);
}

#define SQL_ROW_CAP 200

typedef struct {
    sqlite3 *db;
    char *path;
    int state;   /* 0 open, 1 committed, 2 rolled back */
} LumeSqlTx;

/* Busy timeout: same environment knob and defaults as the interpreter's
 * builtins_sql.c. */
static int lume_sql_busy_timeout_ms(void)
{
    const char *e = getenv("LUME_SQLITE_BUSY_TIMEOUT_MS");
    if (!e || !e[0]) return 5000;
    char *end = NULL;
    long v = strtol(e, &end, 10);
    if (!end || *end != '\0' || v < 0) return 5000;
    if (v > 60000) v = 60000;
    return (int)v;
}

/* DSN semantics identical to the interpreter: bare path wins; empty/NULL
 * falls back to env SQLITE_DB. */
static const char *lume_sql_db_path(const char *db)
{
    if (db && db[0]) return db;
    const char *p = getenv("SQLITE_DB");
    return (p && p[0]) ? p : NULL;
}

/* One row -> one LumeMap; SQL NULL becomes a null OBJ slot (prints "null",
 * get() answers null — same as the interpreter's JSON round-trip). */
static void sql_row_to_map(sqlite3_stmt *st, void *m)
{
    int ncol = sqlite3_column_count(st);
    for (int i = 0; i < ncol; i++) {
        const char *key = sqlite3_column_name(st, i);
        if (!key) key = "col";
        switch (sqlite3_column_type(st, i)) {
        case SQLITE_INTEGER:
            lume_map_put_i(m, key, sqlite3_column_int64(st, i));
            break;
        case SQLITE_FLOAT:
            lume_map_put_f(m, key, sqlite3_column_double(st, i));
            break;
        case SQLITE_NULL:
            lume_map_put_obj(m, key, NULL);
            break;
        default: {
            /* sqlite3_column_text is a pointer into the statement's own
             * buffer: the next sqlite3_step (or finalize) reuses it, so the
             * map must take its own copy. map_put_s stores the pointer
             * verbatim (rt.c map_put), which is fine for literals but fatal
             * for volatile column text. */
            const char *txt = (const char *)sqlite3_column_text(st, i);
            lume_map_put_s(m, key, txt ? strdup(txt) : "");
            break;
        }
    }
    }
}

/* Bind a params LumeList to a prepared statement; the interpreter binds the
 * same values as text (SQLite affinity converts numbers). */
static void sql_bind_params(sqlite3_stmt *st, const void *params)
{
    int64_t n = params ? lume_list_len(params) : 0;
    for (int64_t i = 0; i < n; i++) {
        const char *v = lume_list_param_cstr(params, i);
        if (v)
            sqlite3_bind_text(st, (int)(i + 1), v, -1, SQLITE_TRANSIENT);
        else
            sqlite3_bind_null(st, (int)(i + 1));
    }
}

void *lume_bi_sql_query(const char *db, const char *sql, void *params)
{
    const char *path = lume_sql_db_path(db);
    if (!path) {
        lume_sql_errf("sql_query: no database (set SQLITE_DB or pass a path)");
        return NULL;
    }
    if (!sql || !sql[0]) { lume_error_set("sql_query: empty SQL"); return NULL; }
    char body[SQL_MAX];
    const char *verr = sql_validate_read(sql, body, sizeof body);
    if (verr) {
        lume_sql_errf("sql_query: %s", verr);
        return NULL;
    }
    sqlite3 *sq = NULL;
    if (sqlite3_open_v2(path, &sq, SQLITE_OPEN_READONLY, NULL) != SQLITE_OK) {
        lume_sql_errf("sql_query: cannot open '%s' read-only", path);
        if (sq) sqlite3_close(sq);
        return NULL;
    }
    sqlite3_busy_timeout(sq, lume_sql_busy_timeout_ms());
    sqlite3_stmt *st = NULL;
    if (sqlite3_prepare_v2(sq, body, -1, &st, NULL) != SQLITE_OK) {
        lume_sql_errf("sql_query: %s", sqlite3_errmsg(sq));
        sqlite3_close(sq);
        return NULL;
    }
    sql_bind_params(st, params);
    void *out = lume_list_new();
    while (lume_list_len(out) < SQL_ROW_CAP &&
           sqlite3_step(st) == SQLITE_ROW) {
        void *row = lume_map_new();
        sql_row_to_map(st, row);
        lume_list_push_obj(out, row);
    }
    sqlite3_finalize(st);
    sqlite3_close(sq);
    return out;
}

long long lume_bi_sql_write(const char *db, const char *sql, void *params)
{
    const char *path = lume_sql_db_path(db);
    if (!path) {
        lume_sql_errf("sql_write: no database (set SQLITE_DB or pass a path)");
        return -1;
    }
    if (!sql || !sql[0]) { lume_error_set("sql_write: empty SQL"); return -1; }
    char body[SQL_MAX];
    const char *verr = sql_validate_write(sql, body, sizeof body);
    if (verr) {
        lume_sql_errf("sql_write: %s", verr);
        return -1;
    }
    sqlite3 *sq = NULL;
    if (sqlite3_open_v2(path, &sq, SQLITE_OPEN_READWRITE, NULL) != SQLITE_OK) {
        lume_sql_errf("sql_write: cannot open '%s' for writing", path);
        if (sq) sqlite3_close(sq);
        return -1;
    }
    sqlite3_busy_timeout(sq, lume_sql_busy_timeout_ms());
    sqlite3_stmt *st = NULL;
    if (sqlite3_prepare_v2(sq, body, -1, &st, NULL) != SQLITE_OK) {
        lume_sql_errf("sql_write: %s", sqlite3_errmsg(sq));
        sqlite3_close(sq);
        return -1;
    }
    sql_bind_params(st, params);
    int rc = sqlite3_step(st);
    sqlite3_finalize(st);
    if (rc != SQLITE_DONE) {
        lume_sql_errf("sql_write: %s", sqlite3_errmsg(sq));
        sqlite3_close(sq);
        return -1;
    }
    long long affected = sqlite3_changes(sq);
    sqlite3_close(sq);
    return affected;
}

void *lume_bi_sql_tx_begin(const char *path)
{
    const char *p = lume_sql_db_path(path);
    if (!p) {
        lume_sql_errf("sql_tx_begin: no database (set SQLITE_DB or pass a path)");
        return NULL;
    }
    sqlite3 *sq = NULL;
    if (sqlite3_open_v2(p, &sq,
                        SQLITE_OPEN_READWRITE | SQLITE_OPEN_CREATE, NULL)
        != SQLITE_OK) {
        lume_sql_errf("sql_tx_begin: cannot open '%s'", p);
        if (sq) sqlite3_close(sq);
        return NULL;
    }
    sqlite3_busy_timeout(sq, lume_sql_busy_timeout_ms());
    char *m = NULL;
    if (sqlite3_exec(sq, "BEGIN", NULL, NULL, &m) != SQLITE_OK) {
        lume_sql_errf("sql_tx_begin: BEGIN: %s", m ? m : sqlite3_errmsg(sq));
        sqlite3_free(m);
        sqlite3_close(sq);
        return NULL;
    }
    LumeSqlTx *tx = (LumeSqlTx *)calloc(1, sizeof *tx);
    if (!tx) { sqlite3_close(sq); lume_error_set("sql_tx_begin: out of memory"); return NULL; }
    tx->db = sq;
    tx->path = strdup(p);
    tx->state = 0;
    return tx;
}

long long lume_bi_sql_tx_exec(void *tx, const char *sql, void *params)
{
    LumeSqlTx *t = (LumeSqlTx *)tx;
    if (!t || !t->db || t->state != 0) {
        lume_sql_errf("sql_tx_exec: transaction is already closed");
        return -1;
    }
    if (!sql || !sql[0]) { lume_error_set("sql_tx_exec: empty SQL"); return -1; }
    char body[SQL_MAX];
    const char *verr = sql_validate_write(sql, body, sizeof body);
    if (verr) {
        lume_sql_errf("sql_tx_exec: %s", verr);
        return -1;
    }
    sqlite3_stmt *st = NULL;
    if (sqlite3_prepare_v2(t->db, body, -1, &st, NULL) != SQLITE_OK) {
        lume_sql_errf("sql_tx_exec: %s", sqlite3_errmsg(t->db));
        return -1;
    }
    sql_bind_params(st, params);
    int rc = sqlite3_step(st);
    sqlite3_finalize(st);
    if (rc != SQLITE_DONE) {
        /* Transaction stays open; the caller decides commit vs rollback. */
        lume_sql_errf("sql_tx_exec: %s", sqlite3_errmsg(t->db));
        return -1;
    }
    return sqlite3_changes(t->db);
}

static long long sql_tx_end(void *tx, int commit)
{
    LumeSqlTx *t = (LumeSqlTx *)tx;
    if (!t || !t->db || t->state != 0) {
        lume_sql_errf("sql_tx_%s: transaction is already closed",
                       commit ? "commit" : "rollback");
        return 0;
    }
    char *m = NULL;
    if (sqlite3_exec(t->db, commit ? "COMMIT" : "ROLLBACK", NULL, NULL, &m)
        != SQLITE_OK) {
        lume_sql_errf("sql_tx_%s: %s", commit ? "commit" : "rollback",
                       m ? m : sqlite3_errmsg(t->db));
        sqlite3_free(m);
        return 0;
    }
    sqlite3_close(t->db);
    t->db = NULL;
    t->state = commit ? 1 : 2;
    free(t->path);
    free(t);
    return 1;
}

long long lume_bi_sql_tx_commit(void *tx) { return sql_tx_end(tx, 1); }
long long lume_bi_sql_tx_rollback(void *tx) { return sql_tx_end(tx, 0); }
#endif /* LUME_HAS_SQLITE */

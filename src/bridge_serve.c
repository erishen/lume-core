/* bridge_serve.c — bridge_run(): the language's own minimal HTTP server.
 *
 * In the host tree (work/research/lume + agent-httpd), bridge_run() starts
 * agent-httpd: epoll/kqueue, forked workers, static files, SSE, the whole
 * framework. This tree is the standalone compiler with no host library, so
 * bridge_run() used to refuse loudly. It now serves instead — a single-
 * threaded, sequential, shared-VM server that covers the language surface:
 *
 *   - `server { host, port }` selects the listen address (default
 *     127.0.0.1:8082). No workers: requests are handled one at a time on the
 *     same VM, so handlers keep their state across requests.
 *   - `route "GET", "/path", (req) => {...}` and the get/post/put/patch/
 *     delete/read/write sugar all dispatch here through vm->routes.
 *   - req carries path (full URI, query included), method, query (raw),
 *     query_params (URL-decoded map), body (string, null when absent) and
 *     label (the verb-group label, when the route was registered through a
 *     map group).
 *   - Responses follow the host semantics: a handler returning a string is
 *     served as text/html; a map without a `body` key is served as
 *     200 + application/json; a map with body/status/type is served exactly;
 *     a handler error becomes 500 + application/json; no route match is
 *     404 + application/json.
 *
 * Transport is net_compat.h (winsock on Windows, POSIX sockets elsewhere),
 * so this file is platform-neutral by construction.
 */

#include "net_compat.h"
#include "lume.h"

#include <ctype.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define SERVE_DEFAULT_PORT  8082
#define SERVE_IO_TIMEOUT_MS 10000L   /* per-connection request deadline */
#define SERVE_MAX_BODY      (1u << 20) /* request body cap: 1 MiB */
#define SERVE_LISTEN_BACKLOG 16

/* ---------------- small helpers ---------------- */

static int ascii_case_eq(const char *a, const char *b)
{
    for (;;) {
        unsigned char x = (unsigned char)*a++, y = (unsigned char)*b++;
        if (x >= 'A' && x <= 'Z') x += 'a' - 'A';
        if (y >= 'A' && y <= 'Z') y += 'a' - 'A';
        if (x != y) return 0;
        if (x == 0) return 1;
    }
}

/* URL-decode s[0..n) into out: '+' -> space, %XX -> byte, invalid escapes
 * pass through literally. Returns malloc'd NUL-terminated string. */
static char *url_decode(const char *s, size_t n)
{
    char *out = malloc(n + 1);
    if (!out) return NULL;
    size_t k = 0;
    for (size_t i = 0; i < n; i++) {
        char c = s[i];
        if (c == '+') { out[k++] = ' '; continue; }
        if (c == '%' && i + 2 < n && isxdigit((unsigned char)s[i + 1]) &&
            isxdigit((unsigned char)s[i + 2])) {
            int hi = isdigit((unsigned char)s[i + 1])
                         ? s[i + 1] - '0'
                         : (tolower((unsigned char)s[i + 1]) - 'a' + 10);
            int lo = isdigit((unsigned char)s[i + 2])
                         ? s[i + 2] - '0'
                         : (tolower((unsigned char)s[i + 2]) - 'a' + 10);
            out[k++] = (char)((hi << 4) | lo);
            i += 2;
            continue;
        }
        out[k++] = c;
    }
    out[k] = '\0';
    return out;
}

/* ---------------- connection reader ---------------- */

typedef struct {
    net_fd fd;
    char buf[8192];
    size_t len, pos;
    long deadline;
} ReqReader;

static void rr_init(ReqReader *rr, net_fd fd)
{
    rr->fd = fd;
    rr->len = rr->pos = 0;
    rr->deadline = net_now_ms() + SERVE_IO_TIMEOUT_MS;
}

/* Read one line up to and including '\n' into out (cap-1 bytes max, NUL
 * terminated, '\r' stripped). 0 = line read, 1 = EOF, -1 = error/timeout. */
static int rr_read_line(ReqReader *rr, char *out, size_t cap)
{
    for (;;) {
        for (size_t i = rr->pos; i < rr->len; i++) {
            if (rr->buf[i] == '\n') {
                size_t n = i - rr->pos;
                if (n + 1 >= cap) n = cap - 2;
                memcpy(out, rr->buf + rr->pos, n);
                out[n] = '\0';
                if (n > 0 && out[n - 1] == '\r') out[n - 1] = '\0';
                rr->pos = i + 1;
                return 0;
            }
        }
        if (rr->pos > 0) { /* compact */
            memmove(rr->buf, rr->buf + rr->pos, rr->len - rr->pos);
            rr->len -= rr->pos;
            rr->pos = 0;
        }
        long left = rr->deadline - net_now_ms();
        if (left <= 0) return -1;
        struct pollfd p = { .fd = (int)rr->fd, .events = POLLIN };
        int r = net_poll(&p, 1, (int)(left < 500 ? left : 500));
        if (r < 0) { if (NET_ERRNO() == NET_EINTR) continue; return -1; }
        if (r == 0) continue;
        if (p.revents & (POLLERR | POLLNVAL)) return -1;
        if (!(p.revents & POLLIN)) continue;
        if (rr->len >= sizeof rr->buf) return -1; /* line too long */
        int got = (int)recv(rr->fd, rr->buf + rr->len,
                            (int)(sizeof rr->buf - rr->len), 0);
        if (got < 0) {
            int e = NET_ERRNO();
            if (e == NET_EINTR || e == NET_EAGAIN || e == NET_EWOULDBLOCK) continue;
            return -1;
        }
        if (got == 0) return (rr->len == 0) ? 1 : -1; /* EOF mid-line */
        rr->len += (size_t)got;
    }
}

/* Read exactly want bytes into out. 0 = ok, 1 = EOF, -1 = error/timeout. */
static int rr_read_exact(ReqReader *rr, char *out, size_t want)
{
    size_t got = 0;
    while (got < want) {
        size_t avail = rr->len - rr->pos;
        if (avail > 0) {
            size_t n = want - got < avail ? want - got : avail;
            memcpy(out + got, rr->buf + rr->pos, n);
            rr->pos += n;
            got += n;
            continue;
        }
        long left = rr->deadline - net_now_ms();
        if (left <= 0) return -1;
        struct pollfd p = { .fd = (int)rr->fd, .events = POLLIN };
        int r = net_poll(&p, 1, (int)(left < 500 ? left : 500));
        if (r < 0) { if (NET_ERRNO() == NET_EINTR) continue; return -1; }
        if (r == 0) continue;
        if (!(p.revents & POLLIN)) continue;
        int got2 = (int)recv(rr->fd, rr->buf + rr->len,
                             (int)(sizeof rr->buf - rr->len), 0);
        if (got2 < 0) {
            int e = NET_ERRNO();
            if (e == NET_EINTR || e == NET_EAGAIN || e == NET_EWOULDBLOCK) continue;
            return -1;
        }
        if (got2 == 0) return 1; /* EOF */
        rr->len += (size_t)got2;
    }
    return 0;
}

/* ---------------- response writing ---------------- */

static int write_all(net_fd fd, const char *buf, size_t len)
{
    size_t off = 0;
    while (off < len) {
        int w = (int)send(fd, buf + off, (unsigned)(len - off), 0);
        if (w < 0) {
            int e = NET_ERRNO();
            if (e == NET_EINTR) continue;
            return -1;
        }
        if (w == 0) return -1;
        off += (size_t)w;
    }
    return 0;
}

static void serve_respond(net_fd fd, int status, const char *ctype,
                          const char *body, size_t body_len)
{
    const char *reason = "OK";
    if (status == 400) reason = "Bad Request";
    else if (status == 404) reason = "Not Found";
    else if (status == 413) reason = "Payload Too Large";
    else if (status == 500) reason = "Internal Server Error";

    char head[512];
    int n = snprintf(head, sizeof head,
                     "HTTP/1.1 %d %s\r\n"
                     "Content-Type: %s\r\n"
                     "Content-Length: %zu\r\n"
                     "Connection: close\r\n"
                     "\r\n",
                     status, reason, ctype, body_len);
    if (n < 0) return;
    if (write_all(fd, head, (size_t)n) != 0) return;
    if (body_len) write_all(fd, body, body_len);
}

/* ---------------- request handling ---------------- */

/* Build the req map passed to route handlers. Stack discipline: the caller
 * has pushed the handler (callee) first; the map is pushed here and LEFT on
 * the stack as the single argument, so the stack reads [callee, req] on
 * return and every value pushed along the way is a GC root. */
static void make_request(VM *vm, const char *method, const char *full_path,
                         size_t full_path_len, const char *raw_query,
                         size_t raw_query_len, const char *body, size_t body_len,
                         const char *label)
{
    Obj *req = AS_OBJ(make_map(vm));
    vm_push(vm, val_obj((Obj *)req)); /* arg slot; rooted until call */

    map_set(vm, req, "method", make_string(vm, method, strlen(method)));
    map_set(vm, req, "path", make_string(vm, full_path, full_path_len));

    if (raw_query_len) {
        map_set(vm, req, "query", make_string(vm, raw_query, raw_query_len));
    } else {
        map_set(vm, req, "query", val_null());
    }

    /* query_params: URL-decoded map; a segment without '=' gets an empty
     * value; repeated keys are last-wins; absent query is an empty map. */
    Obj *qp = AS_OBJ(make_map(vm));
    vm_push(vm, val_obj((Obj *)qp));
    if (raw_query_len) {
        size_t i = 0;
        while (i < raw_query_len) {
            size_t j = i;
            while (j < raw_query_len && raw_query[j] != '&') j++;
            size_t seg_n = j - i;
            if (seg_n > 0) {
                size_t eq = 0;
                while (eq < seg_n && raw_query[i + eq] != '=') eq++;
                char *k = url_decode(raw_query + i, eq);
                char *v = url_decode(raw_query + i + eq + (eq < seg_n ? 1 : 0),
                                     seg_n - eq - (eq < seg_n ? 1 : 0));
                if (k) {
                    map_set(vm, qp, k, v ? make_string_cstr(vm, v) : val_null());
                    free(k);
                }
                free(v);
            }
            i = j + 1;
        }
    }
    map_set(vm, req, "query_params", vm_pop(vm));

    if (body_len) map_set(vm, req, "body", make_string(vm, body, body_len));
    else map_set(vm, req, "body", val_null());

    if (label) map_set(vm, req, "label", make_string_cstr(vm, label));

    /* req stays on the stack as arg0; nothing to pop here */
}

/* Run the matched handler and write the response. Clears sticky VM errors
 * so the next request starts clean. */
static void serve_dispatch(VM *vm, net_fd conn, const char *method,
                           const char *full_path, size_t full_path_len,
                           const char *raw_query, size_t raw_query_len,
                           const char *body, size_t body_len)
{
    /* route match: method case-insensitive, path compared without query */
    const RouteRec *route = NULL;
    size_t path_n = full_path_len;
    if (raw_query_len) path_n = (size_t)(raw_query - full_path);
    for (int i = 0; i < vm->route_count; i++) {
        RouteRec *r = &vm->routes[i];
        if (!ascii_case_eq(r->method, method)) continue;
        if (strlen(r->path) != path_n ||
            memcmp(r->path, full_path, path_n) != 0)
            continue;
        route = r;
        break;
    }

    if (!route) {
        serve_respond(conn, 404, "application/json",
                      "{\"error\":\"not found\"}", 26);
        return;
    }

    /* handler call: stack is [callee, req] at this point */
    vm_push(vm, route->handler);
    make_request(vm, method, full_path, full_path_len,
                 raw_query, raw_query_len, body, body_len,
                 route->label);
    call_function(vm, route->handler, 1);
    Value result = vm_pop(vm);

    if (vm->error) {
        char buf[600];
        int n = snprintf(buf, sizeof buf, "{\"err\":\"%s\"}",
                         vm->error_msg[0] ? vm->error_msg : "handler error");
        serve_respond(conn, 500, "application/json", buf, (size_t)(n < 0 ? 0 : n));
        vm->error = false;
        vm->error_msg[0] = '\0';
        return;
    }

    /* string -> text/html; map without body -> JSON; map with body/status/
     * type -> exact response; anything else -> JSON. */
    sbuf b = {0};
    if (IS_OBJ(result) && AS_OBJ(result)->type == OBJ_STRING) {
        Obj *s = AS_OBJ(result);
        serve_respond(conn, 200, "text/html; charset=utf-8",
                      obj_string(s), s->as.str.len);
        return;
    }

    if (IS_OBJ(result) && AS_OBJ(result)->type == OBJ_MAP) {
        Obj *m = AS_OBJ(result);
        int found = 0;
        Value bv = map_get(vm, m, "body", &found);
        if (found && IS_OBJ(bv) && AS_OBJ(bv)->type == OBJ_STRING) {
            int status = 200;
            Value sv = map_get(vm, m, "status", &found);
            if (found && IS_NUM(sv)) status = (int)AS_NUM(sv);
            const char *ctype = "text/plain; charset=utf-8";
            Value tv = map_get(vm, m, "type", &found);
            if (found && IS_OBJ(tv) && AS_OBJ(tv)->type == OBJ_STRING)
                ctype = obj_string(AS_OBJ(tv));
            Obj *bs = AS_OBJ(bv);
            serve_respond(conn, status, ctype, obj_string(bs), bs->as.str.len);
            return;
        }
    }

    json_append_value(vm, &b, result);
    serve_respond(conn, 200, "application/json", b.p ? b.p : "", b.len);
    free(b.p);
}

static void serve_one(VM *vm, net_fd conn)
{
    net_set_nonblock(conn);
    ReqReader rr;
    rr_init(&rr, conn);

    char line[8192];
    if (rr_read_line(&rr, line, sizeof line) != 0)
        return; /* EOF / error / timeout: nothing to answer */
    if (line[0] == '\0') { /* stray CRLF before the request line */
        if (rr_read_line(&rr, line, sizeof line) != 0) return;
    }

    /* request line: METHOD SP PATH SP HTTP/x.y */
    char *sp1 = strchr(line, ' ');
    if (!sp1) { serve_respond(conn, 400, "application/json", "{\"error\":\"bad request\"}", 22); return; }
    char *path_start = sp1 + 1;
    char *sp2 = strchr(path_start, ' ');
    if (sp2) *sp2 = '\0';
    char method[64];
    size_t mlen = (size_t)(path_start - line - 1);
    if (mlen == 0 || mlen >= sizeof method) { serve_respond(conn, 400, "application/json", "{\"error\":\"bad request\"}", 22); return; }
    memcpy(method, line, mlen);
    method[mlen] = '\0';
    for (char *p = method; *p; p++) *p = (char)toupper((unsigned char)*p);

    /* headers until the blank line; find Content-Length */
    size_t content_length = 0;
    int have_cl = 0;
    for (;;) {
        if (rr_read_line(&rr, line, sizeof line) != 0) break;
        if (line[0] == '\0') break;
        if (strncasecmp(line, "Content-Length:", 15) == 0) {
            const char *v = line + 15;
            while (*v == ' ' || *v == '\t') v++;
            content_length = (size_t)strtoul(v, NULL, 10);
            have_cl = 1;
        }
    }

    if (have_cl && content_length > SERVE_MAX_BODY) {
        serve_respond(conn, 413, "application/json",
                      "{\"error\":\"payload too large\"}", 27);
        return;
    }

    char *body = NULL;
    if (have_cl && content_length > 0) {
        body = malloc(content_length + 1);
        if (!body) { serve_respond(conn, 500, "application/json", "{\"error\":\"oom\"}", 16); return; }
        int r = rr_read_exact(&rr, body, content_length);
        if (r != 0) { free(body); return; }
        body[content_length] = '\0';
    }

    /* query: everything after the first '?' in the path */
    const char *full_path = path_start;
    size_t full_path_len = strlen(path_start);
    const char *raw_query = NULL;
    size_t raw_query_len = 0;
    const char *qm = strchr(path_start, '?');
    if (qm) {
        raw_query = qm + 1;
        raw_query_len = full_path_len - (size_t)(raw_query - path_start);
    }

    serve_dispatch(vm, conn, method, full_path, full_path_len,
                   raw_query, raw_query_len, body ? body : "", body ? content_length : 0);
    free(body);
}

/* ---------------- entry ---------------- */

void bridge_run(VM *vm)
{
    net_init();

    int port = SERVE_DEFAULT_PORT;
    const char *host = "127.0.0.1";
    if (vm->server_config) {
        int found = 0;
        Value pv = map_get(vm, vm->server_config, "port", &found);
        if (found && IS_NUM(pv)) port = (int)AS_NUM(pv);
        Value hv = map_get(vm, vm->server_config, "host", &found);
        if (found && IS_OBJ(hv) && AS_OBJ(hv)->type == OBJ_STRING)
            host = obj_string(AS_OBJ(hv));
    }
    if (port <= 0 || port > 65535) {
        fprintf(stderr, "lume: run(): invalid port %d (server { port = N })\n",
                port);
        exit(2);
    }

    char portbuf[16];
    snprintf(portbuf, sizeof portbuf, "%d", port);
    struct addrinfo hints;
    memset(&hints, 0, sizeof hints);
    hints.ai_family = AF_UNSPEC;
    hints.ai_socktype = SOCK_STREAM;
    hints.ai_flags = AI_PASSIVE;
    struct addrinfo *res = NULL;
    if (getaddrinfo(host, portbuf, &hints, &res) != 0 || !res) {
        fprintf(stderr, "lume: run(): cannot resolve host '%s'\n", host);
        exit(2);
    }

    net_fd listener = NET_INVALID;
    for (struct addrinfo *r = res; r; r = r->ai_next) {
        listener = socket(r->ai_family, r->ai_socktype, r->ai_protocol);
        if (listener == NET_INVALID) continue;
        int one = 1;
        setsockopt(listener, SOL_SOCKET, SO_REUSEADDR,
                   (const char *)&one, sizeof one);
        if (bind(listener, r->ai_addr, (int)r->ai_addrlen) == 0 &&
            listen(listener, SERVE_LISTEN_BACKLOG) == 0)
            break;
        net_close(listener);
        listener = NET_INVALID;
    }
    freeaddrinfo(res);
    if (listener == NET_INVALID) {
        fprintf(stderr, "lume: run(): cannot listen on %s:%d (%s)\n", host,
                port, net_strerror(NET_ERRNO()));
        exit(2);
    }

    fprintf(stderr, "lume: listening on http://%s:%d (%d route%s)\n", host, port,
            vm->route_count, vm->route_count == 1 ? "" : "s");
    fflush(stderr);

    for (;;) {
        net_fd conn = accept(listener, NULL, NULL);
        if (conn == NET_INVALID) {
            int e = NET_ERRNO();
            if (e == NET_EINTR) continue;
            /* transient winsock errors (WSAECONNABORTED etc.) keep going */
            if (e == NET_EWOULDBLOCK || e == NET_EAGAIN) continue;
            fprintf(stderr, "lume: accept failed (%s), continuing\n",
                    net_strerror(e));
            continue;
        }
        serve_one(vm, conn);
        net_close(conn);
        /* sticky VM error from a handler must not leak into the next
         * request; serve_dispatch already cleared it, belt-and-braces. */
        vm->error = false;
        vm->error_msg[0] = '\0';
    }
}

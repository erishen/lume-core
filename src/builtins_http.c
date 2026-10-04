/* 出站 HTTP(S) 内建 —— http_get()。
 *
 * 本树是脱离 agent-httpd 的独立编译器,只链 libc(+ 可选 libLLVM),既没有
 * libcurl,也用不了 agent-httpd 那份 fork-curl 的 fetch_url(它落在子模块
 * 里、跨不过来)。所以传输层是裸 socket + 手写 HTTP/1.1:
 *
 *   http_get(url, {headers?, timeout?, max_bytes?, allow_private?})
 *     -> {ok: bool, status: int, body: string, err: string}
 *
 * 安全模型,与 agent 侧 fetch_url 同款(本树自己实现一遍,因为原版在子模块):
 *   - scheme 白名单 http/https;
 *   - 剥 userinfo,避免 http://evil@169.254.169.254/ 把真实目标藏在 '@' 后面;
 *   - 字面 IP 与 getaddrinfo 之后**每一个**解析结果都要过私有地址闸门
 *     (命中任意一个即拒 —— 只查"第一个"的话,Round-Robin DNS 换个答案
 *     就绕过去了);
 *   - 重定向逐跳再校验(最多 5 跳),代理隧道和 TLS 都不能跳过这一步;
 *   - body 上限(默认 16 KiB,与 fetch_url 一致)、整体超时(默认 10 s)。
 *
 * 默认**拒绝**私有/回环/链路本地/云元数据地址。allow_private 只给测试与
 * 内网脚本用,开了它本内建就挡不住指向本机的请求。
 *
 * 代理:跟随 https_proxy / http_proxy(大小写都认),LUME_HTTP_PROXY 强制覆盖
 * —— 直连 api.github.com 在国内基本不可用,这不是可选功能。http 走绝对 URI
 * 的 GET,https 走 CONNECT 隧道再开 TLS。
 *
 * TLS 依赖可选的 libssl(Makefile 用 pkg-config openssl 探测)。没装时
 * http_get 仍可用,但对 https:// 直接报 "needs libssl",不静默降明文。 */

#include "builtins_internal.h"
#include <ctype.h>
#include <openssl/err.h>
#include <fcntl.h>
#include <netinet/tcp.h>
#include <strings.h>
#include <time.h>

#if defined(HAVE_OPENSSL)
#include <openssl/ssl.h>
#endif

#include <arpa/inet.h>
#include <errno.h>
#include <netdb.h>
#include <netinet/in.h>
#include <poll.h>
#include <stdbool.h>
#include <string.h>
#include <sys/socket.h>
#include <sys/types.h>
#include <unistd.h>

/* opts.timeout 以秒为单位(脚本里写 30 表示 30 s),内部一律转毫秒。
 * 别把秒当毫秒用:那样 { timeout: 5 } 会在代理握手前就超时。 */
#define HTTP_DFLT_TIMEOUT_MS 10000L /* 与 fetch_url 的 10 s 窗口一致 */
#define HTTP_SEC_TO_MS(s)    ((long)((s) * 1000.0))
#define HTTP_TLS_TIMEOUT_MS  15000L /* 单次 TLS 握手的上限 */
#define HTTP_MAX_TIMEOUT_MS  60000L
#define HTTP_DFLT_MAX_BYTES  (16u * 1024u)
#define HTTP_MAX_MAX_BYTES   (8u << 20) /* 上限本身也要封顶 */
#define HTTP_MAX_REDIRECTS   5
#define HTTP_MAX_HEADER_SZ   (8u * 1024u)

/* ---- 私有地址判定(SSRF 闸门的地基) ---- */

/* RFC1918 + 回环 + 链路本地 + CGNAT + 保留段。true = 不该从服务端主动打过去。 */
static bool ipv4_is_private(const unsigned char b[4]) {
    if (b[0] == 10) return true;                                  /* 10/8      */
    if (b[0] == 127) return true;                                 /* 127/8     */
    if (b[0] == 169 && b[1] == 254) return true;                  /* 链路本地/元数据 */
    if (b[0] == 172 && b[1] >= 16 && b[1] <= 31) return true;     /* 172.16/12 */
    if (b[0] == 192 && b[1] == 168) return true;                  /* 192.168/16 */
    if (b[0] == 100 && (b[1] & 0xc0) == 64) return true;          /* CGNAT 100.64/10 */
    if (b[0] == 192 && b[1] == 0 && b[2] == 0) return true;       /* IETF 协议预留 */
    if (b[0] == 198 && (b[1] & 0xfe) == 18) return true;          /* 198.18/15 */
    if ((b[0] & 0xf0) == 0xe0) return true;                       /* 组播 + 保留 */
    if (b[0] == 255 && b[1] == 255 && b[2] == 255 && b[3] == 255) return true;
    return false;
}

static bool ip_is_private(const char *ip) {
    struct in_addr a4;
    if (inet_pton(AF_INET, ip, &a4) == 1)
        return ipv4_is_private((const unsigned char *)&a4);
    struct in6_addr a6;
    if (inet_pton(AF_INET6, ip, &a6) == 1) {
        const unsigned char *p = (const unsigned char *)&a6;
        bool all_zero = true;
        for (int i = 0; i < 8; i++) if (p[i]) { all_zero = false; break; }
        if (all_zero) return true;                                /* ::        */
        if (p[0] == 0x00 && p[1] == 0x01) return true;            /* ::1       */
        if ((p[0] & 0xfe) == 0xfc) return true;                   /* fc00::/7 ULA */
        if ((p[0] & 0xfe) == 0xfe) return true;                   /* fe80::/10、ff00::/8 */
        /* IPv4-mapped (::ffff:10.0.0.1): 内嵌的四段才是真相。 */
        if (p[0] == 0 && p[1] == 0 && p[2] == 0 && p[3] == 0 &&
            p[5] == 0xff && p[6] == 0xff)
            return ipv4_is_private(p + 12);
        return false;
    }
    return true; /* 认不出来的形式一律按危险处理(fail closed) */
}

/* 解析后逐个 IP 判;任一个命中私有即拒绝。 */
static bool host_resolves_private(const char *host) {
    struct addrinfo hints;
    memset(&hints, 0, sizeof hints);
    hints.ai_family = AF_UNSPEC;
    hints.ai_socktype = SOCK_STREAM;
    hints.ai_flags = AI_ADDRCONFIG;
    struct addrinfo *res = NULL;
    /* 解析失败 = 打不出去,直接放行(调用方会去连、连不上自然报错);
     * 拿到 0 条目同样放行(连不出去,不等于目标私有)——闸门只拦「私有」,
     * 不拦「不可达」,否则离线环境里所有公网请求都会被误杀成 SSRF。
     * service 必须给非空串:node=NULL 时 getaddrinfo 会 EAI_NONAME,
     * 而 host 常常是 IP 字面量(如代理 127.0.0.1),不能只当域名走。 */
    if (getaddrinfo(host, "0", &hints, &res) != 0 || !res) return false;
    bool hit = false;
    for (struct addrinfo *r = res; r; r = r->ai_next) {
        char buf[INET6_ADDRSTRLEN];
        if (!r->ai_addr) continue;
        if (r->ai_family == AF_INET) {
            hit = true;
            if (inet_ntop(AF_INET, &((struct sockaddr_in *)r->ai_addr)->sin_addr, buf,
                          sizeof buf))
                if (ip_is_private(buf)) { freeaddrinfo(res); return true; }
        } else if (r->ai_family == AF_INET6) {
            hit = true;
            if (inet_ntop(AF_INET6, &((struct sockaddr_in6 *)r->ai_addr)->sin6_addr, buf,
                          sizeof buf))
                if (ip_is_private(buf)) { freeaddrinfo(res); return true; }
        }
    }
    freeaddrinfo(res);
    /* 逐条都看过且都不私有 => 放行;只有一条地址都没拿到(极端情况)才拒。 */
    return !hit;
}

/* ---- URL 解析 ---- */

typedef struct {
    const char *host;   /* 指向 hostbuf */
    const char *path;   /* 指向原串,以 '/' 开头 */
    unsigned port;
    int https;
} Url;

static unsigned parse_port(const char *s, unsigned dflt) {
    if (!s || !*s) return dflt;
    char *end = NULL;
    long v = strtol(s, &end, 10);
    if (end == s || v <= 0 || v > 65535) return 0;
    return (unsigned)v;
}

/* 0 成功,-1 不可解析。host 写进调用方的 hostbuf。 */
static int url_parse(const char *url, char *hostbuf, size_t hostsz, Url *u) {
    const char *rest;
    if (strncasecmp(url, "http://", 7) == 0)       { u->https = 0; u->port = 80;  rest = url + 7; }
    else if (strncasecmp(url, "https://", 8) == 0) { u->https = 1; u->port = 443; rest = url + 8; }
    else return -1;

    size_t seg = 0;
    while (rest[seg] && rest[seg] != '/' && rest[seg] != '?' && rest[seg] != '#') seg++;
    const char *auth = rest, *auth_end = rest + seg;
    for (const char *p = auth; p < auth_end; p++)
        if (*p == '@') { auth = p + 1; break; } /* 剥 userinfo */
    const size_t alen = (size_t)(auth_end - auth);
    u->path = auth_end;
    if (*u->path == '\0') u->path = "/"; /* 裸主机名 => 根路径 */

    if (auth[0] == '[') { /* IPv6 字面量:']' 之前都是 host */
        const char *cb = memchr(auth, ']', alen);
        if (!cb) return -1;
        size_t hlen = (size_t)(cb + 1 - auth);
        if (hlen == 0 || hlen >= hostsz) return -1;
        memcpy(hostbuf, auth, hlen);
        hostbuf[hlen] = '\0';
        u->host = hostbuf;
        if (cb[1] == ':') {
            u->port = parse_port(cb + 2, u->port);
            if (!u->port) return -1;
        }
        return 0;
    }
    size_t i = 0;
    while (i < alen && auth[i] != ':') i++;
    if (i == 0 || i >= hostsz) return -1;
    memcpy(hostbuf, auth, i);
    hostbuf[i] = '\0';
    u->host = hostbuf;
    if (i < alen) {
        u->port = parse_port(auth + i + 1, u->port);
        if (!u->port) return -1;
    }
    return 0;
}

static bool host_blocked(const char *host) {
    struct in_addr a4;
    struct in6_addr a6;
    if (inet_pton(AF_INET, host, &a4) == 1) return ip_is_private(host);
    if (inet_pton(AF_INET6, host, &a6) == 1) return ip_is_private(host);
    return host_resolves_private(host); /* 域名:解析后逐个判 */
}

/* 把 Location 解析成绝对 URL(只处理绝对 URL、"//host/x"、"/x" 三种);
 * 命中返回 1,其余返回 0(调用方据此报错,不做猜测式拼接)。 */
static int resolve_redirect(const char *base, const char *loc, char *out, size_t outsz) {
    if (strncasecmp(loc, "http://", 7) == 0 || strncasecmp(loc, "https://", 8) == 0) {
        size_t n = strlen(loc);
        if (n == 0 || n >= outsz) return 0;
        memcpy(out, loc, n + 1);
        return 1;
    }
    Url b;
    char host[512];
    if (url_parse(base, host, sizeof host, &b) != 0) return 0;
    const char *scheme = b.https ? "https:" : "http:";
    if (loc[0] == '/' && loc[1] == '/') { /* "//host/path" */
        const char *slash = strchr(loc + 2, '/');
        size_t hl = slash ? (size_t)(slash - loc) : strlen(loc);
        if (hl == 0 || strlen(scheme) + hl >= outsz) return 0;
        memcpy(out, scheme, strlen(scheme));
        memcpy(out + strlen(scheme), loc, hl);
        out[strlen(scheme) + hl] = '\0';
        return 1;
    }
    if (loc[0] == '/') { /* 只换路径 */
        const char *slash = strchr(base, '/');
        if (!slash) return 0;
        size_t blen = (size_t)(slash - base);
        if (blen + strlen(loc) >= outsz) return 0;
        memcpy(out, base, blen);
        memcpy(out + blen, loc, strlen(loc) + 1);
        return 1;
    }
    return 0;
}

/* 释放并复位:只 free 不复位,下一跳/收尾时会再 free 一次(双重释放直接 abort)。 */
static void body_done(sbuf *b) {
    free(b->p);
    b->p = NULL; b->len = 0; b->cap = 0;
}

/* ---- 连接助手:非阻塞 fd + poll,整体 deadline ---- */

typedef struct {
    int fd;   /* 底层 socket;TLS 时它仍是真 fd */
    SSL *ssl; /* NULL => 明文 */
    char why[160]; /* 连接失败原因,给调用方拼错误信息 */
} Conn;

static long now_ms(void) {
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return (long)ts.tv_sec * 1000L + ts.tv_nsec / 1000000L;
}

static int wait_writable(int fd, long deadline) {
    for (;;) {
        long left = deadline - now_ms();
        if (left <= 0) return -2; /* timeout */
        struct pollfd p = { .fd = fd, .events = POLLOUT };
        int r = poll(&p, 1, (int)(left < 500 ? left : 500));
        /* EINTR 是 poll 的常见返回(定时器/子进程信号):重算剩余时间继续等,
         * 别把它当成「连不上」。 */
        if (r < 0) { if (errno == EINTR) continue; return -1; }
        if (r == 0) continue;
        if (p.revents & (POLLERR | POLLNVAL)) return -1;
        if (p.revents & POLLOUT) return 0;
    }
}

static int wait_readable(int fd, long deadline) {
    for (;;) {
        long left = deadline - now_ms();
        if (left <= 0) return -2;
        struct pollfd p = { .fd = fd, .events = POLLIN };
        int r = poll(&p, 1, (int)(left < 500 ? left : 500));
        /* 同 wait_writable:EINTR 重来,不算超时。 */
        if (r < 0) { if (errno == EINTR) continue;
                     fprintf(stderr, "[dbg] wait_readable poll r=%d errno=%d (%s)\n", r, errno, strerror(errno));
                     return -1; }
        if (r == 0) continue;
        if (p.revents & (POLLERR | POLLHUP | POLLNVAL)) return 0;
        if (p.revents & POLLIN) return 0;
    }
}

static int conn_write(Conn *c, const char *buf, size_t len, long deadline) {
    size_t off = 0;
    while (off < len) {
        if (c->ssl) {
            int w = SSL_write(c->ssl, buf + off, (int)(len - off));
            if (w < 0) {
                int e = SSL_get_error(c->ssl, w);
                if (e == SSL_ERROR_WANT_WRITE) { if (wait_writable(c->fd, deadline) != 0) return -2; continue; }
                if (e == SSL_ERROR_WANT_READ)  { if (wait_readable(c->fd, deadline) != 0) return -2; continue; }
                return -1;
            }
            if (w == 0) return -1;
            off += (size_t)w;
        } else {
            if (wait_writable(c->fd, deadline) != 0) return -2;
            ssize_t w = send(c->fd, buf + off, len - off, 0);
            if (w < 0) { if (errno == EINTR) continue; return -1; }
            if (w == 0) return -1;
            off += (size_t)w;
        }
    }
    return 0;
}

/* 读一次,got 为实际字节数(0 = EOF)。 */
static int conn_read_some(Conn *c, char *buf, size_t n, size_t *got, long deadline) {
    *got = 0;
    for (;;) {
        if (c->ssl) {
            int r = SSL_read(c->ssl, buf, (int)n);
            if (r > 0) { *got = (size_t)r; return 0; }
            int e = SSL_get_error(c->ssl, r);
            if (e == SSL_ERROR_WANT_READ)  { if (wait_readable(c->fd, deadline) != 0) return -2; continue; }
            if (e == SSL_ERROR_WANT_WRITE) { if (wait_writable(c->fd, deadline) != 0) return -2; continue; }
            return -1;
        }
        if (wait_readable(c->fd, deadline) != 0) return -2;
        ssize_t r = recv(c->fd, buf, n, 0);
        if (r > 0) { *got = (size_t)r; return 0; }
        if (r == 0) return 0; /* EOF */
        if (errno == EINTR || errno == EAGAIN || errno == EWOULDBLOCK) continue;
        return -1;
    }
}

/* 读满 want 字节或 EOF;got 为已收到字节数。 */
static int conn_read_exact(Conn *c, char *buf, size_t want, size_t *got, long deadline) {
    size_t off = 0;
    *got = 0;
    while (off < want) {
        size_t g = 0;
        int r = conn_read_some(c, buf + off, want - off, &g, deadline);
        if (r != 0) return r;
        if (g == 0) break; /* EOF */
        off += g;
        *got = off;
    }
    return 0;
}

static void conn_close(Conn *c) {
    if (c->ssl) { SSL_free(c->ssl); c->ssl = NULL; }
    if (c->fd >= 0) { close(c->fd); c->fd = -1; }
}

#if defined(HAVE_OPENSSL)
/* 证书模式在 CTX 上只装配一次:VERIFY_PEER + 系统 CA 路径。
 * 不校验证书等于在 TLS 上再挖个坑 —— 谁都能做中间人。CA 缺失本来就该响,
 * 让 SSL_do_handshake 吵出来,而不是悄悄放行。 */
static SSL_CTX *g_ssl_ctx;
static int ssl_ctx_ready(void) {
    if (g_ssl_ctx) return 0;
    SSL_CTX *ctx = SSL_CTX_new(TLS_client_method());
    if (!ctx) return -1;
    SSL_CTX_set_mode(ctx, SSL_MODE_ENABLE_PARTIAL_WRITE | SSL_MODE_AUTO_RETRY);
    SSL_CTX_set_default_verify_paths(ctx);
    SSL_CTX_set_verify(ctx, SSL_VERIFY_PEER, NULL);
    g_ssl_ctx = ctx;
    return 0;
}
#endif

/* 挂 TLS。返回 0 成功,1 = 本构建没有 libssl,-1 = 握手/证书失败。
 * SNI 必须设,否则证书校验拿不到对得上的名字。 */
/* deadline 是绝对时刻:等待函数都按「deadline - now_ms()」算剩余时间,
 * 传相对值(比如 15000)会立刻被判成超时。 */
static int tls_attach(Conn *c, const char *sni_host, long deadline) {
#if !defined(HAVE_OPENSSL)
    (void)c; (void)sni_host;
    return 1;
#else
    if (ssl_ctx_ready() != 0) return -1;
    SSL *ssl = SSL_new(g_ssl_ctx);
    if (!ssl) return -1;
    SSL_set_fd(ssl, c->fd);
    SSL_set_tlsext_host_name(ssl, sni_host);
    SSL_set_connect_state(ssl);
    for (;;) {
        int r = SSL_do_handshake(ssl);
        if (r == 1) break;
        /* 握手不能再超出整体 deadline,但也不让单步被掐死:取两者较小值。 */
        long left = deadline - now_ms();
        if (left <= 0) left = HTTP_TLS_TIMEOUT_MS;
        if (left > HTTP_TLS_TIMEOUT_MS) left = HTTP_TLS_TIMEOUT_MS;
        long step = now_ms() + left;
        int e = SSL_get_error(ssl, r);
        if (e == SSL_ERROR_WANT_READ)  { if (wait_readable(c->fd, step) != 0) { SSL_free(ssl); return -1; } continue; }
        if (e == SSL_ERROR_WANT_WRITE) { if (wait_writable(c->fd, step) != 0) { SSL_free(ssl); return -1; } continue; }
        { unsigned long le = ERR_get_error(); char es[256] = "";
          ERR_error_string(le, es);
          fprintf(stderr, "[dbg] tls handshake failed code=%d err=%lu (%s)\n", e, le, es); }        SSL_free(ssl);
        return -1;
    }
    c->ssl = ssl;
    return 0;
#endif
}

static int connect_with_deadline(int fd, const struct sockaddr *sa, socklen_t len,
                                 long deadline) {
    if (connect(fd, sa, len) == 0) return 0;
    if (errno != EINPROGRESS) return -1;
    for (;;) {
        long left = deadline - now_ms();
        if (left <= 0) return -1;
        struct pollfd p = { .fd = fd, .events = POLLOUT };
        int r = poll(&p, 1, (int)(left < 500 ? left : 500));
        if (r < 0) return -1;
        if (r == 0) continue;
        int soerr = 0;
        socklen_t sl = sizeof soerr;
        if (getsockopt(fd, SOL_SOCKET, SO_ERROR, &soerr, &sl) != 0) return -1;
        if (soerr != 0) return -1;
        return 0;
    }
}

static const char *proxy_for(int https) {
    const char *p = getenv("LUME_HTTP_PROXY");
    if (!p || !p[0]) p = getenv(https ? "https_proxy" : "http_proxy");
    if (!p || !p[0]) p = getenv(https ? "HTTPS_PROXY" : "HTTP_PROXY");
    return p;
}

/* 连上(必要时先开 CONNECT 隧道)。返回 0 成功,1 = 需要 libssl,-1 = 连不上。 */
static int open_conn(Url *u, const char *proxy, Conn *c, long deadline) {
    c->fd = -1;
    c->ssl = NULL;

    const char *chost = u->host;
    unsigned cport = u->port;
    int via_proxy = 0;
    if (proxy && proxy[0]) {
        /* 代理可能写成 host:port 或 http://host:port */
        char phost[512];
        const char *p = proxy;
        if (strncasecmp(proxy, "http://", 7) == 0) p = proxy + 7;
        else if (strncasecmp(proxy, "https://", 8) == 0) p = proxy + 8;
        snprintf(phost, sizeof phost, "%s", p);
        char *colon = strchr(phost, ':');
        if (colon) {
            *colon = '\0';
            unsigned q = parse_port(colon + 1, 8080);
            if (q) cport = q;
        }
        chost = phost;
        if (!cport) cport = 8080;
        via_proxy = 1;
    }

    struct addrinfo hints;
    memset(&hints, 0, sizeof hints);
    hints.ai_family = AF_UNSPEC;
    hints.ai_socktype = SOCK_STREAM;
    struct addrinfo *res = NULL;
    /* service 必须给真实端口号:① node=NULL 时 service 也不能为空,否则 IP
     * 字面量(如代理 127.0.0.1)会 EAI_NONAME;② 若图省事填 "0",getaddrinfo
     * 返回的 sockaddr 里 sin_port 就是 0,connect 到端口 0 直接 EADDRNOTAVAIL。 */
    char portbuf[16];
    snprintf(portbuf, sizeof portbuf, "%u", cport);
    if (getaddrinfo(chost, portbuf, &hints, &res) != 0) {
        snprintf(c->why, sizeof c->why, "name lookup failed for %s", chost);
        return -1;
    }
    int fd = -1;
    for (struct addrinfo *r = res; r; r = r->ai_next) {
        fd = socket(r->ai_family, SOCK_STREAM, 0);
        if (fd < 0) { fd = -1; continue; }
        int one = 1;
        setsockopt(fd, IPPROTO_TCP, TCP_NODELAY, &one, sizeof one);
        if (connect_with_deadline(fd, r->ai_addr, r->ai_addrlen, deadline) != 0) {
            fprintf(stderr, "[dbg] connect(%s) errno=%d %s\n", chost, errno, strerror(errno));
            close(fd);
            fd = -1;
            continue;
        }
        break;
    }
    freeaddrinfo(res);
    if (fd < 0) {
        /* 带出原因:否则「连不上」永远只剩一句没信息的 cannot reach。 */
        snprintf(c->why, sizeof c->why, "%s", strerror(errno));
        return -1;
    }
    c->fd = fd;
    int flags = fcntl(fd, F_GETFL, 0);
    if (flags >= 0) fcntl(fd, F_SETFL, flags | O_NONBLOCK);

    if (via_proxy && u->https) {
        /* CONNECT 隧道:读头部直到 \r\n\r\n,2xx/3xx 才算开成。 */
        char req[1024];
        int n = snprintf(req, sizeof req,
                         "CONNECT %s:%u HTTP/1.1\r\nHost: %s:%u\r\n\r\n",
                         u->host, u->port, u->host, u->port);
        if (n < 0 || (size_t)n >= sizeof req) { conn_close(c); return -1; }
        if (conn_write(c, req, (size_t)n, deadline) != 0) {
            snprintf(c->why, sizeof c->why, "proxy %s:%u unreachable for CONNECT", chost, cport);
            conn_close(c); return -1;
        }
        char hdr[2048];
        size_t off = 0, got = 0;
        for (;;) {
            if (conn_read_exact(c, hdr + off, 1, &got, deadline) != 0) {
                snprintf(c->why, sizeof c->why, "proxy %s:%u sent no CONNECT reply", chost, cport);
                conn_close(c); return -1;
            }
            off += got;
            if (off >= 4 && memcmp(hdr + off - 4, "\r\n\r\n", 4) == 0) break;
            if (off + 1 >= sizeof hdr) {
                hdr[sizeof hdr - 1] = '\0';
                snprintf(c->why, sizeof c->why, "proxy CONNECT reply: %s", hdr);
                conn_close(c); return -1;
            }
        }
        /* 代理回 407/403 时不能当成隧道建好往下走。
         * 不能硬编码状态码下标:状态行 "HTTP/1.1 200 OK" 里状态码第一位在
         * 下标 8(HTTP/ 占 0..4,'1' 5,'.' 6,' ' 7);定位第一个空格后取三位
         * 数字,顺带兼容 HTTP/1.0 与多余空格。 */
        int rc = 0;
        const char *sp = strstr(hdr, "HTTP/");
        if (sp && (sp = strchr(sp, ' ')) != NULL &&
            isdigit((unsigned char)sp[1]) && isdigit((unsigned char)sp[2]) &&
            isdigit((unsigned char)sp[3]))
            rc = (sp[1] - '0') * 100 + (sp[2] - '0') * 10 + (sp[3] - '0');
        if (rc < 200 || rc > 399) {
            hdr[off < sizeof hdr ? off : sizeof hdr - 1] = '\0';
            snprintf(c->why, sizeof c->why, "proxy refused CONNECT (%d): %s", rc, hdr);
            conn_close(c); return -1;
        }
    }
    if (u->https) {
        int r = tls_attach(c, u->host, deadline);
        if (r == 1) { conn_close(c); return 1; }
        if (r != 0) {
            if (!c->why[0]) snprintf(c->why, sizeof c->why, "TLS handshake failed");
            conn_close(c); return -1;
        }
    }
    return 0;
}

/* ---- 请求报文 ---- */

/* Host 头:默认端口不带 ':port';IPv6 字面量要加方括号。 */
static void append_host_header(sbuf *b, Url *u) {
    char host[600];
    unsigned dflt = u->https ? 443u : 80u;
    if (strchr(u->host, ':'))
        snprintf(host, sizeof host, "[%s]:%u", u->host, u->port);
    else if (u->port != dflt)
        snprintf(host, sizeof host, "%s:%u", u->host, u->port);
    else
        snprintf(host, sizeof host, "%s", u->host);
    sb_str(b, "Host: ");
    sb_str(b, host);
    sb_str(b, "\r\n");
}

static int build_request(sbuf *b, Url *u, const char *proxy, Obj *hdrs) {
    int via_proxy = (proxy && proxy[0] && !u->https) ? 1 : 0;
    if (via_proxy) { sb_str(b, "GET "); sb_str(b, "http://"); sb_str(b, u->host); sb_str(b, u->path); }
    else           { sb_str(b, "GET "); sb_str(b, u->path); }
    sb_str(b, " HTTP/1.1\r\n");

    append_host_header(b, u);
    sb_str(b, "User-Agent: lume-http/0.1\r\n");
    sb_str(b, "Accept: */*\r\n");
    /* 本实现不做解压,明确要 identity:否则中间代理(或服务端)回 gzip,
     * body 里就是一串二进制,脚本侧拿到的是乱码而非内容。 */
    sb_str(b, "Accept-Encoding: identity\r\n");
    sb_str(b, "Connection: close\r\n"); /* 无 keep-alive:响应读完即收工 */

    if (hdrs && hdrs->type == OBJ_MAP) {
        for (int i = 0; i < hdrs->as.map.count; i++) {
            const char *k = hdrs->as.map.keys[i];
            if (!k || !k[0]) continue;
            Value v = hdrs->as.map.vals[i];
            char line[1024];
            if (IS_OBJ(v) && AS_OBJ(v)->type == OBJ_STRING) {
                int n = snprintf(line, sizeof line, "%s: %s\r\n", k, obj_string(AS_OBJ(v)));
                if (n > 0 && (size_t)n < sizeof line) sb_mem(b, line, (size_t)n);
            } else if (IS_NUM(v)) {
                int n = snprintf(line, sizeof line, "%s: %g\r\n", k, AS_NUM(v));
                if (n > 0 && (size_t)n < sizeof line) sb_mem(b, line, (size_t)n);
            } else if (IS_BOOL(v)) {
                int n = snprintf(line, sizeof line, "%s: %s\r\n", k, AS_BOOL(v) ? "true" : "false");
                if (n > 0 && (size_t)n < sizeof line) sb_mem(b, line, (size_t)n);
            }
        }
    }
    sb_str(b, "\r\n");
    return b->oom ? -1 : 0;
}

/* ---- 响应读取 ---- */

/* 读头部到 \r\n\r\n 为止,解析状态码。返回 0 成功,-1 失败,-2 超时。 */
static int read_head(Conn *c, char *hdr, size_t hdrsz, int *status, long deadline) {
    size_t off = 0, got = 0;
    for (;;) {
        if (conn_read_exact(c, hdr + off, 1, &got, deadline) != 0) return -2;
        off += got;
        if (off >= 4 && memcmp(hdr + off - 4, "\r\n\r\n", 4) == 0) break;
        if (off + 1 >= hdrsz) return -1; /* 头太大:当作畸形响应 */
    }
    /* 状态行 "HTTP/1.1 200 OK":'HTTP/' 占 0..4,'1' 5,'.' 6,'1' 7,' ' 8,
     * 状态码第一位在下标 9 —— atoi 会自动吃到末尾空白,直接整段解析即可。 */
    if (isdigit((unsigned char)hdr[9])) *status = atoi(hdr + 9);
    else *status = 0;
    return 0;
}

static bool head_has(const char *hdr, const char *key) {
    return strncasecmp(hdr, key, strlen(key)) == 0;
}

/* 按 Content-Length / chunked / EOF 三种方式读 body,总计不超过 max_bytes。 */
static int read_body(Conn *c, sbuf *b, size_t max_bytes, const char *hdr,
                     long deadline) {
    const char *cl = strstr(hdr, "Content-Length:");
    if (cl) {
        cl += strlen("Content-Length:");
        while (*cl == ' ' || *cl == '\t') cl++;
        long long want = strtoll(cl, NULL, 10);
        if (want > 0) {
            if ((size_t)want > max_bytes) want = (long long)max_bytes;
            char *chunk = malloc((size_t)want + 1);
            if (!chunk) return -1;
            size_t got = 0;
            int r = conn_read_exact(c, chunk, (size_t)want, &got, deadline);
            (void)got;
            if (r == 0) sb_mem(b, chunk, (size_t)want);
            free(chunk);
            return r;
        }
        return 0;
    }
    if (strstr(hdr, "Transfer-Encoding:") && strstr(hdr, "chunked")) {
        /* 逐块:行数 + 十六进制长度 + 数据 + CRLF,遇到 0 长度结束。 */
        for (;;) {
            char line[256];
            size_t n = 0, got = 0;
            for (; n + 1 < sizeof line;) {
                if (conn_read_exact(c, line + n, 1, &got, deadline) != 0) return -2;
                if (got == 0) return -1;
                n += got;
                if (line[n - 1] == '\n') break;
            }
            line[n > 0 ? n - 1 : 0] = '\0';
            long len = strtol(line, NULL, 16); /* hex */
            if (len <= 0) return 0;
            if (b->len + (size_t)len > max_bytes) return 0;
            char *chunk = malloc((size_t)len);
            if (!chunk) return -1;
            size_t g = 0;
            int r = conn_read_exact(c, chunk, (size_t)len, &g, deadline);
            if (r == 0) sb_mem(b, chunk, g);
            free(chunk);
            if (r != 0) return r;
            char crlf[2];
            size_t g2 = 0;
            conn_read_exact(c, crlf, 2, &g2, deadline); /* 吃掉 CRLF */
        }
    }
    /* 没有 Content-Length:读到 EOF 为止(Connection: close 的常规响应)。 */
    for (;;) {
        char buf[16384];
        size_t g = 0;
        int r = conn_read_some(c, buf, sizeof buf, &g, deadline);
        if (r != 0) return r;
        if (g == 0) return 0;
        if (b->len + g > max_bytes) { sb_mem(b, buf, max_bytes - b->len); return 0; }
        sb_mem(b, buf, g);
    }
}

/* ---- 内建 ---- */

void native_http_get(VM *vm, int argc, Value *args, Value *out) {
    if (vm->no_net) {
        vm_set_error(vm, "http_get(): network access is disabled in this run "
                         "(--no-net / LUME_NO_NET=1)");
        return;
    }
    if (argc < 1) { vm_set_error(vm, "http_get() needs a url"); return; }
    const char *url0 = NULL;
    if (!arg_string(vm, args[0], &url0)) return;

    long timeout_ms = HTTP_DFLT_TIMEOUT_MS;
    size_t max_bytes = HTTP_DFLT_MAX_BYTES;
    int allow_private = 0;
    Obj *hdrs = NULL;
    if (argc >= 2 && IS_OBJ(args[1]) && AS_OBJ(args[1])->type == OBJ_MAP) {
        Obj *o = AS_OBJ(args[1]);
        int f = 0;
        Value v;
        /* timeout 单位是秒;60 s 封顶。 */
        v = map_get(vm, o, "timeout", &f);       if (f && IS_NUM(v)) { double d = AS_NUM(v); if (d > 0) timeout_ms = HTTP_SEC_TO_MS(d) < HTTP_MAX_TIMEOUT_MS ? HTTP_SEC_TO_MS(d) : HTTP_MAX_TIMEOUT_MS; }
        v = map_get(vm, o, "max_bytes", &f);     if (f && IS_NUM(v)) { double d = AS_NUM(v); if (d > 0) max_bytes = (size_t)(d < (double)HTTP_MAX_MAX_BYTES ? d : (double)HTTP_MAX_MAX_BYTES); }
        v = map_get(vm, o, "allow_private", &f); if (f && IS_BOOL(v)) allow_private = AS_BOOL(v) ? 1 : 0;
        v = map_get(vm, o, "headers", &f);
        if (f && IS_OBJ(v) && AS_OBJ(v)->type == OBJ_MAP) hdrs = AS_OBJ(v);
    }

    char cur[1024];
    snprintf(cur, sizeof cur, "%s", url0);
    char hostbuf[512];
    char next[1024];
    char hdr[HTTP_MAX_HEADER_SZ];
    int status = 0;
    bool ok = false;
    bool truncated = false;
    char err[256] = "";
    sbuf body = {0}; /* 跨循环复用:结果 map 要读最后一跳的 body */

    for (int hop = 0; hop <= HTTP_MAX_REDIRECTS; hop++) {
        Url u;
        memset(&u, 0, sizeof u);
        if (url_parse(cur, hostbuf, sizeof hostbuf, &u) != 0) {
            snprintf(err, sizeof err, "http_get(): unsupported url: %s", cur);
            break;
        }
        if (!allow_private && host_blocked(u.host)) {
            snprintf(err, sizeof err, "http_get(): refused — %s resolves to a "
                                      "private/reserved address", u.host);
            break;
        }
        Conn c = { -1, NULL, { 0 } };
        long deadline = now_ms() + timeout_ms;
        int rc = open_conn(&u, proxy_for(u.https), &c, deadline);
        if (rc != 0) {
            conn_close(&c);
            if (rc == 1) snprintf(err, sizeof err, "http_get(): https:// needs "
                                                   "libssl (rebuild with openssl)");
            else snprintf(err, sizeof err, "http_get(): cannot reach %s (%s)",
                          u.host, c.why[0] ? c.why : "connection refused");
            break;
        }
        sbuf req = {0};
        int brc = build_request(&req, &u, proxy_for(u.https), hdrs);
        if (brc == 0) brc = conn_write(&c, req.p, req.len, deadline);
        free(req.p);
        if (brc != 0) {
            conn_close(&c);
            snprintf(err, sizeof err, brc == -2
                     ? "http_get(): request timed out after %ld ms"
                     : "http_get(): send failed", timeout_ms);
            break;
        }
        memset(hdr, 0, sizeof hdr);
        int rrc = read_head(&c, hdr, sizeof hdr, &status, deadline);
        if (rrc != 0) {
            conn_close(&c);
            snprintf(err, sizeof err, rrc == -2
                     ? "http_get(): response timed out after %ld ms"
                     : "http_get(): malformed response head", timeout_ms);
            break;
        }
        int drc = read_body(&c, &body, max_bytes, hdr, deadline);
        if (drc == -2) {
            body_done(&body);
            conn_close(&c);
            snprintf(err, sizeof err, "http_get(): body timed out after %ld ms", timeout_ms);
            break;
        }
        if (body.len >= max_bytes && max_bytes > 0) truncated = true;

        /* 3xx 且给了 Location:逐跳再校验后继续(循环开头会再走一遍闸门)。 */
        const char *loc = strstr(hdr, "\r\nLocation:");
        if (head_has(hdr, "HTTP/") && status >= 300 && status < 400 && loc) {
            loc += strlen("\r\nLocation:");
            while (*loc == ' ' || *loc == '\t') loc++;
            char lb[512];
            snprintf(lb, sizeof lb, "%s", loc);
            char *cr = strpbrk(lb, "\r\n");
            if (cr) *cr = '\0';
            conn_close(&c);
            body_done(&body); /* 这一跳作废:不带着上一跳的缓冲进下一跳 */
            if (resolve_redirect(cur, lb, next, sizeof next)) {
                snprintf(cur, sizeof cur, "%s", next);
                continue;
            }
            snprintf(err, sizeof err, "http_get(): unsupported redirect target: %s", lb);
            break;
        }
        conn_close(&c);
        ok = true;
        break;
    }

    if (truncated && !err[0])
        snprintf(err, sizeof err, "http_get(): body truncated at %zu bytes", max_bytes);

    /* 结果 map 要先 vm_push 再 make_string:make_string 可能触发 GC,
     * 没扎根的话 r 会在这一瞬间被回收。 */
    Obj *r = AS_OBJ(make_map(vm));
    vm_push(vm, val_obj(r));
    map_set(vm, r, "ok", val_bool(ok));
    map_set(vm, r, "status", val_int((long long)status));
    /* 顺序要紧:make_string 已经把 body 拷成字符串对象了(body 可能是二进制,
     * length 用 body.len 而非 strlen),这之后才能释放并复位缓冲区。 */
    map_set(vm, r, "body", make_string(vm, body.p ? body.p : "", body.len));
    map_set(vm, r, "err", make_string_cstr(vm, err[0] ? err : ""));
    body_done(&body);
    *out = vm_pop(vm);
}

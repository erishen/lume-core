/* Cross-platform socket layer: POSIX (linux/macOS) vs winsock (Windows/mingw).
 *
 * Shared by builtins_http.c (outbound http_get/post/...) and bridge_serve.c
 * (inbound server, bridge_run). The rest of lume-core is platform-neutral;
 * this header is the only place that knows how a socket is spelled on each
 * platform, so the two network files stay readable.
 *
 * Windows notes:
 *   - winsock2.h must be included before any header that may pull in
 *     windows.h (it conflicts with winsock1). This header is the first
 *     network include on purpose.
 *   - SOCKET is unsigned; INVALID_SOCKET is (SOCKET)~0, which is -1 when
 *     stored in a signed int. Code here uses net_fd + NET_INVALID, never a
 *     bare `int fd = -1`.
 *   - There is no poll(); WSAPoll (Vista+) is the direct replacement.
 *   - Non-blocking is ioctlsocket(FIONBIO), not fcntl.
 *   - WSAStartup must run once before any socket call.
 */

#ifndef LUME_NET_COMPAT_H
#define LUME_NET_COMPAT_H

#if defined(_WIN32)
#include <winsock2.h>
#include <ws2tcpip.h>

typedef SOCKET net_fd;
#define NET_INVALID   INVALID_SOCKET
#define NET_ERRNO()   ((int)WSAGetLastError())
#define NET_EINTR     WSAEINTR
#define NET_EAGAIN    WSAEWOULDBLOCK
#define NET_EWOULDBLOCK WSAEWOULDBLOCK
#define NET_EINPROGRESS WSAEWOULDBLOCK /* non-blocking connect on winsock */
#define net_close(fd) closesocket(fd)
#define net_poll(pfd, n, t) WSAPoll((pfd), (n), (t))

#else
#include <arpa/inet.h>
#include <errno.h>
#include <fcntl.h>
#include <netdb.h>
#include <netinet/in.h>
#include <netinet/tcp.h>
#include <poll.h>
#include <strings.h>
#include <sys/socket.h>
#include <sys/types.h>
#include <time.h>
#include <unistd.h>

typedef int net_fd;
#define NET_INVALID   (-1)
#define NET_ERRNO()   (errno)
#define NET_EINTR     EINTR
#define NET_EAGAIN    EAGAIN
#define NET_EWOULDBLOCK EWOULDBLOCK
#define NET_EINPROGRESS EINPROGRESS
#define net_close(fd) close(fd)
#define net_poll(pfd, n, t) poll((pfd), (n), (t))

#endif

/* One-time init: WSAStartup on Windows, no-op elsewhere. */
void net_init(void);

/* Monotonic milliseconds (CLOCK_MONOTONIC / GetTickCount64). */
long net_now_ms(void);

/* Set the fd non-blocking (fcntl O_NONBLOCK / ioctlsocket FIONBIO). 0 = ok. */
int net_set_nonblock(net_fd fd);

/* Thread-safe-ish text for the last error (strerror / numeric WSA code). */
const char *net_strerror(int e);

#endif /* LUME_NET_COMPAT_H */

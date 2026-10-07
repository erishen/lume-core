/* net_compat.c — platform glue for the socket layer (see net_compat.h). */

#include "net_compat.h"

#include <stdio.h>
#include <string.h>

#if defined(_WIN32)
#include <windows.h>
#endif

void net_init(void)
{
#if defined(_WIN32)
    static int started = 0;
    if (!started) {
        WSADATA wsa;
        /* Any modern Windows has >= 2.2; failure here is fatal for sockets. */
        if (WSAStartup(MAKEWORD(2, 2), &wsa) == 0) started = 1;
    }
#else
    /* nothing to do */
#endif
}

long net_now_ms(void)
{
#if defined(_WIN32)
    return (long)GetTickCount64();
#else
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return (long)ts.tv_sec * 1000L + ts.tv_nsec / 1000000L;
#endif
}

int net_set_nonblock(net_fd fd)
{
#if defined(_WIN32)
    u_long one = 1;
    return ioctlsocket(fd, FIONBIO, &one) == 0 ? 0 : -1;
#else
    int flags = fcntl(fd, F_GETFL, 0);
    if (flags < 0) return -1;
    return fcntl(fd, F_SETFL, flags | O_NONBLOCK);
#endif
}

const char *net_strerror(int e)
{
#if defined(_WIN32)
    /* FormatMessage is overkill for a debug path; WSA codes are stable. */
    static char buf[64];
    snprintf(buf, sizeof buf, "wsa error %d", e);
    return buf;
#else
    return strerror(e);
#endif
}

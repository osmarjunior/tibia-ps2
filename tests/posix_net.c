/**
 * posix_net.c — NetStreamVt sobre sockets POSIX (testes de host).
 */
#include <stdio.h>
#include <string.h>
#include <errno.h>
#include <unistd.h>
#include <fcntl.h>
#include <sys/socket.h>
#include <sys/select.h>
#include <netinet/in.h>
#include <netinet/tcp.h>
#include <arpa/inet.h>

#include "../src/net/net_stream.h"

static int posix_connect(const char *ip, uint16_t port, int timeout_ms)
{
    struct sockaddr_in addr;
    int fd, flags, r;
    fd_set wfds;
    struct timeval tv;
    int so_err;
    socklen_t slen = sizeof(so_err);

    fd = socket(AF_INET, SOCK_STREAM, IPPROTO_TCP);
    if (fd < 0)
        return -1;

    memset(&addr, 0, sizeof(addr));
    addr.sin_family = AF_INET;
    addr.sin_port = htons(port);
    if (inet_pton(AF_INET, ip, &addr.sin_addr) != 1) {
        close(fd);
        return -1;
    }

    flags = fcntl(fd, F_GETFL, 0);
    fcntl(fd, F_SETFL, flags | O_NONBLOCK);
    r = connect(fd, (struct sockaddr *)&addr, sizeof(addr));
    if (r < 0) {
        if (errno != EINPROGRESS) {
            close(fd);
            return -1;
        }
        FD_ZERO(&wfds);
        FD_SET(fd, &wfds);
        tv.tv_sec = timeout_ms / 1000;
        tv.tv_usec = (timeout_ms % 1000) * 1000;
        if (select(fd + 1, NULL, &wfds, NULL, &tv) <= 0) {
            close(fd);
            return -1;
        }
        if (getsockopt(fd, SOL_SOCKET, SO_ERROR, &so_err, &slen) < 0 ||
            so_err != 0) {
            close(fd);
            return -1;
        }
    }
    fcntl(fd, F_SETFL, flags);
    return fd;
}

static int posix_send_all(int fd, const void *buf, size_t len)
{
    const char *p = buf;
    size_t left = len;
    while (left > 0) {
        ssize_t n = send(fd, p, left, MSG_NOSIGNAL);
        if (n <= 0)
            return -1;
        p += n;
        left -= (size_t)n;
    }
    return 0;
}

static int wait_readable(int fd, int timeout_ms)
{
    fd_set rfds;
    struct timeval tv;
    FD_ZERO(&rfds);
    FD_SET(fd, &rfds);
    tv.tv_sec = timeout_ms / 1000;
    tv.tv_usec = (timeout_ms % 1000) * 1000;
    return select(fd + 1, &rfds, NULL, NULL, &tv);
}

static int posix_recv_all(int fd, void *buf, size_t len, int timeout_ms)
{
    char *p = buf;
    size_t left = len;
    while (left > 0) {
        int r = wait_readable(fd, timeout_ms);
        if (r == 0)
            return -2;
        if (r < 0)
            return -1;
        ssize_t n = recv(fd, p, left, 0);
        if (n == 0)
            return -1;
        if (n < 0)
            return -1;
        p += n;
        left -= (size_t)n;
    }
    return 0;
}

static int posix_recv_some(int fd, void *buf, size_t len, int timeout_ms)
{
    int r = wait_readable(fd, timeout_ms);
    if (r == 0)
        return -2;
    if (r < 0)
        return -1;
    ssize_t n = recv(fd, buf, len, 0);
    if (n == 0)
        return -1;
    return (int)n;
}

static void posix_close(int fd)
{
    close(fd);
}

static void posix_yield(void)
{
}

void posix_net_install(void)
{
    static const NetStreamVt vt = {
        posix_connect, posix_send_all, posix_recv_all, posix_recv_some,
        posix_close, posix_yield,
    };
    net_set_stream(&vt);
}

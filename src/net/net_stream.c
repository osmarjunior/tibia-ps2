/**
 * net_stream.c — despacho da abstração de socket.
 */
#include "net_stream.h"

static const NetStreamVt *vt = 0;

void net_set_stream(const NetStreamVt *v)
{
    vt = v;
}

int net_connect(const char *ip, uint16_t port, int timeout_ms)
{
    return vt->connect(ip, port, timeout_ms);
}

int net_send_all(int fd, const void *buf, size_t len)
{
    return vt->send_all(fd, buf, len);
}

int net_recv_all(int fd, void *buf, size_t len, int timeout_ms)
{
    return vt->recv_all(fd, buf, len, timeout_ms);
}

int net_recv_some(int fd, void *buf, size_t len, int timeout_ms)
{
    return vt->recv_some(fd, buf, len, timeout_ms);
}

void net_close(int fd)
{
    vt->close(fd);
}

void net_yield(void)
{
    if (vt && vt->yield)
        vt->yield();
}

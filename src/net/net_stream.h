/**
 * net_stream.h — abstração fina de socket para o código de protocolo.
 *
 * Na PS2 a implementação é ps2_net.c (lwIP). Nos testes de host uma
 * implementação POSIX/Winsock é registrada pelo harness de teste. Assim
 * login.c/game_protocol.c compilam sem #ifdef nos dois ambientes.
 */
#ifndef NET_STREAM_H
#define NET_STREAM_H

#include <stddef.h>
#include <stdint.h>

typedef struct {
    int (*connect)(const char *ip, uint16_t port, int timeout_ms);
    int (*send_all)(int fd, const void *buf, size_t len);
    /* 0 ok, -1 erro/EOF, -2 timeout */
    int (*recv_all)(int fd, void *buf, size_t len, int timeout_ms);
    /* >=0 bytes lidos, -1 erro/EOF, -2 timeout */
    int (*recv_some)(int fd, void *buf, size_t len, int timeout_ms);
    void (*close)(int fd);
    void (*yield)(void);
} NetStreamVt;

/* Registra a implementação (PS2: ps2_net_default_stream(); host: harness). */
void net_set_stream(const NetStreamVt *vt);

int  net_connect(const char *ip, uint16_t port, int timeout_ms);
int  net_send_all(int fd, const void *buf, size_t len);
int  net_recv_all(int fd, void *buf, size_t len, int timeout_ms);
int  net_recv_some(int fd, void *buf, size_t len, int timeout_ms);
void net_close(int fd);
void net_yield(void);

#endif /* NET_STREAM_H */

/**
 * netmsg.h — leitor/escritor de mensagens do protocolo Tibia 7.4
 * + enquadramento XTEA ([u16 total][XTEA{[u16 inner][body][pad]}]).
 *
 * Layout da mensagem na memória (igual ao NetworkMessage do OTHire):
 *   buf[0..1]  : comprimento (crypto header / length header)
 *   buf[2..3]  : inner length (u16) — primeiro campo do corpo
 *   buf[4...]  : corpo
 * pos == índice de leitura/escrita a partir de buf[2].
 */
#ifndef NETMSG_H
#define NETMSG_H

#include <stdint.h>
#include <stddef.h>
#include "xtea.h"

#define NETMSG_MAXSIZE 8192

typedef struct {
    uint8_t buf[NETMSG_MAXSIZE];
    int pos; /* read/write cursor, relativo a buf[2] */
    int len; /* bytes de corpo válidos */
} NetMsg;

void nm_init(NetMsg *m);

/* ---- escrita ---- */
void nm_add_u8(NetMsg *m, uint8_t v);
void nm_add_u16(NetMsg *m, uint16_t v);
void nm_add_u32(NetMsg *m, uint32_t v);
void nm_add_string(NetMsg *m, const char *s);
void nm_add_bytes(NetMsg *m, const uint8_t *b, int n);
void nm_add_position(NetMsg *m, uint16_t x, uint16_t y, uint8_t z);

/* ---- leitura ---- */
uint8_t  nm_get_u8(NetMsg *m);
uint16_t nm_get_u16(NetMsg *m);
uint32_t nm_get_u32(NetMsg *m);
int      nm_get_string(NetMsg *m, char *out, int cap);
void     nm_skip(NetMsg *m, int n);
uint8_t  nm_peek_u8(NetMsg *m);
int      nm_remaining(const NetMsg *m);

/* ---- framing / IO ----
 * Envia m: corpo = buf[2..2+len]. Se key: pad até %8, cifra, header total.
 * Retorna 0/-1. */
int nm_send(int fd, NetMsg *m, const XteaKey *key);

/* Lê uma mensagem completa do socket. Se key != NULL, decifra e valida o
 * inner length. Retorna 0 ok, -1 erro/EOF, -2 timeout, -3 malformed. */
int nm_recv(int fd, NetMsg *m, const XteaKey *key, int timeout_ms);

#endif /* NETMSG_H */

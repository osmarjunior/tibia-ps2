/**
 * netmsg.c — implementação do leitor/escritor + framing XTEA.
 * Portátil (host e PS2). Endianness: little no wire.
 */
#include <string.h>
#include <stdio.h>
#include "netmsg.h"
#include "net_stream.h"

void nm_init(NetMsg *m)
{
    m->pos = 0;
    m->len = 0;
}

/* ---------------- leitura ---------------- */

static int nm_can_read(NetMsg *m, int n)
{
    return (m->pos + n) <= m->len;
}

uint8_t nm_get_u8(NetMsg *m)
{
    if (!nm_can_read(m, 1))
        return 0;
    return m->buf[2 + m->pos++];
}

uint16_t nm_get_u16(NetMsg *m)
{
    uint16_t v;
    if (!nm_can_read(m, 2))
        return 0;
    const uint8_t *p = &m->buf[2 + m->pos];
    m->pos += 2;
    v = (uint16_t)(p[0] | (p[1] << 8));
    return v;
}

uint32_t nm_get_u32(NetMsg *m)
{
    uint32_t v;
    if (!nm_can_read(m, 4))
        return 0;
    const uint8_t *p = &m->buf[2 + m->pos];
    m->pos += 4;
    v = (uint32_t)p[0] | ((uint32_t)p[1] << 8) |
        ((uint32_t)p[2] << 16) | ((uint32_t)p[3] << 24);
    return v;
}

int nm_get_string(NetMsg *m, char *out, int cap)
{
    uint16_t n = nm_get_u16(m);
    if (n > 8192 || !nm_can_read(m, (int)n))
        return -1;
    int copy = n;
    if (copy >= cap)
        copy = cap - 1;
    if (copy > 0)
        memcpy(out, &m->buf[2 + m->pos], (size_t)copy);
    if (cap > 0)
        out[copy] = 0;
    m->pos += n;
    return copy;
}

void nm_skip(NetMsg *m, int n)
{
    m->pos += n;
    if (m->pos > m->len)
        m->pos = m->len;
}

uint8_t nm_peek_u8(NetMsg *m)
{
    if (!nm_can_read(m, 1))
        return 0;
    return m->buf[2 + m->pos];
}

int nm_remaining(const NetMsg *m)
{
    return m->len - m->pos;
}

/* ---------------- escrita ---------------- */

static int nm_can_write(NetMsg *m, int n)
{
    return (m->pos + n) <= NETMSG_MAXSIZE - 4;
}

void nm_add_u8(NetMsg *m, uint8_t v)
{
    if (!nm_can_write(m, 1))
        return;
    m->buf[2 + m->pos++] = v;
    m->len++;
}

void nm_add_u16(NetMsg *m, uint16_t v)
{
    if (!nm_can_write(m, 2))
        return;
    uint8_t *p = &m->buf[2 + m->pos];
    p[0] = (uint8_t)(v & 0xFF);
    p[1] = (uint8_t)(v >> 8);
    m->pos += 2;
    m->len += 2;
}

void nm_add_u32(NetMsg *m, uint32_t v)
{
    if (!nm_can_write(m, 4))
        return;
    uint8_t *p = &m->buf[2 + m->pos];
    p[0] = (uint8_t)(v & 0xFF);
    p[1] = (uint8_t)((v >> 8) & 0xFF);
    p[2] = (uint8_t)((v >> 16) & 0xFF);
    p[3] = (uint8_t)((v >> 24) & 0xFF);
    m->pos += 4;
    m->len += 4;
}

void nm_add_string(NetMsg *m, const char *s)
{
    size_t n = strlen(s);
    if (n > 8192)
        n = 8192;
    nm_add_u16(m, (uint16_t)n);
    nm_add_bytes(m, (const uint8_t *)s, (int)n);
}

void nm_add_bytes(NetMsg *m, const uint8_t *b, int n)
{
    if (!nm_can_write(m, n))
        return;
    memcpy(&m->buf[2 + m->pos], b, n);
    m->pos += n;
    m->len += n;
}

void nm_add_position(NetMsg *m, uint16_t x, uint16_t y, uint8_t z)
{
    nm_add_u16(m, x);
    nm_add_u16(m, y);
    nm_add_u8(m, z);
}

/* ---------------- framing ---------------- */

int nm_send(int fd, NetMsg *m, const XteaKey *key)
{
    int total;

    if (key) {
        /* corpo está em buf[2..2+len); abre 2 bytes para o u16 inner */
        memmove(&m->buf[4], &m->buf[2], (size_t)m->len);
        m->buf[2] = (uint8_t)(m->len & 0xFF);
        m->buf[3] = (uint8_t)(m->len >> 8);

        total = m->len + 2; /* inner + body */
        int pad = (8 - (total & 7)) & 7;
        memset(&m->buf[2 + total], 0x33, pad);
        total += pad;

        xtea_encrypt(key, &m->buf[2], (size_t)total);
    } else {
        total = m->len;
    }

    if (total + 2 > NETMSG_MAXSIZE)
        return -1;

    m->buf[0] = (uint8_t)(total & 0xFF);
    m->buf[1] = (uint8_t)(total >> 8);

    if (net_send_all(fd, m->buf, (size_t)(total + 2)) != 0)
        return -1;
    return 0;
}

int nm_recv(int fd, NetMsg *m, const XteaKey *key, int timeout_ms)
{
    uint8_t hdr[2];
    int total, r;

    r = net_recv_all(fd, hdr, 2, timeout_ms);
    if (r != 0)
        return r; /* -1 erro/EOF, -2 timeout */

    total = (int)(hdr[0] | (hdr[1] << 8));
    if (total == 0)
        return -3; /* mensagem vazia (game server envia ao conectar) */
    if (total < 2 || total > NETMSG_MAXSIZE - 2) {
        uint8_t discard[256];
        int remaining = total;

        while (remaining > 0) {
            int chunk = remaining < (int)sizeof(discard)
                            ? remaining : (int)sizeof(discard);
            r = net_recv_all(fd, discard, (size_t)chunk, timeout_ms);
            if (r != 0)
                return r;
            remaining -= chunk;
        }
        return -3;
    }

    r = net_recv_all(fd, &m->buf[2], (size_t)total, timeout_ms);
    if (r != 0)
        return r;

    if (key) {
        if ((total & 7) != 0)
            return -3;
        xtea_decrypt(key, &m->buf[2], (size_t)total);
        int inner = (int)(m->buf[2] | (m->buf[3] << 8));
        if (inner < 0 || inner + 2 > total)
            return -3;
        /* região decifrada: [u16 inner][body] a partir de buf[2];
         * pos conta a partir de buf[2], então o corpo começa em pos=2 */
        m->len = inner + 2;
        m->pos = 2;
    } else {
        m->len = total;
        m->pos = 0;
    }
    return 0;
}

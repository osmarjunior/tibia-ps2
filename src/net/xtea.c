/**
 * xtea.c — XTEA (32 rodadas, delta 0x9E3779B9), palavras LE do buffer.
 * Portátil (host de teste e PS2/EE).
 */
#include "xtea.h"

#define XTEA_DELTA 0x9E3779B9u
#define XTEA_ROUNDS 32

static inline uint32_t rd_le32(const uint8_t *p)
{
    return (uint32_t)p[0] | ((uint32_t)p[1] << 8) |
           ((uint32_t)p[2] << 16) | ((uint32_t)p[3] << 24);
}

static inline void wr_le32(uint8_t *p, uint32_t v)
{
    p[0] = (uint8_t)(v & 0xFF);
    p[1] = (uint8_t)((v >> 8) & 0xFF);
    p[2] = (uint8_t)((v >> 16) & 0xFF);
    p[3] = (uint8_t)((v >> 24) & 0xFF);
}

void xtea_encrypt(const XteaKey *key, uint8_t *buf, size_t len)
{
    for (size_t off = 0; off + 8 <= len; off += 8) {
        uint32_t v0 = rd_le32(buf + off);
        uint32_t v1 = rd_le32(buf + off + 4);
        uint32_t sum = 0;

        for (int i = 0; i < XTEA_ROUNDS; i++) {
            v0 += (((v1 << 4) ^ (v1 >> 5)) + v1) ^ (sum + key->k[sum & 3]);
            sum += XTEA_DELTA;
            v1 += (((v0 << 4) ^ (v0 >> 5)) + v0) ^ (sum + key->k[(sum >> 11) & 3]);
        }

        wr_le32(buf + off, v0);
        wr_le32(buf + off + 4, v1);
    }
}

void xtea_decrypt(const XteaKey *key, uint8_t *buf, size_t len)
{
    for (size_t off = 0; off + 8 <= len; off += 8) {
        uint32_t v0 = rd_le32(buf + off);
        uint32_t v1 = rd_le32(buf + off + 4);
        uint32_t sum = XTEA_DELTA * XTEA_ROUNDS; /* 0xC6EF3720 */

        for (int i = 0; i < XTEA_ROUNDS; i++) {
            v1 -= (((v0 << 4) ^ (v0 >> 5)) + v0) ^ (sum + key->k[(sum >> 11) & 3]);
            sum -= XTEA_DELTA;
            v0 -= (((v1 << 4) ^ (v1 >> 5)) + v1) ^ (sum + key->k[sum & 3]);
        }

        wr_le32(buf + off, v0);
        wr_le32(buf + off + 4, v1);
    }
}

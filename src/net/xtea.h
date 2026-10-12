/**
 * xtea.h — XTEA para o protocolo Tibia 7.4 (era OTServ).
 *
 * Variante usada pelos servidores da era (Protocol::XTEA_* do OTHire):
 * 32 rodadas, delta 0x9E3779B9, blocos de 8 bytes lidos do buffer como
 * dois u32 little-endian. Idêntica ao XTEA padrão.
 */
#ifndef TIBIA_XTEA_H
#define TIBIA_XTEA_H

#include <stdint.h>
#include <stddef.h>

typedef struct {
    uint32_t k[4];
} XteaKey;

/* Encrypt len bytes in place. len MUST be a multiple of 8. */
void xtea_encrypt(const XteaKey *key, uint8_t *buf, size_t len);

/* Decrypt len bytes in place. len MUST be a multiple of 8. */
void xtea_decrypt(const XteaKey *key, uint8_t *buf, size_t len);

#endif /* TIBIA_XTEA_H */

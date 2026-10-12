/**
 * tibia_rsa.h — RSA do handshake de login (1024-bit, e=65537).
 *
 * Usa aritmética Montgomery de tamanho fixo para desempenho no EE do PS2.
 * O servidor referencia (OTHire/OTServ) usa o par de chaves default da era;
 * o cliente cifra com a pública.
 */
#ifndef TIBIA_RSA_H
#define TIBIA_RSA_H

#include <stdint.h>

#define TIBIA_RSA_SIZE 128

/* c = m^65537 mod n (chave pública da era OT). out e in: 128 bytes BE. */
int tibia_rsa_encrypt(uint8_t out[TIBIA_RSA_SIZE],
                      const uint8_t in[TIBIA_RSA_SIZE]);

/* PRNG fraco para as chaves XTEA / padding do handshake (não precisa ser
 * forte: o servidor de teste confia em qualquer valor). */
void tibia_random_bytes(uint8_t *buf, unsigned len);
void tibia_random_seed(void);

#endif /* TIBIA_RSA_H */

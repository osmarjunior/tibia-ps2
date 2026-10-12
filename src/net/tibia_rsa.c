/**
 * tibia_rsa.c — RSA-1024 (e=65537) com aritmética Montgomery de tamanho fixo.
 *
 * A chave pública é o par das chaves privadas default de OTServ/OTHire
 * (source/otserv.cpp do OTHire carrega p, q, d; n = p*q, e = 65537).
 */
#include <stdint.h>
#include <string.h>
#include <time.h>

#include "tibia_rsa.h"

#define RSA_LIMBS (TIBIA_RSA_SIZE / 4)

/* n = p*q (1024-bit) em hex, MSB primeiro */
static const char *RSA_MODULUS_HEX =
    "9b646903b45b07ac956568d87353bd7165139dd7940703b03e6dd079399661b4"
    "a837aa60561d7ccb9452fa0080594909882ab5bca58a1a1b35f8b1059b72b1212"
    "611c6152ad3dbb3cfbee7adc142a75d3d75971509c321c5c24a5bd51fd460f01b"
    "4e15beb0de1930528a5d3f15c1e3cbf5c401d6777e10acaab33dbe8d5b7ff5";

static uint32_t rsa_n[RSA_LIMBS];
static uint32_t rsa_r2[RSA_LIMBS];
static uint32_t rsa_ninv;
static int rsa_ready;

static int hex_value(char c)
{
    if (c >= '0' && c <= '9')
        return c - '0';
    if (c >= 'a' && c <= 'f')
        return c - 'a' + 10;
    return c - 'A' + 10;
}

static int limbs_cmp(const uint32_t *a, const uint32_t *b)
{
    int i;

    for (i = RSA_LIMBS - 1; i >= 0; i--) {
        if (a[i] != b[i])
            return a[i] > b[i] ? 1 : -1;
    }
    return 0;
}

static uint32_t limbs_sub(uint32_t *a, const uint32_t *b)
{
    uint64_t borrow = 0;
    int i;

    for (i = 0; i < RSA_LIMBS; i++) {
        uint64_t av = a[i];
        uint64_t bv = (uint64_t)b[i] + borrow;
        a[i] = (uint32_t)(av - bv);
        borrow = av < bv;
    }
    return (uint32_t)borrow;
}

/* r = a*b*R^-1 mod n, where R = 2^(32*RSA_LIMBS). */
static void montgomery_mul(uint32_t *r, const uint32_t *a,
                           const uint32_t *b)
{
    uint32_t t[2 * RSA_LIMBS + 2];
    uint32_t result[RSA_LIMBS];
    uint32_t high;
    int i, j;

    memset(t, 0, sizeof(t));

    /* Schoolbook product; keep the full 2048-bit intermediate. */
    for (i = 0; i < RSA_LIMBS; i++) {
        uint64_t carry = 0;
        for (j = 0; j < RSA_LIMBS; j++) {
            uint64_t uv = (uint64_t)a[i] * b[j] + t[i + j] + carry;
            t[i + j] = (uint32_t)uv;
            carry = uv >> 32;
        }
        j = i + RSA_LIMBS;
        while (carry != 0) {
            uint64_t uv = (uint64_t)t[j] + carry;
            t[j++] = (uint32_t)uv;
            carry = uv >> 32;
        }
    }

    /* REDC: choose a multiple of n that zeros each low limb in turn. */
    for (i = 0; i < RSA_LIMBS; i++) {
        uint32_t m = t[i] * rsa_ninv;
        uint64_t carry = 0;
        for (j = 0; j < RSA_LIMBS; j++) {
            uint64_t uv = (uint64_t)m * rsa_n[j] + t[i + j] + carry;
            t[i + j] = (uint32_t)uv;
            carry = uv >> 32;
        }
        j = i + RSA_LIMBS;
        while (carry != 0) {
            uint64_t uv = (uint64_t)t[j] + carry;
            t[j++] = (uint32_t)uv;
            carry = uv >> 32;
        }
    }

    for (i = 0; i < RSA_LIMBS; i++)
        result[i] = t[RSA_LIMBS + i];
    high = t[2 * RSA_LIMBS];

    /* REDC produces a value below 2n, so at most one subtraction is needed. */
    if (high != 0 || limbs_cmp(result, rsa_n) >= 0) {
        uint32_t borrow = limbs_sub(result, rsa_n);
        high -= borrow;
    }

    /* Copy last so r may alias either input (as in the squaring loop). */
    memcpy(r, result, sizeof(result));
}

static void rsa_setup(void)
{
    uint32_t v[RSA_LIMBS];
    int digits = (int)strlen(RSA_MODULUS_HEX);
    int i;

    memset(rsa_n, 0, sizeof(rsa_n));
    for (i = 0; i < digits; i++) {
        int nibble_from_lsb = digits - 1 - i;
        rsa_n[i / 8] |= (uint32_t)hex_value(RSA_MODULUS_HEX[nibble_from_lsb])
                        << ((i % 8) * 4);
    }

    /* rsa_ninv = -n[0]^-1 mod 2^32, using Newton iteration. */
    rsa_ninv = rsa_n[0];
    for (i = 0; i < 5; i++)
        rsa_ninv *= 2u - rsa_n[0] * rsa_ninv;
    rsa_ninv = 0u - rsa_ninv;

    /* R2 = 2^(2*1024) mod n, obtained by repeated modular doubling. */
    memset(v, 0, sizeof(v));
    v[0] = 1;
    for (i = 0; i < 2 * RSA_LIMBS * 32; i++) {
        uint32_t top = v[RSA_LIMBS - 1] >> 31;
        uint32_t carry = 0;
        int j;

        for (j = 0; j < RSA_LIMBS; j++) {
            uint32_t next = v[j] >> 31;
            v[j] = (v[j] << 1) | carry;
            carry = next;
        }
        if (top || limbs_cmp(v, rsa_n) >= 0)
            (void)limbs_sub(v, rsa_n);
    }
    memcpy(rsa_r2, v, sizeof(rsa_r2));
    rsa_ready = 1;
}

int tibia_rsa_encrypt(uint8_t out[TIBIA_RSA_SIZE],
                      const uint8_t in[TIBIA_RSA_SIZE])
{
    uint32_t m[RSA_LIMBS], xm[RSA_LIMBS], acc[RSA_LIMBS];
    uint32_t one[RSA_LIMBS];
    int i;

    if (!rsa_ready)
        rsa_setup();

    /* Importa m: 128 bytes big-endian -> 32 limbs little-endian. */
    for (i = 0; i < RSA_LIMBS; i++) {
        const uint8_t *p = &in[TIBIA_RSA_SIZE - 4 - 4 * i];
        m[i] = ((uint32_t)p[0] << 24) | ((uint32_t)p[1] << 16) |
               ((uint32_t)p[2] << 8) | p[3];
    }
    if (limbs_cmp(m, rsa_n) >= 0)
        return -1;

    /* x^65537 = x^(2^16)*x: 16 squarings and one multiply. */
    montgomery_mul(xm, m, rsa_r2); /* x -> xR mod n */
    memcpy(acc, xm, sizeof(acc));
    for (i = 0; i < 16; i++)
        montgomery_mul(acc, acc, acc);
    montgomery_mul(acc, acc, xm);

    /* Convert out of Montgomery form and export as 128-byte big-endian. */
    memset(one, 0, sizeof(one));
    one[0] = 1;
    montgomery_mul(acc, acc, one);
    for (i = 0; i < RSA_LIMBS; i++) {
        uint8_t *p = &out[TIBIA_RSA_SIZE - 4 - 4 * i];
        p[0] = (uint8_t)(acc[i] >> 24);
        p[1] = (uint8_t)(acc[i] >> 16);
        p[2] = (uint8_t)(acc[i] >> 8);
        p[3] = (uint8_t)acc[i];
    }

    return 0;
}

/* ---- PRNG fraco (chaves de sessão) ---- */

static uint32_t rng_state = 0x12345678u;

void tibia_random_seed(void)
{
    rng_state ^= (uint32_t)time(NULL);
    rng_state ^= (uint32_t)(uintptr_t)&rng_state;
    if (!rng_state)
        rng_state = 0xA5A5A5A5u;
}

void tibia_random_bytes(uint8_t *buf, unsigned len)
{
    for (unsigned i = 0; i < len; i++) {
        /* xorshift32 */
        rng_state ^= rng_state << 13;
        rng_state ^= rng_state >> 17;
        rng_state ^= rng_state << 5;
        buf[i] = (uint8_t)(rng_state >> 16);
    }
}

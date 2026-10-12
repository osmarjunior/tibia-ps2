/**
 * login.c — handshake de login do protocolo 7.4 (era OTServ).
 *
 * Formato (fonte: OTHire source/protocollogin.cpp + protocol.cpp):
 *   request  = [u8 0x01][u16 os][u16 version][12 bytes][RSA128]
 *   RSA clear= [0x00][key0..3 u32][acc u32][password string][pad]
 *   response (XTEA) = [0x14 motd] [0x64 charlist...] [u16 premium]
 *   erro     = [0x0A string]
 */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "net_stream.h"
#include "netmsg.h"
#include "xtea.h"
#include "tibia_rsa.h"
#include "login.h"

static LoginServerConfig login_cfg = { "127.0.0.1", 7172 };

void tibia_login_set_server(const char *host, uint16_t port)
{
    login_cfg.host = host;
    login_cfg.port = port;
}

/* Envia o primeiro pacote (login ou game). charname==NULL -> login server. */
int tibia_send_first_packet(int fd, uint16_t version, uint16_t os,
                            const XteaKey *key, const char *account,
                            const char *password, const char *charname)
{
    NetMsg m;
    uint8_t rsa_plain[TIBIA_RSA_SIZE];
    uint8_t rsa_crypt[TIBIA_RSA_SIZE];
    int isGame = (charname != NULL);

    /* corpo (não cifrado) */
    nm_init(&m);
    nm_add_u8(&m, isGame ? 0x0A : 0x01); /* protocol id */
    nm_add_u16(&m, os);
    nm_add_u16(&m, version);
    if (!isGame) {
        memset(rsa_plain, 0, sizeof(rsa_plain));
        nm_add_bytes(&m, rsa_plain, 12); /* 12 bytes ignorados pelo servidor */
    }

    /* bloco RSA: 128 bytes big-endian, primeiro byte 0 */
    memset(rsa_plain, 0, sizeof(rsa_plain));
    int p = 0;
    rsa_plain[p++] = 0x00;
    for (int i = 0; i < 4; i++) {
        uint32_t k = key->k[i];
        rsa_plain[p++] = (uint8_t)(k & 0xFF);
        rsa_plain[p++] = (uint8_t)((k >> 8) & 0xFF);
        rsa_plain[p++] = (uint8_t)((k >> 16) & 0xFF);
        rsa_plain[p++] = (uint8_t)((k >> 24) & 0xFF);
    }
    if (isGame) {
        rsa_plain[p++] = 0x00; /* isGM flag */
        /* u32 account */
        uint32_t acc = (uint32_t)strtoul(account, NULL, 10);
        rsa_plain[p++] = (uint8_t)(acc & 0xFF);
        rsa_plain[p++] = (uint8_t)((acc >> 8) & 0xFF);
        rsa_plain[p++] = (uint8_t)((acc >> 16) & 0xFF);
        rsa_plain[p++] = (uint8_t)((acc >> 24) & 0xFF);
        /* string charname */
        size_t n = strlen(charname);
        rsa_plain[p++] = (uint8_t)(n & 0xFF);
        rsa_plain[p++] = (uint8_t)(n >> 8);
        memcpy(&rsa_plain[p], charname, n);
        p += (int)n;
        /* string password */
        size_t np = strlen(password);
        rsa_plain[p++] = (uint8_t)(np & 0xFF);
        rsa_plain[p++] = (uint8_t)(np >> 8);
        memcpy(&rsa_plain[p], password, np);
        p += (int)np;
    } else {
        /* u32 account number */
        uint32_t acc = (uint32_t)strtoul(account, NULL, 10);
        rsa_plain[p++] = (uint8_t)(acc & 0xFF);
        rsa_plain[p++] = (uint8_t)((acc >> 8) & 0xFF);
        rsa_plain[p++] = (uint8_t)((acc >> 16) & 0xFF);
        rsa_plain[p++] = (uint8_t)((acc >> 24) & 0xFF);
        /* string password */
        size_t np = strlen(password);
        rsa_plain[p++] = (uint8_t)(np & 0xFF);
        rsa_plain[p++] = (uint8_t)(np >> 8);
        memcpy(&rsa_plain[p], password, np);
        p += (int)np;
    }
    /* resto já é zero (padding) */

    tibia_rsa_encrypt(rsa_crypt, rsa_plain);
    nm_add_bytes(&m, rsa_crypt, TIBIA_RSA_SIZE);

    return nm_send(fd, &m, NULL); /* primeiro pacote: sem XTEA */
}

int tibia_login(const char *account, const char *password,
                CharacterListing *outChars)
{
    XteaKey key;
    NetMsg m;
    int fd;
    int got_charlist = 0;

    memset(outChars, 0, sizeof(*outChars));

    fd = net_connect(login_cfg.host, login_cfg.port, 8000);
    if (fd < 0) {
        snprintf(outChars->error, sizeof(outChars->error),
                 "connect falhou em %s:%u", login_cfg.host, login_cfg.port);
        return 0;
    }

    /* chave XTEA de sessão */
    tibia_random_seed();
    tibia_random_bytes((uint8_t *)key.k, 16);

    if (tibia_send_first_packet(fd, 772, 2, &key, account, password, NULL) != 0) {
        net_close(fd);
        snprintf(outChars->error, sizeof(outChars->error), "send do login falhou");
        return 0;
    }

    /* resposta: pacotes XTEA com a key escolhida */
    for (;;) {
        int r = nm_recv(fd, &m, &key, 8000);
        if (r == -3) /* mensagem vazia: ignora */
            continue;
        if (r != 0) {
            net_close(fd);
            snprintf(outChars->error, sizeof(outChars->error),
                     "recv da resposta falhou (%d)", r);
            return 0;
        }

        while (nm_remaining(&m) > 0) {
            uint8_t op = nm_get_u8(&m);
            switch (op) {
            case 0x14: /* MOTD */
                nm_get_string(&m, outChars->motd, sizeof(outChars->motd));
                printf("login: MOTD: %.64s\n", outChars->motd);
                break;

            case 0x64: { /* character list */
                uint8_t count = nm_get_u8(&m);
                if (count > LOGIN_MAX_CHARS)
                    count = LOGIN_MAX_CHARS;
                outChars->count = count;
                for (int i = 0; i < count; i++) {
                    nm_get_string(&m, outChars->chars[i].name,
                                  LOGIN_NAME_LEN);
                    nm_get_string(&m, outChars->chars[i].world,
                                  LOGIN_WORLD_LEN);
                    outChars->chars[i].ip = nm_get_u32(&m);
                    outChars->chars[i].port = nm_get_u16(&m);
                }
                outChars->premiumDays = nm_get_u16(&m);
                got_charlist = 1;
                break;
            }

            case 0x0A: /* erro */
                nm_get_string(&m, outChars->error, sizeof(outChars->error));
                printf("login: erro do servidor: %s\n", outChars->error);
                net_close(fd);
                return 0;

            default:
                printf("login: opcode desconhecido 0x%02X na resposta "
                       "(inner=%d restam=%d) body: ",
                       op, m.len, nm_remaining(&m));
                for (int i = 0; i < m.len && i < 24; i++)
                    printf("%02x ", m.buf[2 + i]);
                printf("\n");
                net_close(fd);
                return 0;
            }
        }

        if (got_charlist)
            break;
    }

    net_close(fd);
    return 1;
}

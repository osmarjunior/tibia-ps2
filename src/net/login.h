/**
 * login.h — handshake de login (RSA + XTEA) contra servidor 7.4.
 */
#ifndef TIBIA_LOGIN_H
#define TIBIA_LOGIN_H

#include <stdint.h>
#include "xtea.h"

#define LOGIN_MAX_CHARS 16
#define LOGIN_NAME_LEN  64
#define LOGIN_WORLD_LEN 32

typedef struct {
    char     name[LOGIN_NAME_LEN];
    char     world[LOGIN_WORLD_LEN];
    uint32_t ip;   /* host byte order */
    uint16_t port;
} CharacterInfo;

typedef struct {
    char     motd[256];
    int      count;
    CharacterInfo chars[LOGIN_MAX_CHARS];
    uint16_t premiumDays;
    char     error[128]; /* preenchido em falha de autenticacao */
} CharacterListing;

/* Configuracao do servidor de login (definida pelo main antes de chamar). */
typedef struct {
    const char *host;
    uint16_t    port;
} LoginServerConfig;
void tibia_login_set_server(const char *host, uint16_t port);

/*
 * Conecta no login server, envia usuario/senha cifrado com RSA,
 * decifra a resposta e preenche a lista de personagens.
 * (Fase 2 do TASKS2.md — sem UI; o chamador imprime via log.)
 */
int tibia_login(const char *account, const char *password,
                CharacterListing *outChars);

/* Versao de baixo nivel (para o game protocol reusar o RSA/XTEA):
 * conecta e envia o primeiro pacote (handshake). Preenche a key XTEA
 * da sessao. Retorna fd ou -1. version = 740 etc. */
int tibia_send_first_packet(int fd, uint16_t version, uint16_t os,
                            const XteaKey *key, const char *account,
                            const char *password, const char *charname);

#endif /* TIBIA_LOGIN_H */

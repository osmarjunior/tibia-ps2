/**
 * game_protocol.h — cliente do protocolo de jogo 7.4 (TFS/OTHire-era).
 *
 * Escreve DIRETAMENTE na GameMap que o renderer consome (Fase 3),
 * expõe estado p/ UI (Fase 8) e callbacks p/ predição/reconciliação
 * (Fase 4). Portátil: host de teste e PS2.
 */
#ifndef GAME_PROTOCOL_H
#define GAME_PROTOCOL_H

#include <stdint.h>
#include "../poc/game_map.h"

#define GP_CHAT_LINES 8
#define GP_CHAT_LEN 96
#define GP_CONTAINERS 4
#define GP_CONT_ITEMS 20
#define GP_INV_SLOTS 10

typedef struct {
    int active;
    uint8_t cid;
    uint16_t itemId;
    char name[24];
    uint8_t capacity, hasParent, count;
    uint16_t items[GP_CONT_ITEMS];   /* client ids */
    uint8_t itemCount[GP_CONT_ITEMS];/* count p/ stackable */
} GpContainer;

typedef struct {
    char lines[GP_CHAT_LINES][GP_CHAT_LEN];
    int head; /* índice circular da próxima linha */
    int count;
} GpChatLog;

/* callbacks (opcional) — registrados pelo cliente/PS2 ou pelo teste */
typedef struct {
    /* passo do próprio player confirmado pelo servidor (0x6D próprio) */
    void (*on_own_move_confirmed)(int fx, int fy, int fz, int tx, int ty, int tz);
    /* servidor rejeitou o passo (0xB5) — reconciliar para a posição confirmada */
    void (*on_own_move_rejected)(void);
    /* teleport / (re)login — posição autoritativa veio de um MapDescription */
    void (*on_own_position_reset)(int x, int y, int z);
    /* troca de andar do próprio player: up!=0 subiu (z-1, x+1, y+1) */
    void (*on_own_floor_change)(int up, int nx, int ny, int nz);
    void (*on_chat)(uint8_t speakType, const char *name, const char *text);
    void (*on_disconnect)(void);
} GpCallbacks;

/* o mapa que o renderer lê — global do protocolo */
extern GameMap gp_map;

/* UI state */
extern GpChatLog gp_chat;
extern GpContainer gp_containers[GP_CONTAINERS];
extern uint16_t gp_inventory[GP_INV_SLOTS]; /* 0 = vazio */

void gp_set_callbacks(const GpCallbacks *cb);

/* Callback de progresso do gp_enter_game (tela de loading) — mesmo
 * formato do ps2_net_set_progress_callback() do ps2_net.c. */
typedef void (*GpProgressCallback)(const char *stage);
void gp_set_progress_callback(GpProgressCallback cb);

/* Fluxo (Fase 2 + Fase 3): login server -> lista de chars -> game server. */
/* 1 = ok. Em falha, gp_error() descreve. */
int gp_enter_game(const char *account, const char *password,
                  const char *charName, const char *loginHost,
                  uint16_t loginPort, const char *gameHost, uint16_t gamePort);
void gp_disconnect(void);
int gp_is_connected(void);
const char *gp_error(void);

/* Processa pacotes que chegaram (até timeout_ms total no primeiro recv;
 * depois drena tudo que já chegou). Retorna nº de mensagens processadas,
 * -1 se a conexão caiu. */
int gp_poll(int timeout_ms);

/* ---- envio (Fase 4/7) ---- */
void gp_send_ping(void);
void gp_send_logout(void);
void gp_send_walk(int dir); /* 0=N 1=E 2=S 3=W 4=NE 5=SE 6=SW 7=NW */
void gp_send_turn(int dir);

/*
 * Fase 4: passo otimista — aplica o movimento LOCALMENTE na janela
 * (scroll + central + thing do player), envia o pedido ao servidor e
 * enfileira até dois passos previstos. Confirmações (0x6D próprio) removem
 * o primeiro da fila; divergência ou 0xB5 revertem as previsões pendentes.
 * Retorna 1 se aplicou.
 */
int gp_walk_optimistic(int dir);

/* 1 se a fila de dois passos previstos está cheia */
int gp_move_pending(void);

/* Chame 1x por frame no loop principal: após 60 frames sem confirmação,
 * reverte a fila para aguardar uma posição autoritativa. */
void gp_move_tick(void);

/* Duração do passo em ms (groundSpeed do tile de destino; Fase 4).
 * Retorna -1 se não há ground speed conhecido (usa default do caller). */
int gp_step_delay_ms(int dir);

/* direção local do player (para o render do outfit) */
int gp_own_dir(void);

/* Diagnóstico de tiles pretos: imprime no log o grid do andar central
 * (# = tem chão) + nº de tiles vazios e o maior thingCount da janela.
 * Chamar numa tecla de debug quando o preto aparecer na tela. */
void gp_debug_dump_window(void);

void gp_send_look_at(uint16_t x, uint16_t y, uint8_t z,
                      uint16_t spriteId, uint8_t stackpos);
void gp_send_use(uint16_t x, uint16_t y, uint8_t z,
                  uint16_t spriteId, uint8_t stackpos, uint8_t index);
void gp_send_use_with(uint16_t fx, uint16_t fy, uint8_t fz, uint16_t fSprite,
                       uint8_t fStack, uint16_t tx, uint16_t ty, uint8_t tz,
                       uint16_t tSprite, uint8_t tStack);
void gp_send_attack(uint32_t creatureId);
void gp_send_say(uint8_t type, const char *text);
void gp_send_container_close(uint8_t cid);
void gp_send_container_up(uint8_t cid);

/* helpers de posição virtual (inventário/container) p/ use/look */
#define GP_POS_INVENTORY_SLOT (0xFFFF)
#define GP_INV_POS(slot) ((uint16_t)(slot))
#define GP_CONT_POS(cid) ((uint16_t)(0x40 | (cid)))

/* stacks de chat p/ UI */
void gp_chat_push(const char *text);
const char *gp_chat_line(int i); /* i = 0..count-1, 0 = mais antiga visível */

#endif /* GAME_PROTOCOL_H */

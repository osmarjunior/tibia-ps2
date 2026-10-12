/**
 * game_map.h — mapa em janela cliente (18x14x16) alimentado pelo game
 * protocol e consumido pelo renderer (render_game.c).
 *
 * TASKS2.md (Fase 3): handlers de pacote escrevem DIRETAMENTE nestas
 * structs — nada de struct paralela de rede. Thing/Tile extendem as
 * structs que o renderer da POC já lia (mock_map.h): os campos originais
 * (spriteId, outfitId, isCreature, isOnTop, groundSpriteId, thingCount,
 * elevation) mantêm nome/semântica; campos novos vêm por último.
 * Cabeçalho 100% portável (sem includes de PS2) — usado também nos
 * testes de host.
 */
#ifndef GAME_MAP_H
#define GAME_MAP_H

#include <stdint.h>

#define NET_MAP_W 18 /* janela que o servidor descreve (x-8..x+9) */
#define NET_MAP_H 14 /* (y-6..y+7) */
#define NET_MAP_FLOORS 16
#define TILE_THINGS_MAX 10 /* protocolo: até 10 things por tile */
#define VIEW_W_TILES 17    /* viewport do renderer (como na POC) */
#define VIEW_H_TILES 11

typedef struct {
    /* ---- formato original (mock_map.h) ---- */
    uint16_t spriteId;   /* item: sprite resolvido (do .dat) */
    uint16_t outfitId;   /* criatura: looktype */
    uint8_t  isCreature;
    uint8_t  isOnTop;
    /* ---- campos novos (Fase 3+) ---- */
    uint16_t itemId;     /* item: client id do protocolo (.dat id) */
    uint32_t creatureId; /* criatura: id do servidor (CreatureMove etc) */
    uint8_t  stackpos;   /* índice no tile (0..9) */
    uint8_t  count;      /* stackable: count (p/ padrão 4x2) */
    uint8_t  dir;        /* direção (criatura) */
    uint8_t  healthPct;  /* criatura: 0..100 */
    char     name[24];   /* criatura: nome (battle list / look) */
    /* animação de walk (modelo Creature::walk do otclient): a criatura
     * fica no tile de DESTINO e o renderer aplica um offset visual
     * que desliza da origem (dir * (pixels-32) → 0). Phase cicla
     * 1..footPhases durante o passo (nunca 0, que é idle). */
    uint8_t  walking;
    int8_t   walkDx, walkDy;
    uint32_t walkStartMs;
    uint16_t walkDurationMs;
} Thing;

typedef struct {
    /* ---- formato original ---- */
    uint16_t groundSpriteId;
    uint8_t  thingCount;
    uint8_t  elevation;
    /* ---- campos novos ---- */
    Thing    things[TILE_THINGS_MAX];
    uint16_t groundItemId; /* client id do chão (0 se vazio) */
    uint8_t  isDirty;      /* RT invalidation hint */
    /* oclusão de floors (Fase 5) — calculado no parse */
    uint8_t  blocksView;        /* chão ou onBottom+blockProjectile */
    uint8_t  blocksProjectile;  /* algum thing com blockProjectile */
} Tile;

typedef struct {
    /* tiles[z][y][x]; (x,y) locais à janela; origem = canto NW */
    Tile tiles[NET_MAP_FLOORS][NET_MAP_H][NET_MAP_W];

    int originX, originY; /* world coords do tile [z][0][0] */
    int playerFloor;       /* z do player */
    int playerTileX, playerTileY; /* world coords do player */
    int hasPlayer;

    uint32_t playerId;
    uint16_t playerSpeed;      /* stepSpeed do AddCreature próprio */
    uint16_t playerHealth, playerMaxHealth;
    uint16_t playerMana, playerMaxMana;
    uint16_t playerLevel;
    uint32_t playerExperience;

    /* previsão de passo (Fase 4): posição confirmada pelo servidor */
    int confirmedTileX, confirmedTileY;
    int confirmedFloor;
} GameMap;

/* Converte world -> índice local (retorna 0 se fora da janela). */
static inline int gm_local_x(const GameMap *m, int wx)
{
    return (wx >= m->originX && wx < m->originX + NET_MAP_W) ? wx - m->originX : -1;
}
static inline int gm_local_y(const GameMap *m, int wy)
{
    return (wy >= m->originY && wy < m->originY + NET_MAP_H) ? wy - m->originY : -1;
}

/* Helpers usados pelos handlers (definidos em game_protocol.c). */

/* Adiciona um item ao tile (no fim da lista). 0 ok, -1 cheio. */
int gm_tile_add_item(Tile *t, uint16_t itemId, uint16_t spriteId,
                      uint8_t count, uint8_t stackpos);

/* Adiciona/replace criatura no tile. */
int gm_tile_add_creature(Tile *t, uint32_t creatureId, uint16_t outfitId,
                          uint8_t dir, uint8_t healthPct, const char *name,
                          uint8_t stackpos);

/* Remove thing por stackpos (compacta a lista). 0 ok. */
int gm_tile_remove_stackpos(Tile *t, uint8_t stackpos);

/* Remove criatura por id (em qualquer stackpos). 0 ok. */
int gm_tile_remove_creature(Tile *t, uint32_t creatureId);

/* Encontra criatura por id no tile; preenche *stackpos. NULL se não achar. */
Thing *gm_tile_find_creature(Tile *t, uint32_t creatureId, uint8_t *stackpos);

/* Limpa o tile inteiro. */
void gm_tile_clear(Tile *t);

/* Move a janela do mapa (roll) preservando tiles que continuam visíveis. */
void gm_scroll(GameMap *m, int dx, int dy);

/* Relógio portátil em ms (PS2: rs2_now; host: clock_gettime). */
uint32_t gm_now_ms(void);

/* Zera o mapa todo. */
void gm_clear(GameMap *m);

#endif /* GAME_MAP_H */

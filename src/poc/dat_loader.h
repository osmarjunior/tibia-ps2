/**
 * dat_loader.h — parser do Tibia.dat 7.60.
 *
 * O DAT substituído mantém a assinatura 0x439D5A33 e o layout de thing
 * com pattern_z explícito. Itens usam IDs visuais 7.60 (gold coin = 3031;
 * IDs 100..5089),
 * seguidos por outfits, efeitos e mísseis.
 */
#ifndef DAT_LOADER_H
#define DAT_LOADER_H

#include <stdint.h>

/* flags de thing (bits de thingFlags) */
#define DATF_GROUND         (1u << 0)
#define DATF_GROUNDBORDER   (1u << 1)
#define DATF_ONBOTTOM        (1u << 2)
#define DATF_ONTOP           (1u << 3)
#define DATF_CONTAINER       (1u << 4)
#define DATF_STACKABLE       (1u << 5)
#define DATF_FORCEUSE        (1u << 6)
#define DATF_MULTIUSE        (1u << 7)
#define DATF_FLUIDCONTAINER  (1u << 8)
#define DATF_SPLASH          (1u << 9)
#define DATF_NOTWALKABLE     (1u << 10)
#define DATF_NOTMOVEABLE     (1u << 11)
#define DATF_BLOCKPROJECTILE (1u << 12)
#define DATF_NOTPATHABLE     (1u << 13)
#define DATF_PICKUPABLE      (1u << 14)
#define DATF_LIGHT           (1u << 15)
#define DATF_TRANSULENT      (1u << 16)
#define DATF_DISPLACEMENT    (1u << 17)
#define DATF_ELEVATION       (1u << 18)
#define DATF_LYINGCORPSE     (1u << 19)
#define DATF_ANIMATEALWAYS   (1u << 20)
#define DATF_FULLGROUND      (1u << 21)
#define DATF_DONTHIDE        (1u << 22)
#define DATF_ROTATEABLE      (1u << 23)
#define DATF_HANGABLE        (1u << 24)
#define DATF_HOOKSOUTH       (1u << 25)
#define DATF_HOOKEAST        (1u << 26)
#define DATF_FLOORCHANGE     (1u << 27)

#define DAT_ITEM_FIRST 100
#define TILE_SPRITE_MAX 4 /* peções w*h máx. que o renderer desenha */

typedef struct {
    uint32_t flags;
    uint16_t groundSpeed; /* DATF_GROUND */
    uint16_t elevation;   /* DATF_ELEVATION */
    uint8_t  w, h, layers, px, py, pz, phases;
    uint16_t spriteCount;
    const uint16_t *sprites; /* spriteCount entradas */
} DatThingInfo;

void dat_loader_init(void);

/* item por client ID do DAT (100..5089). NULL se inválido. */
const DatThingInfo *dat_get_item(int item_id);

/* outfit por looktype (1..n). NULL se inválido. */
const DatThingInfo *dat_get_outfit_info(int outfit_id);

/* API da POC (mantida — usada pelo render de outfits). */
uint16_t dat_outfit_sprite(int outfit_id, int direction, int phase);
int dat_get_outfit_count(void);

/* Peças de sprite de uma criatura (outfits 1x1 e 2x2) na direção/fase.
 * Preenche out[] (até TILE_SPRITE_MAX peças) e as dimensões. */
int dat_creature_sprites(int outfit_id, int direction, int phase,
                          uint16_t out[TILE_SPRITE_MAX], int *out_w, int *out_h);

/*
 * Resolve as peças de sprite para desenhar um thing num tile.
 * count: pilha de stackable (0 = ignora padrão de count).
 * (wx, wy, wz): posição do tile — decide os padrões de ground.
 * Preenche out[] (até TILE_SPRITE_MAX peças, ordem de leitura do .dat),
 * out_w/out_h com as dimensões do thing. Retorna o nº de peças ou 0.
 */
int dat_resolve_sprites(const DatThingInfo *t, uint8_t count,
                          int wx, int wy, int wz,
                          uint16_t out[TILE_SPRITE_MAX],
                          int *out_w, int *out_h);

/* como acima, mas com fase de animação e camada (blend layer) explícitas.
 * Itens multi-camada (walls parede/telhado, stairs degraus, portas) têm
 * a figura real na layer 1: resolva e desenhe TODAS as layers 0..n-1
 * sobrepostas no mesmo âncora — o otclient pré-compõe exatamente assim. */
int dat_resolve_sprites_anim(const DatThingInfo *t, uint8_t count,
                              int wx, int wy, int wz, int phase, int layer,
                              uint16_t out[TILE_SPRITE_MAX],
                              int *out_w, int *out_h);
/* Helpers de flag */
int dat_item_is_stackable(int item_id);
int dat_item_is_container(int item_id);
int dat_item_is_pickupable(int item_id);

#endif /* DAT_LOADER_H */

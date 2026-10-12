/**
 * spr_loader.h - Runtime Tibia 7.60 .spr decoder + cache de texturas p/ PS2
 *
 * Fase 6 do TASKS2.md: instrumentação do cache (texturas residentes,
 * cache-miss por frame, evictions/s) p/ decidir atlas com dados reais.
 */
#ifndef SPR_LOADER_H
#define SPR_LOADER_H

#include <kernel.h>
#include <gsKit.h>
#include <tamtypes.h>
#include <stdint.h>

/* Estatísticas do cache (Fase 6) */
typedef struct {
    int resident;      /* texturas únicas residentes agora */
    int capacity;      /* tamanho do cache */
    int misses_total;  /* loads desde o início (== uploads) */
    int misses_frame; /* misses neste frame */
    int hits_frame;    /* hits neste frame */
    int evictions;     /* despejos desde o início */
    int evict_window;  /* despejos desde o último spr_stats_tick */
    int starved;   /* misses sem slot elegível (working set > capacidade) */
    int deferred;  /* misses adiados pelo limite de uploads por frame */
} SprStats;

void spr_loader_init(void);
GSTEXTURE *spr_load_texture(GSGLOBAL *gsGlobal, uint16_t sprite_id);
void spr_draw(GSGLOBAL *gsGlobal, uint16_t sprite_id, float x, float y, float size);

/* Variante com alpha explícito (fading de floors — Fase 5).
 * alpha: 0..255 (255 = opaco). Requer blending ativo no primalpha. */
void spr_draw_alpha(GSGLOBAL *gsGlobal, uint16_t sprite_id, float x, float y,
                     float size, uint8_t alpha);

uint16_t spr_get_count(void);

/* ---- instrumentação (Fase 6) ---- */
void spr_stats_frame(void);          /* zera contadores por-frame */
void spr_stats_tick(void);           /* janela de 1s p/ evictions/s */
const SprStats *spr_get_stats(void);

#endif /* SPR_LOADER_H */

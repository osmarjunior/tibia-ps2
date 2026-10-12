/**
 * dat_loader.c — parser do Tibia.dat 7.60 para PS2 (e host de teste).
 *
 * O parser da POC pulava as flags escaneando até 0xFF e só lia outfits.
 * Esta versão (Fase 3 do TASKS2.md) decodifica as flags de verdade
 * (larguras validadas por consumo exato do arquivo), parseia TODOS os
 * itens (flags + sprites) e mantém os outfits. A API de outfits que o
 * renderer da POC usava continua idêntica.
 */
#include <stdint.h>
#include <stdio.h>
#include <string.h>

#include "dat_loader.h"

extern unsigned char _binary_Tibia_dat_start[];

/*
 * O arquivo é auto-delimitado pelas contagens do header (validado por
 * consumo exato em docs/protocol-74-notes.md). O limite abaixo só serve
 * de trava contra dados corrompidos — os testes de host carregam o
 * arquivo num buffer com folga.
 */
#define DAT_MAX_SCAN (256 * 1024)

#define MAX_ITEMS 5200
#define MAX_OUTFITS 260
#define MAX_ITEM_SPRITES 65536
#define OUTFIT_SPRITE_STORE 64

/* memória estática (EE tem 32MB; host de teste também é folgado) */
static DatThingInfo items[MAX_ITEMS];
static int item_count = 0; /* nº de entradas (ids 100..) */
static int item_first_id = 100;
static uint16_t item_sprites_store[MAX_ITEM_SPRITES];
static int item_sprites_used = 0;

static DatThingInfo outfits[MAX_OUTFITS];
static uint16_t outfit_sprites_store[MAX_OUTFITS][OUTFIT_SPRITE_STORE];
static int outfit_count = 0;

static int dat_loaded = 0;

static inline uint16_t rd16(const unsigned char *p)
{
    return (uint16_t)(p[0] | (p[1] << 8));
}

/* ---- flags no formato DAT 7.60 (enum de atributos do RME/otclient) ---- */

static int parse_flags(const unsigned char **pp, uint32_t *flags,
                        uint16_t *groundSpeed, uint16_t *elevation)
{
    const unsigned char *p = *pp;
    *flags = 0;
    *groundSpeed = 0;
    *elevation = 0;

    for (;;) {
        uint8_t a = *p++;
        if (a == 0xFF)
            break;
        if (a == 23) /* este DAT ainda codifica FloorChange como 23 */
            a = 252;

        switch (a) {
        case 0x00: *flags |= DATF_GROUND;    *groundSpeed = rd16(p); p += 2; break;
        case 0x01: *flags |= DATF_GROUNDBORDER; break;
        case 0x02: *flags |= DATF_ONBOTTOM;  break;
        case 0x03: *flags |= DATF_ONTOP;      break;
        case 0x04: *flags |= DATF_CONTAINER;  break;
        case 0x05: *flags |= DATF_STACKABLE;  break;
        case 0x06: *flags |= DATF_FORCEUSE;   break;
        case 0x07: *flags |= DATF_MULTIUSE;   break;
        case 0x08: /* writable (u16) */        p += 2; break;
        case 0x09: /* writableOnce (u16) */    p += 2; break;
        case 0x0A: *flags |= DATF_FLUIDCONTAINER; break;
        case 0x0B: *flags |= DATF_SPLASH;     break;
        case 0x0C: *flags |= DATF_NOTWALKABLE; break;
        case 0x0D: *flags |= DATF_NOTMOVEABLE; break;
        case 0x0E: *flags |= DATF_BLOCKPROJECTILE; break;
        case 0x0F: *flags |= DATF_NOTPATHABLE; break;
        case 0x10: *flags |= DATF_PICKUPABLE; break;
        case 0x11: *flags |= DATF_HANGABLE;   break;
        case 0x12: *flags |= DATF_HOOKSOUTH;  break;
        case 0x13: *flags |= DATF_HOOKEAST;   break;
        case 0x14: *flags |= DATF_ROTATEABLE; break;
        case 0x15: *flags |= DATF_LIGHT;      p += 4; break;
        case 0x16: *flags |= DATF_DONTHIDE;   break;
        case 0x17: *flags |= DATF_TRANSULENT; break;
        case 0x18: *flags |= DATF_DISPLACEMENT; p += 4; break;
        case 0x19: *flags |= DATF_ELEVATION;  *elevation = rd16(p); p += 2; break;
        case 0x1A: *flags |= DATF_LYINGCORPSE; break;
        case 0x1B: *flags |= DATF_ANIMATEALWAYS; break;
        case 0x1C: /* minimap color */        p += 2; break;
        case 0x1D: /* lens help */            p += 2; break;
        case 0x1E: *flags |= DATF_FULLGROUND; break;
        case 0x1F: /* look */                  break;
        case 0x20: /* cloth */                 p += 2; break;
        case 0x22: /* usable */                p += 2; break;
        case 252:  *flags |= DATF_FLOORCHANGE; break;
        default:
            printf("dat: flag desconhecida 0x%02X (pos %d)\n", a,
                   (int)(p - _binary_Tibia_dat_start));
            return -1;
        }
    }

    *pp = p;
    return 0;
}

/*
 * Lê um thing completo (header: w, h, [realsize], layers, px, py,
 * pz, phases + sprites). p já aponta PARA O HEADER (flags já consumidas
 * pelo chamador). Retorna ponteiro após o thing, ou NULL se estourar.
 */
static const unsigned char *parse_thing_header(const unsigned char *p,
                                                const unsigned char *end,
                                                uint8_t *w, uint8_t *h,
                                                uint8_t *layers, uint8_t *px,
                                                uint8_t *py, uint8_t *pz,
                                                uint8_t *phases,
                                                uint32_t *sprite_total)
{
    if (p + 6 > end)
        return NULL;
    *w = *p++; *h = *p++;
    if (*w > 1 || *h > 1)
        p++;
    *layers = *p++; *px = *p++; *py = *p++; *pz = *p++; *phases = *p++;
    *sprite_total = (uint32_t)(*w) * (*h) * (*layers) * (*px) * (*py) *
                    (*pz) * (*phases);
    if (p + (int)(*sprite_total) * 2 > end)
        return NULL;
    return p;
}

void dat_loader_init(void)
{
    if (dat_loaded)
        return;

    const unsigned char *dat = _binary_Tibia_dat_start;
    const unsigned char *end = dat + DAT_MAX_SCAN;

    uint16_t item_max = rd16(dat + 4);
    uint16_t outfit_total = rd16(dat + 6);
    uint16_t effect_total = rd16(dat + 8);
    uint16_t missile_total = rd16(dat + 10);

    const unsigned char *p = dat + 12;

    /* ---- itens (IDs de cliente; 7.60 usa 100..5089) ---- */
    item_count = 0;
    if ((int)item_max - item_first_id + 1 > MAX_ITEMS) {
        printf("dat: tabela de itens pequena (%d < %d)\n", MAX_ITEMS,
               (int)item_max - item_first_id + 1);
        return;
    }
    for (int id = item_first_id; id <= item_max && item_count < MAX_ITEMS; id++) {
        uint32_t flags; uint16_t gspeed, elev;
        uint8_t w, h, l, px, py, pz, ph;
        uint32_t n;

        const unsigned char *pflags = p;
        if (parse_flags(&pflags, &flags, &gspeed, &elev) != 0)
            break;
        p = parse_thing_header(pflags, end, &w, &h, &l, &px, &py, &pz, &ph, &n);
        if (!p)
            break;

        DatThingInfo *it = &items[item_count];
        it->flags = flags;
        it->groundSpeed = gspeed;
        it->elevation = elev;
        it->w = w; it->h = h; it->layers = l;
        it->px = px; it->py = py; it->pz = pz; it->phases = ph;
        it->spriteCount = (uint16_t)n;
        it->sprites = &item_sprites_store[item_sprites_used];


        if (item_sprites_used + (int)n > MAX_ITEM_SPRITES) {
            printf("dat: buffer de sprites de item cheio (item %d)\n", id);
            it->spriteCount = 0;
            it->sprites = NULL;
        }
        for (uint32_t i = 0; i < n; i++) {
            if (it->sprites)
                item_sprites_store[item_sprites_used++] = rd16(p);
            p += 2;
        }
        item_count++;
    }

    /* ---- outfits (looktype 1..outfit_total) ---- */
    outfit_count = 0;
    for (int i = 0; i < outfit_total && outfit_count < MAX_OUTFITS; i++) {
        uint32_t flags; uint16_t gspeed, elev;
        uint8_t w, h, l, px, py, pz, ph;
        uint32_t n;

        const unsigned char *pflags = p;
        if (parse_flags(&pflags, &flags, &gspeed, &elev) != 0)
            break;
        p = parse_thing_header(pflags, end, &w, &h, &l, &px, &py, &pz, &ph, &n);
        if (!p)
            break;

        DatThingInfo *o = &outfits[outfit_count];
        o->flags = flags;
        o->groundSpeed = gspeed;
        o->elevation = elev;
        o->w = w; o->h = h; o->layers = l;
        o->px = px; o->py = py; o->pz = pz; o->phases = ph;
        o->spriteCount = (uint16_t)((n <= OUTFIT_SPRITE_STORE) ? n : 0);
        o->sprites = outfit_sprites_store[outfit_count];

        for (uint32_t j = 0; j < n; j++) {
            if (j < OUTFIT_SPRITE_STORE)
                outfit_sprites_store[outfit_count][j] = rd16(p);
            p += 2;
        }
        outfit_count++;
    }

    /* ---- efeitos e mísseis: pular ---- */
    for (int i = 0; i < effect_total + missile_total; i++) {
        uint32_t flags; uint16_t gspeed, elev;
        uint8_t w, h, l, px, py, pz, ph;
        uint32_t n;

        const unsigned char *pflags = p;
        if (parse_flags(&pflags, &flags, &gspeed, &elev) != 0)
            break;
        p = parse_thing_header(pflags, end, &w, &h, &l, &px, &py, &pz, &ph, &n);
        if (!p)
            break;
        p += (int)(n * 2);
    }

    printf("dat: %d itens (id %d..%d), %d outfits, %d sprites de item\n",
           item_count, item_first_id, item_first_id + item_count - 1,
           outfit_count, item_sprites_used);
    dat_loaded = 1;
}

const DatThingInfo *dat_get_item(int item_id)
{
    int idx = item_id - item_first_id;
    if (idx < 0 || idx >= item_count)
        return NULL;
    return &items[idx];
}

const DatThingInfo *dat_get_outfit_info(int outfit_id)
{
    int idx = outfit_id - 1;
    if (idx < 0 || idx >= outfit_count)
        return NULL;
    return &outfits[idx];
}

uint16_t dat_outfit_sprite(int outfit_id, int direction, int phase)
{
    const DatThingInfo *o = dat_get_outfit_info(outfit_id);
    if (!o || o->spriteCount == 0)
        return 0;
    if (direction >= o->px)
        direction = 0;
    if (phase >= o->phases)
        phase = 0;
    /* fórmula otclient (1x1: a*pz? ver resolve) */
    int index = (phase * o->px + direction) * o->layers;
    if (index < 0 || index >= o->spriteCount)
        return 0;
    return o->sprites[index];
}

int dat_creature_sprites(int outfit_id, int direction, int phase,
                          uint16_t out[TILE_SPRITE_MAX], int *out_w, int *out_h)
{
    const DatThingInfo *o = dat_get_outfit_info(outfit_id);
    if (!o || o->spriteCount == 0)
        return 0;
    if (direction < 0 || direction >= o->px)
        direction = 0;
    if (phase < 0 || phase >= o->phases)
        phase = 0;

    *out_w = o->w;
    *out_h = o->h;
    int pieces = o->w * o->h;
    if (pieces > TILE_SPRITE_MAX)
        pieces = TILE_SPRITE_MAX;

    /* index = ((((a*pz + z)*py + y)*px + x)*layers + l)*H + h)*W + w
     * com z=y=0, l=0, x=direction, a=phase */
    for (int i = 0; i < pieces; i++) {
        int iy = i / o->w;
        int ix = i % o->w;
        int idx = ((((((phase * o->pz) * o->py) * o->px + direction)
                      * o->layers) * o->h + iy) * o->w + ix);
        if (idx < 0 || idx >= o->spriteCount)
            out[i] = 0;
        else
            out[i] = o->sprites[idx];
    }
    return pieces;
}

int dat_resolve_sprites(const DatThingInfo *t, uint8_t count,
                           int wx, int wy, int wz,
                           uint16_t out[TILE_SPRITE_MAX],
                           int *out_w, int *out_h)
{
    return dat_resolve_sprites_anim(t, count, wx, wy, wz, 0, 0, out,
                                    out_w, out_h);
}

int dat_resolve_sprites_anim(const DatThingInfo *t, uint8_t count,
                              int wx, int wy, int wz, int phase, int layer,
                              uint16_t out[TILE_SPRITE_MAX],
                              int *out_w, int *out_h)
{
    if (!t || t->spriteCount == 0)
        return 0;
    if (layer < 0 || layer >= t->layers)
        return 0;

    int px = t->px, py = t->py, pz = t->pz;
    int xpat = 0, ypat = 0, zpat = 0;

    if ((t->flags & DATF_STACKABLE) && px == 4 && py == 2) {
        /* padrão de count (fórmula do otclient Item) */
        if (count == 0) { xpat = 0; ypat = 0; }
        else if (count < 5) { xpat = count - 1; ypat = 0; }
        else if (count < 10) { xpat = 0; ypat = 1; }
        else if (count < 25) { xpat = 1; ypat = 1; }
        else if (count < 50) { xpat = 2; ypat = 1; }
        else { xpat = 3; ypat = 1; }
    } else {
        if (px <= 0) px = 1;
        if (py <= 0) py = 1;
        if (pz <= 0) pz = 1;
        int ax = wx, ay = wy, az = wz;
        if (ax < 0) ax = -ax;
        if (ay < 0) ay = -ay;
        if (az < 0) az = -az;
        xpat = ax % px;
        ypat = ay % py;
        zpat = az % pz;
    }

    if (phase < 0)
        phase = 0;
    if (t->phases > 0)
        phase %= t->phases;

    *out_w = t->w;
    *out_h = t->h;

    /* index = ((((a*pz + z)*py + y)*px + x)*layers + l)*H + h)*W + w.
     * Peças do tile (h,w): no sheet do .dat a peça (0,0) é a do tile
     * âncora (bottom-right) — mapeamento do otclient. */
    int pieces = t->w * t->h;
    if (pieces > TILE_SPRITE_MAX)
        pieces = TILE_SPRITE_MAX;

    for (int i = 0; i < pieces; i++) {
        int iy = i / t->w;
        int ix = i % t->w;
        int idx = ((((((phase * pz + zpat) * py + ypat) * px + xpat)
                      * t->layers + layer) * t->h + iy) * t->w + ix);
        if (idx < 0 || idx >= t->spriteCount)
            out[i] = 0;
        else
            out[i] = t->sprites[idx];
    }
    return pieces;
}

int dat_item_is_stackable(int item_id)
{
    const DatThingInfo *t = dat_get_item(item_id);
    return t && (t->flags & DATF_STACKABLE);
}

int dat_item_is_container(int item_id)
{
    const DatThingInfo *t = dat_get_item(item_id);
    return t && (t->flags & DATF_CONTAINER);
}

int dat_item_is_pickupable(int item_id)
{
    const DatThingInfo *t = dat_get_item(item_id);
    return t && (t->flags & DATF_PICKUPABLE);
}

int dat_get_outfit_count(void)
{
    return outfit_count;
}

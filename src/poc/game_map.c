/**
 * game_map.c — implementação dos helpers de GameMap.
 * Portátil (host e PS2).
 *
 * Convenção de stackpos (igual ao servidor): ground = 0; things[i] do
 * array tem stackpos = i + (temGround ? 1 : 0). O parser passa o valor
 * explícito quando o conhece (MapDescription); remoções/moves chegam do
 * servidor com o stackpos do protocolo, então removemos procurando pelo
 * valor gravado.
 */
#include <string.h>
#include <time.h>
#include "game_map.h"

#ifdef __PS2__
/* ps2sdk: relógio de hardware (iniciado por ps2_net_timer_init/ps2.c
 * antes de qualquer uso — walk só roda após o login, bem depois). */
#include <tamtypes.h>
#include <timer.h>
#endif

uint32_t gm_now_ms(void)
{
#ifdef __PS2__
    u64 clocks = GetTimerSystemTime();
    u32 sec, usec;
    TimerBusClock2USec(clocks, &sec, &usec);
    return (uint32_t)(sec * 1000 + usec / 1000);
#else
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return (uint32_t)(ts.tv_sec * 1000 + ts.tv_nsec / 1000000);
#endif
}

int gm_tile_add_item(Tile *t, uint16_t itemId, uint16_t spriteId,
                      uint8_t count, uint8_t stackpos)
{
    if (t->thingCount >= TILE_THINGS_MAX)
        return -1;
    Thing *th = &t->things[t->thingCount];
    memset(th, 0, sizeof(*th));
    th->spriteId = spriteId;
    th->itemId = itemId;
    th->count = count;
    th->stackpos = stackpos;
    t->thingCount++;
    return 0;
}

int gm_tile_add_creature(Tile *t, uint32_t creatureId, uint16_t outfitId,
                          uint8_t dir, uint8_t healthPct, const char *name,
                          uint8_t stackpos)
{
    if (t->thingCount >= TILE_THINGS_MAX)
        return -1;
    Thing *th = &t->things[t->thingCount];
    memset(th, 0, sizeof(*th));
    th->isCreature = 1;
    th->creatureId = creatureId;
    th->outfitId = outfitId;
    th->dir = dir;
    th->healthPct = healthPct;
    th->stackpos = stackpos;
    if (name) {
        strncpy(th->name, name, sizeof(th->name) - 1);
        th->name[sizeof(th->name) - 1] = 0;
    }
    t->thingCount++;
    return 0;
}

static void gm_renumber(Tile *t)
{
    int off = (t->groundItemId != 0) ? 1 : 0;
    for (int i = 0; i < t->thingCount; i++)
        t->things[i].stackpos = (uint8_t)(i + off);
}

int gm_tile_remove_stackpos(Tile *t, uint8_t stackpos)
{
    int idx = -1;
    if (t->groundItemId != 0 && stackpos == 0) {
        /* remover o chão é raro (update de tile cuida disso) */
        t->groundItemId = 0;
        t->groundSpriteId = 0;
        gm_renumber(t);
        return 0;
    }
    for (int i = 0; i < t->thingCount; i++) {
        if (t->things[i].stackpos == stackpos) {
            idx = i;
            break;
        }
    }
    if (idx < 0)
        return -1;
    for (int i = idx; i < t->thingCount - 1; i++)
        t->things[i] = t->things[i + 1];
    t->thingCount--;
    gm_renumber(t);
    return 0;
}

int gm_tile_remove_creature(Tile *t, uint32_t creatureId)
{
    for (int i = 0; i < t->thingCount; i++) {
        if (t->things[i].isCreature && t->things[i].creatureId == creatureId) {
            for (int j = i; j < t->thingCount - 1; j++)
                t->things[j] = t->things[j + 1];
            t->thingCount--;
            gm_renumber(t);
            return 0;
        }
    }
    return -1;
}

Thing *gm_tile_find_creature(Tile *t, uint32_t creatureId, uint8_t *stackpos)
{
    for (int i = 0; i < t->thingCount; i++) {
        if (t->things[i].isCreature && t->things[i].creatureId == creatureId) {
            if (stackpos)
                *stackpos = t->things[i].stackpos;
            return &t->things[i];
        }
    }
    return 0;
}

void gm_tile_clear(Tile *t)
{
    t->thingCount = 0;
    t->groundSpriteId = 0;
    t->groundItemId = 0;
    t->elevation = 0;
    /* flags derivadas também zeram: sem isto o gm_scroll deixava
     * blocksView/blocksProjectile HERDADOS do conteúdo antigo da célula
     * nas faixas novas (e o save/restore da predição não podia pular
     * tiles vazios, pois restaurá-los nem sempre equivaleria a limpá-los) */
    t->blocksView = 0;
    t->blocksProjectile = 0;
    t->isDirty = 0;
}

void gm_scroll(GameMap *m, int dx, int dy)
{
    static Tile tmp[NET_MAP_FLOORS][NET_MAP_H][NET_MAP_W];

    if (dx == 0 && dy == 0)
        return;

    for (int z = 0; z < NET_MAP_FLOORS; z++)
        for (int y = 0; y < NET_MAP_H; y++)
            for (int x = 0; x < NET_MAP_W; x++)
                tmp[z][y][x] = m->tiles[z][y][x];

    /* origin += (dx,dy): o tile mundial que estava em local L passa a
     * estar em L-(dx,dy) => novo[L] = antigo[L+(dx,dy)] */
    for (int z = 0; z < NET_MAP_FLOORS; z++)
        for (int y = 0; y < NET_MAP_H; y++)
            for (int x = 0; x < NET_MAP_W; x++) {
                int sx = x + dx, sy = y + dy;
                if (sx >= 0 && sx < NET_MAP_W && sy >= 0 && sy < NET_MAP_H)
                    m->tiles[z][y][x] = tmp[z][sy][sx];
                else
                    gm_tile_clear(&m->tiles[z][y][x]);
            }

    m->originX += dx;
    m->originY += dy;
}

void gm_clear(GameMap *m)
{
    memset(m, 0, sizeof(*m));
}

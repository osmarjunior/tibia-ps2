/**
 * render_game.c — renderer do GameMap (protocolo de rede) para PS2.
 *
 * Convenções (ver docs/protocol-74-notes.md):
 *  - window[z][ly][lx] contém o tile mundial que RENDERIZA na posição
 *    (lx-8, ly-6) relativa ao centro — o empilhamento diagonal entre
 *    floors já veio "pré-deslocado" do servidor; nenhum shift extra aqui.
 *  - Ordem: floor mais fundo (maior z) primeiro, mais alto por último
 *    (os floors de cima pintam sobre os de baixo — projeção do otclient).
 *  - Elevation do tile desloca itens/criaturas para cima.
 *
 * compute_floor_range verifica os tiles cobertos acima do player e dos
 * oito vizinhos; chão em um floor superior oculta os floors ainda mais
 * altos. Em caves, o andar do player é o topo da pilha (abaixo dele,
 * janela de awareness de +2 floors).
 */
#include <kernel.h>
#include <gsKit.h>
#include <tamtypes.h>
#include <string.h>
#include <stdio.h>

#include "render_game.h"
#include "map_rt.h" /* VIEW_X/Y/W/H do viewport */
#include "spr_loader.h"
#include "dat_loader.h"

#define TILE_SIZE 32
/* viewport do POC: 17x11 tiles centrados no player (window 8,6) */
#define CAM_LOCAL_X 8
#define CAM_LOCAL_Y 6
#define VIEW_TILES_X 17
#define VIEW_TILES_Y 11

/* margem p/ peças multi-tile e elevação desenharem além do viewport */
#define MARGIN_TILES 2

#define SEA_FLOOR 7
#define AWARE_UNDERGROUND 2

/* Frames de render por fase de animação de item (~500ms a 60fps,
 * valor do ITEM_TICKS_PER_FRAME do otclient / Tibia 7.x). */
#define ITEM_ANIM_FRAMES 30

/* ---------------- compute_floor_range (alinhado ao otclient) ----------------
 * O otclient (mapview.cpp::calcFirstVisibleFloor) varre os 8 vizinhos
 * ao redor do player + o próprio tile, subindo em diagonal (+1,+1 por piso).
 * Um ground que NÃO tem DontHide (flag 0x16) em qualquer desses tiles
 * limita a visão — floors acima ficam ocultos (ou em fade).
 * Caves (playerFloor > SEA_FLOOR): nada acima do player é desenhado
 * (o andar dele é o topo, como no cliente 7.x); abaixo, janela
 * +AWARE_UNDERGROUND. Surface: first = 0, sobe até achar teto. */

static int fade_from = -1, fade_to = -1;
static int fade_in_from = -1, fade_in_to = -1;
static int fade_step = 16;
static int prev_first = -1;

static int floor_alpha(int z)
{
    if (fade_step >= 16)
        return 255;
    int a = (fade_step * 255) / 16;
    if (z >= fade_from && z <= fade_to)
        return 255 - a;
    if (z >= fade_in_from && z <= fade_in_to)
        return a;
    return 255;
}

static void fade_advance(void)
{
    if (fade_step < 16)
        fade_step++;
    else
        fade_from = fade_to = fade_in_from = fade_in_to = -1;
}

static Tile *tile_world(GameMap *m, int wx, int wy, int wz)
{
    if (wz < 0 || wz >= NET_MAP_FLOORS)
        return NULL;
    int lx = wx - (m->playerTileX + (m->playerFloor - wz) - CAM_LOCAL_X);
    int ly = wy - (m->playerTileY + (m->playerFloor - wz) - CAM_LOCAL_Y);
    if (lx < 0 || lx >= NET_MAP_W || ly < 0 || ly >= NET_MAP_H)
        return NULL;
    return &m->tiles[wz][ly][lx];
}

void compute_floor_range(GameMap *map, FloorRange *out)
{
    int camZ = map->playerFloor;
    int first, last;

    /* último andar visível (igual ao otclient) */
    if (camZ > SEA_FLOOR) {
        last = camZ + AWARE_UNDERGROUND;
        if (last > NET_MAP_FLOORS - 1)
            last = NET_MAP_FLOORS - 1;
    } else {
        last = SEA_FLOOR;
    }

    if (camZ > SEA_FLOOR) {
        first = camZ; /* underground: o andar do player é o topo da pilha */
    } else {
        first = 0;

        /* Varre 3x3 ao redor do player (incluindo o tile do player),
         * subindo diagonalmente: tile (wx, wy, z) projeta sobre (wx+1, wy+1, z-1).
         * O primeiro ground sem DontHide em QUALQUER desses 9 tiles oculta
         * os andares acima. */
        for (int z = camZ - 1; z >= first; z--) {
            int offset = camZ - z;
            int hit_roof = 0;
            for (int dx = -1; dx <= 1 && !hit_roof; dx++) {
                for (int dy = -1; dy <= 1 && !hit_roof; dy++) {
                    Tile *covered = tile_world(map,
                        map->playerTileX + offset + dx,
                        map->playerTileY + offset + dy, z);
                    if (!covered || covered->groundItemId == 0)
                        continue;
                    const DatThingInfo *ground = dat_get_item(covered->groundItemId);
                    if (!ground || !(ground->flags & DATF_DONTHIDE)) {
                        hit_roof = 1;
                    }
                }
            }
            if (hit_roof) {
                first = z + 1;
                break;
            }
        }
    }

    if (first > last)
        first = last;
    if (first < 0)
        first = 0;
    out->first = first;
    out->last = last;

    /* fade transition */
    if (prev_first == -1)
        prev_first = first;
    if (first != prev_first) {
        if (camZ > SEA_FLOOR) {
            /* caves: first acompanha o andar do player a cada descida/
             * subida — fade do andar anterior cobriria o novo (cliente
             * 7.x troca a seco). */
            fade_from = fade_to = fade_in_from = fade_in_to = -1;
            fade_step = 16;
        } else if (first > prev_first) {
            fade_from = prev_first;
            fade_to = first - 1;
            fade_in_from = fade_in_to = -1;
            fade_step = 0;
        } else {
            fade_in_from = first;
            fade_in_to = prev_first - 1;
            fade_from = fade_to = -1;
            fade_step = 0;
        }
        prev_first = first;
    }
}

/* ---------------- desenho ---------------- */

static void draw_pieces(GSGLOBAL *gs, const uint16_t *spr, int w, int h,
                         int sx, int sy, uint8_t alpha)
{
    /* No sheet do .dat a peça (0,0) é a do tile âncora (bottom-right):
     * o otclient blita a peça (ix,iy) em (w-1-ix, h-1-iy). Logo a peça
     * (ix,iy) fica (ix,iy) tiles a NOROESTE do âncora (sx,sy). */
    for (int iy = 0; iy < h; iy++) {
        for (int ix = 0; ix < w; ix++) {
            int i = iy * w + ix;
            if (spr[i] == 0)
                continue;
            int px = sx - ix * TILE_SIZE;
            int py = sy - iy * TILE_SIZE;
            if (alpha == 255)
                spr_draw(gs, spr[i], (float)px, (float)py, TILE_SIZE);
            else
                spr_draw_alpha(gs, spr[i], (float)px, (float)py, TILE_SIZE,
                               alpha);
        }
    }
}

static int g_walk_phase = 0;
static int g_player_dir = -1;
static uint32_t g_skip_player_id = 0;
static int g_anim_tick = 0;

/* Classificação p/ ordem de desenho dentro do tile (Tile::draw do otclient):
 * 0 = ground, 1 = onBottom (walls), 2 = comum, 3 = criatura, 4 = onTop */
static int thing_draw_order(const Thing *th)
{
    if (th->isCreature)
        return 3;
    const DatThingInfo *d = dat_get_item(th->itemId);
    if (!d)
        return 2;
    if (d->flags & DATF_ONTOP)
        return 4;
    if (d->flags & DATF_ONBOTTOM)
        return 1;
    return 2;
}

/* Criatura andando deve ser desenhada na passada do tile de ORIGEM?
 * (Creature::updateWalkingTile: o bottom-right VISUAL de walkers
 * west/north/NE/NW fica na origem durante O PASSO TODO — só transi-
 * ciona ao destino quando walkOffset chega a 0. East/south/SE/SW
 * entram no destino quase imediatamente.) */
static int walk_draw_at_source(const Thing *th)
{
    if (!th->walking)
        return 0;
    return (th->walkDx + th->walkDy < 0) ||
           (th->walkDx + th->walkDy == 0 && th->walkDy < 0);
}

static void draw_thing(GSGLOBAL *gs, GameMap *map, Thing *th, int lx, int ly,
                        int z, int sx, int sy, uint8_t alpha)
{
    uint16_t spr[TILE_SPRITE_MAX];
    int w, h, n;

    int wx = map->playerTileX + (map->playerFloor - z) - CAM_LOCAL_X + lx;
    int wy = map->playerTileY + (map->playerFloor - z) - CAM_LOCAL_Y + ly;

    if (th->isCreature) {
        if (th->creatureId == g_skip_player_id && g_skip_player_id != 0)
            return;
        /* Walk (Creature::updateWalk/updateWalkAnimation do otclient):
         * criatura já está no tile de destino; offset desliza da
         * origem: dir * (pixels - 32) → 0. Fase cicla 1..footPhases
         * (nunca 0 = idle), footDelay = duration/foot clamped 20..205. */
        int phase = 0;
        int wox = 0, woy = 0;
        if (th->walking) {
            uint32_t elapsed = gm_now_ms() - th->walkStartMs;
            if (elapsed < th->walkDurationMs) {
                int pixels = (int)((uint64_t)TILE_SIZE * elapsed /
                                   th->walkDurationMs);
                wox = th->walkDx * (pixels - TILE_SIZE);
                woy = th->walkDy * (pixels - TILE_SIZE);
                const DatThingInfo *o = dat_get_outfit_info(th->outfitId);
                int phases = o ? o->phases : 1;
                /* otclient p/ clientes antigos: footAnimPhases =
                 * phases-1 (fase 0 = idle), máx 3. Ciclo 1..foot. */
                int foot = phases - 1;
                if (foot > 3) foot = 3;
                if (foot < 1) foot = 1;
                int footDelay = (int)th->walkDurationMs / foot;
                if (footDelay < 20) footDelay = 20;
                if (footDelay > 205) footDelay = 205;
                phase = 1 + (int)((elapsed / (uint32_t)footDelay) %
                                  (uint32_t)foot);
                if (phase >= phases)
                    phase = phases - 1;
            }
        }
        int dir = th->dir & 3;
        n = dat_creature_sprites(th->outfitId, dir, phase, spr, &w, &h);
        if (n > 0)
            draw_pieces(gs, spr, w, h, sx + wox, sy + woy, alpha);
    } else {
        const DatThingInfo *d = dat_get_item(th->itemId);
        if (!d)
            return;
        /* fase de animação por item, ~200ms por fase (otclient
         * ITEM_TICKS_PER_FRAME), ciclando TODAS as fases do item */
        int phase = (d->phases > 1)
                      ? ((g_anim_tick / ITEM_ANIM_FRAMES) % d->phases) : 0;
        for (int l = 0; l < d->layers; l++) {
            n = dat_resolve_sprites_anim(d, th->count, wx, wy, z, phase, l,
                                         spr, &w, &h);
            if (n > 0)
                draw_pieces(gs, spr, w, h, sx, sy, alpha);
        }
    }
}

static void draw_ground(GSGLOBAL *gs, GameMap *map, Tile *t, int lx, int ly,
                         int z, int sx, int sy, uint8_t alpha)
{
    if (t->groundItemId == 0)
        return;
    const DatThingInfo *d = dat_get_item(t->groundItemId);
    if (!d)
        return;
    int wx = map->playerTileX + (map->playerFloor - z) - CAM_LOCAL_X + lx;
    int wy = map->playerTileY + (map->playerFloor - z) - CAM_LOCAL_Y + ly;
    int phase = (d->phases > 1)
                  ? ((g_anim_tick / ITEM_ANIM_FRAMES) % d->phases) : 0;
    uint16_t spr[TILE_SPRITE_MAX];
    int w, h;
    for (int l = 0; l < d->layers; l++) {
        int n = dat_resolve_sprites_anim(d, 0, wx, wy, z, phase, l, spr, &w, &h);
        if (n > 0)
            draw_pieces(gs, spr, w, h, sx, sy, alpha);
    }
}

/* Desenha o player na MESMA posição de tela do tile dele — inclui
 * cam_off (interpolação da câmera durante o walk), VIEW_X/Y no modo
 * direto, e elevation do tile. É isso que mantém o Z-order correto
 * com o painter's algorithm: paredes do tile vizinho a sudeste são
 * desenhadas depois (diag posterior) e cobrem o player. */
static void draw_player_at_tile(GSGLOBAL *gs, GameMap *map,
                                 int sx, int sy, uint8_t alpha)
{
    uint16_t spr[TILE_SPRITE_MAX];
    int w, h;

    uint16_t outfit = 129;
    int dir = g_player_dir;
    for (int y = 0; y < NET_MAP_H; y++)
        for (int x = 0; x < NET_MAP_W; x++) {
            Tile *t = &map->tiles[map->playerFloor][y][x];
            Thing *th = gm_tile_find_creature(t, map->playerId, NULL);
            if (th) {
                outfit = th->outfitId;
                if (dir < 0)
                    dir = th->dir & 3;
                y = NET_MAP_H;
                break;
            }
        }
    if (dir < 0)
        dir = 2;
    int n = dat_creature_sprites(outfit, dir & 3, g_walk_phase, spr, &w, &h);
    if (n > 0)
        draw_pieces(gs, spr, w, h, sx, sy, alpha);
}

void render_game_map(GSGLOBAL *gs, GameMap *map, int cam_off_x, int cam_off_y,
                      int anim_tick, int player_walk_phase, int player_dir,
                      int viewport_local)
{
    FloorRange fr;
    compute_floor_range(map, &fr);

    g_walk_phase = player_walk_phase;
    g_player_dir = player_dir;
    g_skip_player_id = map->playerId;
    g_anim_tick = anim_tick;

    int viewport_x = viewport_local ? 0 : VIEW_X;
    int viewport_y = viewport_local ? 0 : VIEW_Y;
    int base_x = viewport_x + VIEW_W / 2 - TILE_SIZE / 2
               - CAM_LOCAL_X * TILE_SIZE + cam_off_x;
    int base_y = viewport_y + VIEW_H / 2 - TILE_SIZE / 2
               - CAM_LOCAL_Y * TILE_SIZE + cam_off_y;

    /* Posição de tela do player (sem cam_off: a câmera segue o player).
     * Calculada UMA vez, antes do loop — não depende de o tile (8,6)
     * ser visitado (tile vazio por edge de scroll não perde o player). */
    const int pf = map->playerFloor;
    Tile *ptile = &map->tiles[pf][CAM_LOCAL_Y][CAM_LOCAL_X];
    int player_sx = base_x - cam_off_x + CAM_LOCAL_X * TILE_SIZE;
    int player_sy = base_y - cam_off_y + CAM_LOCAL_Y * TILE_SIZE
                  - (int)ptile->elevation;

    /* Deferral do player derivado DO PRÓPRIO cam_off (a variável que
     * realmente posiciona os tiles na tela — não pode dessincronizar,
     * ao contrário de flags separadas). Se os tiles estão deslocados
     * p/ a esquerda (cam_off_x < 0) o player anda p/ oeste e a origem
     * fica a leste — tile que o scan visita DEPOIS do destino; o
     * desenho é adiado para a passada da origem para o chão de lá não
     * cobrir o player (Creature::updateWalkingTile do otclient). */
    int sdx = (cam_off_x < 0) ? 1 : (cam_off_x > 0 ? -1 : 0);
    int sdy = (cam_off_y < 0) ? 1 : (cam_off_y > 0 ? -1 : 0);
    int player_src_lx = CAM_LOCAL_X + sdx;
    int player_src_ly = CAM_LOCAL_Y + sdy;
    int player_defer = 0;
    if (sdx || sdy) {
        int ssum = player_src_lx + player_src_ly;
        int csum = CAM_LOCAL_X + CAM_LOCAL_Y;
        player_defer = (ssum > csum) ||
                       (ssum == csum && player_src_ly > CAM_LOCAL_Y);
    }

    /* modo de mistura por floor (fading) */
    u64 saved_alpha = gs->PrimAlpha;
    int blending_on = 0;

    for (int z = fr.last; z >= fr.first; z--) {
        uint8_t alpha = (uint8_t)floor_alpha(z);
        if (alpha == 0)
            continue;

        if (alpha != 255 && !blending_on) {
            gsKit_set_primalpha(gs, GS_SETREG_ALPHA(0, 1, 0, 1, 0), 0);
            blending_on = 1;
        } else if (alpha == 255 && blending_on) {
            gsKit_set_primalpha(gs, saved_alpha, 0);
            blending_on = 0;
        }

        int x0 = CAM_LOCAL_X - VIEW_TILES_X / 2 - MARGIN_TILES;
        int x1 = CAM_LOCAL_X + VIEW_TILES_X / 2 + 1 + MARGIN_TILES;
        int y0 = CAM_LOCAL_Y - VIEW_TILES_Y / 2 - MARGIN_TILES;
        int y1 = CAM_LOCAL_Y + VIEW_TILES_Y / 2 + 1 + MARGIN_TILES;
        if (x0 < 0) x0 = 0;
        if (y0 < 0) y0 = 0;
        if (x1 > NET_MAP_W) x1 = NET_MAP_W;
        if (y1 > NET_MAP_H) y1 = NET_MAP_H;

        int diag_count = (x1 - x0) + (y1 - y0) - 1;
        for (int diag = 0; diag < diag_count; diag++) {
            for (int ly = y0; ly < y1; ly++) {
                int lx = x0 + (diag - (ly - y0));
                if (lx < x0 || lx >= x1)
                    continue;

                Tile *t = &map->tiles[z][ly][lx];
                int sx = base_x + lx * TILE_SIZE;
                int sy = base_y + ly * TILE_SIZE;

                if (t->groundItemId == 0 && t->thingCount == 0) {
                    /* Tile vazio (edge de scroll / gap de dados): o
                     * player NÃO pode sumir — desenha na passada dele. */
                    if (z == pf) {
                        if (!player_defer &&
                            lx == CAM_LOCAL_X && ly == CAM_LOCAL_Y)
                            draw_player_at_tile(gs, map, player_sx,
                                                player_sy, alpha);
                        if (player_defer &&
                            lx == player_src_lx && ly == player_src_ly)
                            draw_player_at_tile(gs, map, player_sx,
                                                player_sy, alpha);
                    }
                    continue;
                }

                int sy_elev = sy - (int)t->elevation;

                /* 1: chão */
                draw_ground(gs, map, t, lx, ly, z, sx, sy, alpha);

                /* 2: onBottom (paredes, molduras de porta) */
                for (int i = 0; i < t->thingCount; i++) {
                    if (!t->things[i].isCreature &&
                        thing_draw_order(&t->things[i]) == 1)
                        draw_thing(gs, map, &t->things[i], lx, ly, z,
                                   sx, sy_elev, alpha);
                }

                /* 3: itens comuns — REVERSO da pilha (Tile::draw do
                 * otclient: reverse_view sobre m_things) */
                for (int i = t->thingCount - 1; i >= 0; i--) {
                    if (!t->things[i].isCreature &&
                        thing_draw_order(&t->things[i]) == 2)
                        draw_thing(gs, map, &t->things[i], lx, ly, z,
                                   sx, sy_elev, alpha);
                }

                /* 4: criaturas — pulando as que pertencem ao tile de
                 * ORIGEM no scan (serão desenhadas na passada da
                 * origem, abaixo; Creature::updateWalkingTile). */
                for (int i = 0; i < t->thingCount; i++) {
                    if (t->things[i].isCreature &&
                        !walk_draw_at_source(&t->things[i]))
                        draw_thing(gs, map, &t->things[i], lx, ly, z,
                                   sx, sy_elev, alpha);
                }
                /* Criaturas chegando DESTE tile: pertencem à origem,
                 * desenhadas aqui (profundidade da origem) na posição
                 * visual correta (destino + walk offset). */
                for (int ndy = -1; ndy <= 1; ndy++) {
                    for (int ndx = -1; ndx <= 1; ndx++) {
                        if (ndx == 0 && ndy == 0)
                            continue;
                        int nlx = lx + ndx, nly = ly + ndy;
                        if (nlx < 0 || nlx >= NET_MAP_W ||
                            nly < 0 || nly >= NET_MAP_H)
                            continue;
                        Tile *nt = &map->tiles[z][nly][nlx];
                        for (int i = 0; i < nt->thingCount; i++) {
                            Thing *th = &nt->things[i];
                            if (!th->isCreature || !th->walking)
                                continue;
                            if (th->walkDx != ndx || th->walkDy != ndy)
                                continue; /* origem ≠ este tile */
                            if (!walk_draw_at_source(th))
                                continue;
                            int dsx = base_x + nlx * TILE_SIZE;
                            int dsy = base_y + nly * TILE_SIZE -
                                      (int)nt->elevation;
                            draw_thing(gs, map, th, nlx, nly, z,
                                       dsx, dsy, alpha);
                        }
                    }
                }
                /* Player: desenhado SEM cam_off (câmera segue o player).
                 * player_sx/sy pré-computados; o defer (decidido pelo
                 * cam_off) manda o desenho para a passada da origem. */
                if (z == pf) {
                    if (!player_defer &&
                        lx == CAM_LOCAL_X && ly == CAM_LOCAL_Y)
                        draw_player_at_tile(gs, map, player_sx,
                                            player_sy, alpha);
                    if (player_defer &&
                        lx == player_src_lx && ly == player_src_ly)
                        draw_player_at_tile(gs, map, player_sx,
                                            player_sy, alpha);
                }

                /* 5: onTop (efeitos, luzes) */
                for (int i = 0; i < t->thingCount; i++) {
                    if (!t->things[i].isCreature &&
                        thing_draw_order(&t->things[i]) == 4)
                        draw_thing(gs, map, &t->things[i], lx, ly, z,
                                   sx, sy_elev, alpha);
                }
            }
        }
    }

    if (blending_on)
        gsKit_set_primalpha(gs, saved_alpha, 0);

    fade_advance();
}

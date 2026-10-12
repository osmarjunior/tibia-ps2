/**
 * hud.c — UI mínima (HP/MP, chat, battle list, container).
 *
 * Desenha no backbuffer FORA do viewport do mapa (viewport 480x352 em
 * (48,48); a coluna direita, x 528..640, guarda a battle list e a
 * janela de equipamentos sem encobrir o mapa).
 */
#include <kernel.h>
#include <gsKit.h>
#include <tamtypes.h>
#include <string.h>
#include <stdio.h>

#include "hud.h"
#include "font.h"
#include "hud_pic_assets.h"
#include "../net/game_protocol.h"
#include "../poc/map_rt.h" /* VIEW_X/Y/W/H */
#include "../poc/dat_loader.h"
#include "../poc/spr_loader.h"

#define HUD_LX (640 - 92) /* coluna direita, encostada na borda */
#define HUD_BATTLE_H 232

static void bar(GSGLOBAL *gs, int x, int y, int w, int h,
                 uint32_t value, uint32_t max, uint32_t color)
{
    ui_rect(gs, x, y, w, h, UI_COLOR(0x18, 0x18, 0x18));
    if (max > 0) {
        int fw = (int)(((uint64_t)value * (uint64_t)(w - 2)) / (uint64_t)max);
        if (fw > w - 2) fw = w - 2;
        ui_rect(gs, x + 1, y + 1, fw, h - 2, color);
    }
    char txt[24];
    snprintf(txt, sizeof(txt), "%u/%u", (unsigned)value, (unsigned)max);
    font_draw(gs, txt, x, y + 10, UI_COLOR(0xFF, 0xFF, 0xFF));
}

void hud_render(GSGLOBAL *gs, GameMap *map, int show_debug_stats)
{
    /* ---- barra de vida/mana (topo, acima do viewport) ---- */
    bar(gs, VIEW_X, 8, 100, 8, map->playerHealth, map->playerMaxHealth,
        UI_COLOR(0xC0, 0x20, 0x20));
    bar(gs, VIEW_X + 160, 8, 100, 8, map->playerMana, map->playerMaxMana,
        UI_COLOR(0x20, 0x40, 0xC0));

    {
        char pos[32];
        snprintf(pos, sizeof(pos), "lv%d %d,%d,%d", map->playerLevel,
                 map->playerTileX, map->playerTileY, map->playerFloor);
        font_draw(gs, pos, VIEW_X + 250, 8, UI_COLOR(0xC0, 0xC0, 0xC0));
    }

    /* ---- chat (embaixo do viewport; 3 linhas para performance) ---- */
    int cy = VIEW_Y + VIEW_H - 2;
    ui_rect(gs, VIEW_X, cy, VIEW_W, 50, UI_COLOR(0x00, 0x00, 0x08));
    for (int i = 0; i < 3; i++) {
        const char *line = gp_chat_line(gp_chat.count - 3 + i);
        if (!line[0])
            continue;
        font_draw(gs, line, VIEW_X + 2, cy + 1 + i * 15,
                  UI_COLOR(0xE0, 0xE0, 0xE0));
    }

    /* ---- battle list (só no floor do player — 18x14=252 tiles,
     *      não 16x14x18=4032 como antes). Moldura estilo janela 7.6,
     *      mesma família da janela de equipamentos. ---- */
    {
        int bx = HUD_LX;
        int bw = 92, bh = HUD_BATTLE_H;
        ui_rect(gs, bx, VIEW_Y, bw, bh, UI_COLOR(0x4A, 0x38, 0x26));
        ui_rect(gs, bx + 2, VIEW_Y + 16, bw - 4, bh - 18,
                UI_COLOR(0x28, 0x20, 0x14));
        font_draw_scaled(gs, "Battle List", bx + 4, VIEW_Y + 4,
                         UI_COLOR(0xD0, 0xC0, 0x90), 1);
        ui_rect(gs, bx + bw - 13, VIEW_Y + 3, 10, 10,
                UI_COLOR(0xA0, 0x20, 0x20));
        font_draw_scaled(gs, "X", bx + bw - 11, VIEW_Y + 5,
                         UI_COLOR(0xFF, 0xE0, 0xE0), 1);
        int row = 0;
        int pz = map->playerFloor;
        for (int y = 0; y < NET_MAP_H && row < 14; y++)
            for (int x = 0; x < NET_MAP_W && row < 14; x++) {
                Tile *t = &map->tiles[pz][y][x];
                for (int i = 0; i < t->thingCount && row < 14; i++) {
                    Thing *th = &t->things[i];
                    if (!th->isCreature || !th->name[0])
                        continue;
                    if (th->creatureId == map->playerId)
                        continue;
                    int hp = th->healthPct > 100 ? 100 : th->healthPct;
                    uint32_t col = hp > 60 ? UI_COLOR(0x40, 0xE0, 0x40)
                                    : hp > 25 ? UI_COLOR(0xE0, 0xE0, 0x40)
                                               : UI_COLOR(0xE0, 0x40, 0x40);
                    char line[20];
                    snprintf(line, sizeof(line), "%.4s %2d", th->name, hp);
                    font_draw(gs, line, bx + 4, VIEW_Y + 18 + row * 15, col);
                    row++;
                }
            }
    }

    /* ---- container aberto (cid 0) ---- */
    if (gp_containers[0].active) {
        GpContainer *c = &gp_containers[0];
        int wx = VIEW_X + VIEW_W / 2 - 100;
        int wy = VIEW_Y + 20;
        int rows = (c->count + 7) / 8;
        ui_rect(gs, wx, wy, 200, 24 + rows * 24, UI_COLOR(0x30, 0x28, 0x18));
        font_draw(gs, c->name[0] ? c->name : "container", wx + 2, wy + 4,
                  UI_COLOR(0xFF, 0xD0, 0x60));
        for (int i = 0; i < c->count && i < 16; i++) {
            int ix = wx + 4 + (i % 8) * 24;
            int iy = wy + 22 + (i / 8) * 24;
            const DatThingInfo *d = dat_get_item(c->items[i]);
            if (d) {
                uint16_t spr[TILE_SPRITE_MAX];
                int w, h;
                int n = dat_resolve_sprites(d, c->itemCount[i], 1000, 1000,
                                            map->playerFloor, spr, &w, &h);
                if (n > 0)
                    spr_draw(gs, spr[n - 1], (float)ix, (float)iy, 20);
            }
            if (c->itemCount[i] > 1) {
                char cnt[8];
                snprintf(cnt, sizeof(cnt), "%d", c->itemCount[i]);
                font_draw(gs, cnt, ix + 12, iy + 12, UI_COLOR(0xFF, 0xFF, 0xFF));
            }
        }
    }

    /* ---- estatísticas do cache de sprites (Fase 6) ---- */
    if (show_debug_stats) {
        const SprStats *st = spr_get_stats();
        char s[96];
        snprintf(s, sizeof(s),
                 "spr %d/%d miss/f %d evict %d", st->resident, st->capacity,
                 st->misses_frame, st->evictions);
        font_draw(gs, s, VIEW_X, VIEW_Y - 16,
                  UI_COLOR(0x80, 0xFF, 0x80));
    }
}

/* ---- janela de equipamentos (abre/fecha com L2) ----
 * Papel-de-boneco do Tibia 7.6: 10 slots 32px em 3 colunas, feet
 * centralizado. Índice = slot do protocolo (gp_inventory):
 *   0=head 1=amulet 2=backpack 3=armor 4=right 5=left
 *   6=legs 7=feet 8=ring 9=ammo
 * Coluna direita, abaixo da battle list — fora do viewport (o mapa
 * termina em x=528), encostada na borda como os painéis do original.
 * O X vermelho é visual (assinatura 7.6); sem cursor de UI, quem fecha
 * é o próprio L2.
 */
#define EQ_SLOT 32
#define EQ_TITLE_H 16

/* Layout clássico do Tibia (índice = slot do protocolo):
 *   amulet(1) | head(0)  | backpack(2)
 *   right(4)  | armor(3) | left(5)
 *   ring(8)   | legs(6)  | ammo(9)
 *             | feet(7)  |
 * "right hand" (arma) fica na coluna esquerda da tela e "left hand"
 * (escudo) na direita, porque o boneco é visto de frente. */
static const int EQ_ROW[GP_INV_SLOTS] = {0, 0, 0, 1, 1, 1, 2, 3, 2, 2};
static const int EQ_COL[GP_INV_SLOTS] = {1, 0, 2, 1, 0, 2, 1, 1, 0, 2};

/* ---- placeholders de slot (imagens do OTClient) ----
 * PIC_SLOT_TEX[slot] (hud_pic_assets.h): o fundo estatico de cada
 * slot vazio, com alpha. Sobe p/ VRAM no boot (hud_init, chamado do
 * client_net ANTES do cache de sprites reservar a VRAM restante):
 * copia p/ buffer writavel 64-alinhado e segue o mesmo caminho de
 * upload do spr_loader. O quadrado escuro (eq_slot_hole) desenha
 * ATRAS do icone: os placeholders sao silhuetas cinza-escuro feitas
 * p/ fundo quase preto (como o painel do OTClient) — sem ele, o
 * corpo marrom da janela engole o icone. O item equipado cobre
 * tudo, como no OTClient. */
static GSTEXTURE eq_slot_tex[PIC_SLOT_COUNT];
static int eq_slot_ready = 0;
static u32 eq_pixel_buf[PIC_SLOT_COUNT][PIC_SLOT_W * PIC_SLOT_H]
    __attribute__((aligned(64)));

/* gp_inventory é indexado pelo slot do protocolo (1=head ... 10=ammo) */
static uint16_t eq_item(int s)
{
    int idx = s + 1;
    return (idx <= GP_INV_SLOTS) ? gp_inventory[idx] : 0;
}

static void eq_slot_tex_init(GSGLOBAL *gs)
{
    if (eq_slot_ready)
        return;

    u32 tex_size = gsKit_texture_size(PIC_SLOT_W, PIC_SLOT_H, GS_PSM_CT32);
    for (int s = 0; s < PIC_SLOT_COUNT; s++) {
        for (int i = 0; i < PIC_SLOT_W * PIC_SLOT_H; i++) {
            u32 p = PIC_SLOT_TEX[s][i];
            u32 a = ((p >> 24) + 1) >> 1;                 /* 0..255 -> 0..128 */
            eq_pixel_buf[s][i] = (p & 0x00FFFFFFu) | (a << 24);
        }

        GSTEXTURE *t = &eq_slot_tex[s];
        t->Width = PIC_SLOT_W;
        t->Height = PIC_SLOT_H;
        t->PSM = GS_PSM_CT32;
        t->Filter = GS_FILTER_NEAREST;
        t->Delayed = 0;
        t->Mem = eq_pixel_buf[s];
        t->Vram = gsKit_vram_alloc(gs, tex_size, GSKIT_ALLOC_USERBUFFER);
        if (t->Vram == GSKIT_ALLOC_ERROR) {
            printf("hud: vram_alloc falhou no placeholder %d (ptr=0x%X)\n",
                   s, (unsigned)gs->CurrentPointer);
            return; /* slots ficam so com o recesso vetorial */
        }

        /* mesmo padrao do spr_loader nos cache-miss: sync + upload */
        SyncDCache(eq_pixel_buf[s],
                   (u8 *)eq_pixel_buf[s] + sizeof(eq_pixel_buf[s]));
        gsKit_texture_upload(gs, t);
    }
    eq_slot_ready = 1;
    printf("hud: %d placeholders na VRAM (ptr=0x%X)\n",
           PIC_SLOT_COUNT, (unsigned)gs->CurrentPointer);
}

void hud_init(GSGLOBAL *gs)
{
    eq_slot_tex_init(gs);
}

/* Recesso vetorial: o quadrado quase preto de fundo de slot (o
 * "buraco" do painel do OTClient) — desenhado SEMPRE, atras do
 * placeholder e do item. */
static void eq_slot_hole(GSGLOBAL *gs, int x, int y, int size)
{
    ui_rect(gs, x, y, size, size, UI_COLOR(0x0A, 0x0A, 0x08));
    ui_rect(gs, x, y, size, 1, UI_COLOR(0x00, 0x00, 0x00));
    ui_rect(gs, x, y, 1, size, UI_COLOR(0x00, 0x00, 0x00));
    ui_rect(gs, x, y + size - 1, size, 1, UI_COLOR(0x48, 0x38, 0x24));
    ui_rect(gs, x + size - 1, y, 1, size, UI_COLOR(0x48, 0x38, 0x24));
}

static void eq_slot_draw(GSGLOBAL *gs, int x, int y, int slot, int filled)
{
    eq_slot_hole(gs, x, y, EQ_SLOT);
    if (filled || !eq_slot_ready)
        return; /* item equipado: sem placeholder */
    gsKit_prim_sprite_texture(gs, &eq_slot_tex[slot],
        (float)x, (float)y, 0.0f, 0.0f,
        (float)(x + EQ_SLOT), (float)(y + EQ_SLOT),
        (float)PIC_SLOT_W, (float)PIC_SLOT_H,
        0, GS_SETREG_RGBAQ(0x80, 0x80, 0x80, 0x80, 0x00));
}

void hud_equipment_window(GSGLOBAL *gs, GameMap *map, int open)
{
    if (!open)
        return;

    int wx = 640 - (4 + EQ_SLOT * 3 + 4);            /* encostada à direita */
    int wy = VIEW_Y + HUD_BATTLE_H + 8;              /* abaixo da battle list */
    int w = 4 + EQ_SLOT * 3 + 4;
    int h = EQ_TITLE_H + 4 + EQ_SLOT * 4 + 4;

    /* moldura + corpo, cores de janela 7.6 (marrom-pedra) */
    ui_rect(gs, wx, wy, w, h, UI_COLOR(0x4A, 0x38, 0x26));
    ui_rect(gs, wx + 2, wy + EQ_TITLE_H, w - 4, h - EQ_TITLE_H - 2,
            UI_COLOR(0x28, 0x20, 0x14));
    /* título e X em escala 1: "Inventory" a escala 2 (108px) não cabe em w */
    font_draw_scaled(gs, "Inventory", wx + 4, wy + 4,
                     UI_COLOR(0xD0, 0xC0, 0x90), 1);
    ui_rect(gs, wx + w - 13, wy + 3, 10, 10, UI_COLOR(0xA0, 0x20, 0x20));
    font_draw_scaled(gs, "X", wx + w - 11, wy + 5,
                     UI_COLOR(0xFF, 0xE0, 0xE0), 1);

    for (int s = 0; s < GP_INV_SLOTS; s++) {
        int sx = wx + 4 + EQ_COL[s] * EQ_SLOT;
        int sy = wy + EQ_TITLE_H + 4 + EQ_ROW[s] * EQ_SLOT;

        uint16_t spr[TILE_SPRITE_MAX];
        int n = 0;
        uint16_t item = eq_item(s);
        if (item) {
            const DatThingInfo *d = dat_get_item(item);
            if (d) {
                int tw, th;
                n = dat_resolve_sprites(d, 1, 1000, 1000, map->playerFloor,
                                        spr, &tw, &th);
            }
        }
        eq_slot_draw(gs, sx, sy, s, n > 0);
        if (n > 0)
            spr_draw(gs, spr[n - 1], (float)sx, (float)sy, EQ_SLOT);
    }
}

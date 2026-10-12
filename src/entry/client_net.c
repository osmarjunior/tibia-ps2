/**
 * client_net.c — cliente Tibia 7.4 conectável para PS2 (TASKS2.md).
 *
 * Substitui o fluxo do main da POC (build_mock_map) por:
 *   login (RSA+XTEA) → game server → pacotes → GameMap → render.
 *
 * Fases implementadas aqui:
 *   3: mapa real vindo do servidor no renderer (render_game)
 *   4: walk preditivo com reconciliação (gp_walk_optimistic + 0xB5)
 *   5: múltiplos floors com culling/fading (compute_floor_range)
 *   6: estatísticas do cache de sprites em tela/log
 *   7: look/use/attack via cursor + teclado virtual p/ login/chat
 *   8: HUD (HP/MP, chat, battle list, container)
 *
 * Controles:
 *   D-pad       anda          L1+D-pad  move o cursor de alvo
 *   X           usar item     □         look no cursor
 *   △           atacar        ○         digitar chat (say)
 *   START       logout        R1        liga/desliga stats (Fase 6)
 *   R2          dump do grid no log (diagnóstico de tiles pretos)
 *   L2          abre/fecha a janela de equipamentos (Inventory)
 *   SELECT      trocar conta (volta ao login)
 */
#include <kernel.h>
#include <loadfile.h>
#include <sifrpc.h>
#include <stdio.h>
#include <string.h>
#include <tamtypes.h>
#include <timer.h>

#include <gsKit.h>
#include <dmaKit.h>

#include "../net/ps2_net.h"
#include "../net/net_stream.h"
#include "../net/login.h"
#include "../net/game_protocol.h"
#include "../poc/spr_loader.h"
#include "../poc/dat_loader.h"
#include "../poc/map_rt.h"
#include "../poc/map_rt.h"
#include "../poc/render_game.h"
#include "../ui/font.h"
#include "../ui/pad.h"
#include "../ui/vkeyboard.h"
#include "../ui/hud.h"

#define USE_MAP_RT 0  /* mesmo approach do poc.elf: RT offscreen */

/* ---- configuração (defaults compiláveis via -D) ---- */
#ifndef LOGIN_HOST
#define LOGIN_HOST "192.168.1.2"
#endif
#ifndef LOGIN_PORT
#define LOGIN_PORT 7171
#endif
#ifndef ACCOUNT
#define ACCOUNT "111111"
#endif
#ifndef PASSWORD
#define PASSWORD "tibia"
#endif
#ifndef CHARNAME
#define CHARNAME "Ps2 Tester"
#endif

#define SCREEN_W 640
#define SCREEN_H 448
#define TILE_SIZE 32
#define STEP_LEAD_MS 0

GSGLOBAL *gsGlobal;

static uint32_t now_ms(void)
{
    u64 clocks = GetTimerSystemTime();
    u32 sec, usec;
    TimerBusClock2USec(clocks, &sec, &usec);
    return (uint32_t)(sec * 1000u + usec / 1000u);
}

/* ---------------- estado de sessão ---------------- */

static char cfg_account[32] = ACCOUNT;
static char cfg_password[32] = PASSWORD;
static char cfg_char[32] = CHARNAME;

static int connected = 0;
static int show_stats = 0;
static int show_equipment = 0; /* janela Inventory (toggle L2) */
static uint32_t last_pad_held = 0;
static uint32_t last_pad_edge = 0;

/* ---------------- animação de passo (câmera) ----------------
 * gp_walk_optimistic desloca a janela imediatamente e mantém o player
 * no tile central. O render aplica o deslocamento inverso da câmera
 * em pixels, como a POC, para que o cenário deslize sob o player fixo. */
static int walk_anim = 0;
static int walk_dx, walk_dy;
static int walk_duration = 680;
static uint32_t walk_start_ms = 0;
static int anim_tick = 0;
static void clear_startup_framebuffers(GSGLOBAL *gs)
{
    /* OPL can leave its previous image in either VRAM framebuffer. Clear
     * and present both buffers before the first UI frame is queued. */
    for (int i = 0; i < 2; i++) {
        gsKit_clear(gs, GS_SETREG_RGBAQ(0, 0, 0, 0x80, 0));
        gsKit_queue_exec(gs);
        gsKit_sync_flip(gs);
    }
}

static int centered_text_x(const char *text)
{
    int width = (int)strlen(text) * FONT_GLYPH_W * 2;
    return width < SCREEN_W ? (SCREEN_W - width) / 2 : 8;
}

/* ---------------- cursor de alvo ---------------- */
static int cur_dx = 0, cur_dy = 0; /* relativo ao player, -3..3 */

/* ---------------- tela de login ---------------- */

static void wait_screen(GSGLOBAL *gs, const char *line1, const char *line2)
{
    gsKit_clear(gs, GS_SETREG_RGBAQ(0, 0, 0, 0, 0));
    font_draw(gs, line1, centered_text_x(line1), 200,
              UI_COLOR(0xFF, 0xFF, 0xFF));
    if (line2)
        font_draw(gs, line2, centered_text_x(line2), 220,
                  UI_COLOR(0xFF, 0x80, 0x80));
    gsKit_queue_exec(gs);
    gsKit_sync_flip(gs);
}

static void login_screen(GSGLOBAL *gs)
{

    for (;;) {
        /* Mantém a mesma cadência da POC e do loop de jogo: espera o
         * VSync/troca de buffer antes de construir o próximo quadro. */
        gsKit_sync_flip(gs);
        gsKit_clear(gs, GS_SETREG_RGBAQ(0, 0, 0, 0, 0));
        font_draw(gs, "tibia-ps2", 16, 16, UI_COLOR(0xFF, 0xD0, 0x40));
        font_draw(gs, "Aperte O para conectar  SELECT edita conta/senha/char", 16, 40,
                  UI_COLOR(0xC0, 0xC0, 0xC0));
        char line[96];
        snprintf(line, sizeof(line), "acc %s  char %s  @%s", cfg_account,
                 cfg_char, LOGIN_HOST);
        font_draw(gs, line, 16, 56, UI_COLOR(0x80, 0xFF, 0x80));
        gsKit_queue_exec(gs);

        uint32_t edge = pad2_read_edge(NULL);
        if (edge & PAD_BTN_SELECT) {
            if (vkeyboard_run(gs, "conta:", cfg_account, sizeof(cfg_account),
                              cfg_account)) {
                if (vkeyboard_run(gs, "senha:", cfg_password,
                                  sizeof(cfg_password), cfg_password)) {
                    vkeyboard_run(gs, "personagem:", cfg_char,
                                  sizeof(cfg_char), cfg_char);
                }
            }
            continue;
        }
        if (edge & PAD_BTN_CROSS)
            return;
    }
}

static void try_connect(GSGLOBAL *gs)
{
    wait_screen(gs, "conectando ao login server...", NULL);

    if (ps2_net_init() != 0) {
        wait_screen(gs, "rede indisponivel (X volta)", NULL);
        while (!(pad2_read_edge(NULL) & PAD_BTN_CROSS))
            gsKit_sync_flip(gs);
        return;
    }

    /* RS2 port: SIO2MAN/PADMAN devem ser carregados DEPOIS dos modulos
     * de rede (DEV9/NETMAN/SMAP). O carregamento anterior (em main)
     * serve para a tela de login; este re-init garante que o pad
     * continue funcionando apos o bring-up da rede. */
    pad2_init();
    printf("client_net: pad re-init apos rede\n");

    net_set_stream(ps2_net_stream());
    tibia_login_set_server(LOGIN_HOST, LOGIN_PORT);

    wait_screen(gs, "entrando no mundo...", NULL);
    if (gp_enter_game(cfg_account, cfg_password, cfg_char, LOGIN_HOST,
                      LOGIN_PORT, LOGIN_HOST, 7172)) {
        connected = 1;
        return;
    }

    char msg[160];
    snprintf(msg, sizeof(msg), "erro: %s", gp_error());
    wait_screen(gs, "login falhou (X volta)", msg);
    while (!(pad2_read_edge(NULL) & PAD_BTN_CROSS)) {
        gsKit_sync_flip(gs);
        gsKit_clear(gs, GS_SETREG_RGBAQ(0, 0, 0, 0, 0));
        const char *title = "login falhou (X volta)";
        font_draw(gs, title, centered_text_x(title), 200,
                  UI_COLOR(0xFF, 0xFF, 0xFF));
        font_draw(gs, msg, centered_text_x(msg), 220,
              UI_COLOR(0xFF, 0x80, 0x80));
        gsKit_queue_exec(gs);
    }
    connected = 0;
}

/* ---------------- callbacks de protocolo ---------------- */

static void cb_on_disconnect(void)
{
    connected = 0;
}

static void cb_on_own_move_rejected(void)
{
    /* passo rejeitado: reconcilia a animação */
    walk_anim = 0;
    walk_start_ms = 0;
}

static void cb_on_own_position_reset(int x, int y, int z)
{
    (void)x; (void)y; (void)z;
    walk_anim = 0;
    walk_start_ms = 0;
}

static void cb_on_own_floor_change(int up, int nx, int ny, int nz)
{
    (void)up; (void)nx; (void)ny; (void)nz;
    walk_anim = 0;
    walk_start_ms = 0;
}

/* ---------------- helpers de alvo (Fase 7) ---------------- */

typedef struct {
    int found;
    int isCreature;
    uint32_t creatureId;
    uint16_t itemId;
    uint8_t stackpos;
} Target;

static Target target_at_cursor(void)
{
    Target t = {0, 0, 0, 0, 0};
    GameMap *m = &gp_map;
    int wx = m->playerTileX + cur_dx;
    int wy = m->playerTileY + cur_dy;
    int wz = m->playerFloor;
    int lx = wx - (m->playerTileX + (m->playerFloor - wz) - 8);
    int ly = wy - (m->playerTileY + (m->playerFloor - wz) - 6);
    if (lx < 0 || lx >= NET_MAP_W || ly < 0 || ly >= NET_MAP_H)
        return t;
    Tile *tile = &m->tiles[wz][ly][lx];

    if (tile->thingCount > 0) {
        Thing *top = &tile->things[tile->thingCount - 1];
        if (top->isCreature) {
            t.found = 1;
            t.isCreature = 1;
            t.creatureId = top->creatureId;
            t.stackpos = top->stackpos;
            return t;
        }
    }
    for (int i = 0; i < tile->thingCount; i++) {
        if (!tile->things[i].isCreature) {
            t.found = 1;
            t.itemId = tile->things[i].itemId;
            t.stackpos = tile->things[i].stackpos;
            return t;
        }
    }
    if (tile->groundItemId) {
        t.found = 1;
        t.itemId = tile->groundItemId;
        t.stackpos = 0;
    }
    return t;
}

/* ---------------- loop principal ---------------- */

int main(void)
{
    SifInitRpc(0);

    dmaKit_init(D_CTRL_RELE_OFF, D_CTRL_MFD_OFF, D_CTRL_STS_UNSPEC,
                D_CTRL_STD_OFF, D_CTRL_RCYC_8, 1 << DMA_CHANNEL_GIF);
    dmaKit_chan_init(DMA_CHANNEL_GIF);

    gsGlobal = gsKit_init_global();
    //gsGlobal->Field = GS_FRAME;
    gsGlobal->Width = SCREEN_W;
    gsGlobal->Height = SCREEN_H;
    gsGlobal->PSM = GS_PSM_CT24;
    gsGlobal->DoubleBuffering = GS_SETTING_ON;
    gsGlobal->ZBuffering = GS_SETTING_OFF;
    gsGlobal->PrimAlphaEnable = GS_SETTING_ON;
    gsKit_set_primalpha(gsGlobal, 0, 0);
    gsGlobal->Test->ATE = 1;
    gsGlobal->Test->ATST = 6;
    gsGlobal->Test->AREF = 0;
    gsGlobal->Test->ZTE = 0;
    gsGlobal->Test->ZTST = 1;
    gsKit_set_test(gsGlobal, GS_ATEST_ON);
    gsKit_init_screen(gsGlobal);
    clear_startup_framebuffers(gsGlobal);

#if USE_MAP_RT
    /* Aloca o RT ANTES de qualquer sprite (mesma ordem do poc.elf —
     * os sprites não podem sobrescrever o RT na VRAM). */
    if (map_framebuffer_init(gsGlobal) != 0)
        printf("client_net: map_framebuffer_init falhou\n");
#endif

    spr_loader_init();
    dat_loader_init();
    if (font_init(gsGlobal) != 0) {
        printf("fonte falhou — HUD ficará sem texto\n");
    }
    /* placeholders do inventário: precisam de VRAM ANTES do cache
     * de sprites, que reserva toda a restante na 1a carga */
    hud_init(gsGlobal);
    pad2_init();

    GpCallbacks cb;
    memset(&cb, 0, sizeof(cb));
    cb.on_disconnect = cb_on_disconnect;
    cb.on_own_move_rejected = cb_on_own_move_rejected;
    cb.on_own_position_reset = cb_on_own_position_reset;
    cb.on_own_floor_change = cb_on_own_floor_change;
    gp_set_callbacks(&cb);

    int stats_frames = 0;

    for (;;) {
        /* ---- sessão ---- */
        while (!connected) {
            login_screen(gsGlobal);
            try_connect(gsGlobal);
        }

        cur_dx = 0;
        cur_dy = 0;
        show_equipment = 0;
        walk_anim = 0;
        walk_start_ms = 0;
        anim_tick = 0;

        while (connected) {
            //gsKit_vsync_wait();            /* campo extra: segura o frame anterior */
            gsKit_sync_flip(gsGlobal);

            /* 1) rede */
            gp_poll(0);
            gp_move_tick(); /* timeout do pred_pending baseado em frames */

            /* 2) input */
            int held;
            uint32_t edge = pad2_read_edge(&held);
            last_pad_held = (uint32_t)held;
            last_pad_edge = edge;

            if (edge & PAD_BTN_START) {
                gp_send_logout();
                gp_disconnect();
                connected = 0;
                break;
            }
            if (edge & PAD_BTN_SELECT) {
                gp_disconnect();
                connected = 0;
                break;
            }
            if (edge & PAD_BTN_R1)
                show_stats = !show_stats;
            if (edge & PAD_BTN_L2)
                show_equipment = !show_equipment;
            if (edge & PAD_BTN_R2)
                gp_debug_dump_window();

            if (held & PAD_BTN_L1) {
                /* mover cursor */
                if (edge & PAD_BTN_UP && cur_dy > -3) cur_dy--;
                if (edge & PAD_BTN_DOWN && cur_dy < 3) cur_dy++;
                if (edge & PAD_BTN_LEFT && cur_dx > -3) cur_dx--;
                if (edge & PAD_BTN_RIGHT && cur_dx < 3) cur_dx++;
            } else {
                /* andar: usa HELD para manter o passo segurando o D-pad.
                 * O próximo comando pode sair pouco antes do fim da
                 * animação atual para esconder a latência do servidor. */
                uint32_t t = now_ms();
                int elapsed = walk_anim ? (int)(t - walk_start_ms) : 0;
                int can_step = (!walk_anim) ||
                    (elapsed >= walk_duration - STEP_LEAD_MS);
                if (can_step) {
                    int dir = -1, dx = 0, dy = 0;
                    if (held & PAD_BTN_UP) { dir = 0; dy = -1; }
                    else if (held & PAD_BTN_DOWN) { dir = 2; dy = 1; }
                    else if (held & PAD_BTN_LEFT) { dir = 3; dx = -1; }
                    else if (held & PAD_BTN_RIGHT) { dir = 1; dx = 1; }

                    if (dir >= 0) {
                        int delay = gp_step_delay_ms(dir);
                        if (delay <= 0)
                            delay = 680;
                        if (gp_walk_optimistic(dir)) {
                            int carry = walk_anim ? elapsed - walk_duration : 0;
                            if (carry < 0)
                                carry = 0;
                            walk_anim = 1;
                            walk_start_ms = t - (uint32_t)carry;
                            walk_dx = dx;
                            walk_dy = dy;
                            walk_duration = delay;
                        }
                    }
                }

                /* ações no cursor (Fase 7) */
                if (edge & PAD_BTN_SQUARE || edge & PAD_BTN_CROSS ||
                    edge & PAD_BTN_TRI) {
                    Target t = target_at_cursor();
                    int wx = gp_map.playerTileX + cur_dx;
                    int wy = gp_map.playerTileY + cur_dy;
                    if (t.found) {
                        if (edge & PAD_BTN_SQUARE)
                            gp_send_look_at((uint16_t)wx, (uint16_t)wy,
                                             (uint8_t)gp_map.playerFloor,
                                             t.itemId, t.stackpos);
                        if (edge & PAD_BTN_CROSS)
                            gp_send_use((uint16_t)wx, (uint16_t)wy,
                                         (uint8_t)gp_map.playerFloor, t.itemId,
                                         t.stackpos, 0);
                        if (edge & PAD_BTN_TRI) {
                            if (t.isCreature)
                                gp_send_attack(t.creatureId);
                            else {
                                Thing *th = NULL;
                                /* ataca a criatura do tile, se houver */
                                int lx = wx - (gp_map.playerTileX - 8);
                                int ly = wy - (gp_map.playerTileY - 6);
                                if (lx >= 0 && lx < NET_MAP_W && ly >= 0 &&
                                    ly < NET_MAP_H) {
                                    Tile *tile = &gp_map.tiles[gp_map.playerFloor][ly][lx];
                                    th = NULL;
                                    for (int i = 0; i < tile->thingCount; i++)
                                        if (tile->things[i].isCreature) {
                                            th = &tile->things[i];
                                            break;
                                        }
                                    if (th)
                                        gp_send_attack(th->creatureId);
                                }
                            }
                        }
                    }
                }

                /* chat (Fase 7/8) */
                if (edge & PAD_BTN_CIRCLE) {
                    char text[64];
                    if (vkeyboard_run(gsGlobal, "say:", text, sizeof(text), "")) {
                        if (text[0])
                            gp_send_say(1, text);
                    }
                }
            }

            /* 3) câmera em pixels. Como o mapa conectado já foi rolado
             * pelo gp_walk_optimistic(), o alvo está no centro lógico;
             * deslocamos o mapa na direção do passo por (TILE_SIZE - off)
             * para reconstruir a câmera interpolada. */
            int map_shift_x = 0, map_shift_y = 0;
            int player_walk_phase = 0;
            if (walk_anim) {
                int progress = (int)(now_ms() - walk_start_ms);
                if (progress >= walk_duration) {
                    walk_anim = 0;
                    player_walk_phase = 0; /* idle (Creature::terminateWalk) */
                } else {
                    int off = (TILE_SIZE * progress) / walk_duration;
                    /* gm_scroll já colocou o DESTINO no tile central. O
                     * cam_off compensa o scroll: +32*dir → 0 faz o mapa
                     * começar exatamente na posição VELHA e deslizar até
                     * a nova (pans suave da câmera). O player é desenhado
                     * SEM cam_off (render_game.c) — ele fica no centro
                     * visual enquanto o mapa desliza embaixo, exatamente
                     * como a câmera do otclient segue o LocalPlayer.
                     * O render_game deriva o Z-order do player do
                     * PRÓPRIO cam_off — não há flags separadas. */
                    map_shift_x = walk_dx * (TILE_SIZE - off);
                    map_shift_y = walk_dy * (TILE_SIZE - off);
                    /* walk frames 1..3 (nunca 0 = idle; otclient
                     * updateWalkAnimation p/ clientes antigos) */
                    player_walk_phase =
                        1 + (progress * 3 / walk_duration) % 3;
                }
            }

            anim_tick++;

            /* 4) render — mesmo approach do poc.elf: mapa vai para o
             * RT offscreen, depois apresentado como uma única textura.
             * Isto elimina ~200 draws individuais por frame no
             * backbuffer, que causava o efeito de "tremedeira". */
            gsKit_clear(gsGlobal, GS_SETREG_RGBAQ(0x00, 0x00, 0x00, 0xFF, 0x00));

#if USE_MAP_RT
            /* RT: mapa e player são recortados pelo scissor fixo do
             * viewport (map_rt.c). O player fica no centro; o mapa
             * recebe o deslocamento interpolado da câmera da POC. */
             map_framebuffer_begin(gsGlobal);
             render_game_map(gsGlobal, &gp_map, map_shift_x, map_shift_y,
                          anim_tick, player_walk_phase,
                              gp_own_dir(), 1);
             map_framebuffer_end(gsGlobal);

            /* apresentar o RT como uma única textura no viewport */
            map_framebuffer_present(gsGlobal);
#else
            /* fallback: render direto (comparação/debug) */
            gsKit_set_scissor(
                gsGlobal,
                GS_SETREG_SCISSOR_1(VIEW_X, VIEW_X + VIEW_W - 1, VIEW_Y,
                                     VIEW_Y + VIEW_H - 1));
              render_game_map(gsGlobal, &gp_map, map_shift_x, map_shift_y,
                           anim_tick, player_walk_phase,
                               gp_own_dir(), 0);
             gsKit_set_scissor(gsGlobal, GS_SCISSOR_RESET);
#endif

            /* cursor de alvo */
            if (cur_dx || cur_dy) {
                int cx = VIEW_X + VIEW_W / 2 - TILE_SIZE / 2 + cur_dx * TILE_SIZE;
                int cy = VIEW_Y + VIEW_H / 2 - TILE_SIZE / 2 + cur_dy * TILE_SIZE;
                uint32_t c = UI_COLOR(0xFF, 0xFF, 0x40);
                ui_rect(gsGlobal, cx, cy, TILE_SIZE, 1, c);
                ui_rect(gsGlobal, cx, cy + TILE_SIZE - 1, TILE_SIZE, 1, c);
                ui_rect(gsGlobal, cx, cy, 1, TILE_SIZE, c);
                ui_rect(gsGlobal, cx + TILE_SIZE - 1, cy, 1, TILE_SIZE, c);
            }

            hud_render(gsGlobal, &gp_map, show_stats);
            hud_equipment_window(gsGlobal, &gp_map, show_equipment);

            /* debug do pad: mostra o que o controle está mandando */
            {
                char dbg[80];
                snprintf(dbg, sizeof(dbg), "pad=%04X edge=%04X anim=%d pend=%d",
                         (unsigned)last_pad_held, (unsigned)last_pad_edge,
                         walk_anim,
                         gp_move_pending());
                    font_draw_scaled(gsGlobal, dbg, VIEW_X, 440,
                               UI_COLOR(0x80, 0xFF, 0x80), 1);
            }

            /* Fase 6: log periódico das estatísticas do cache */
            spr_stats_frame();
            if (++stats_frames >= 1800) { /* log raro: nao mascarar yield da rede */
                stats_frames = 0;
                const SprStats *st = spr_get_stats();
                printf("spr-cache: resident=%d/%d misses=%d evictions=%d starved=%d deferred=%d\n",
                       st->resident, st->capacity, st->misses_total, st->evictions,
                       st->starved, st->deferred);
                spr_stats_tick();
            }

            gsKit_queue_exec(gsGlobal);
        }
    }

    return 0;
}

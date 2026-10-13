/**
 * loading.c — implementação da tela de loading (ver loading.h).
 *
 * Cadência gsKit: usa um loop de 2 iterações com sync_flip ao final
 * para garantir que AMBOS os buffers do Double Buffering (A e B)
 * recebam a imagem de loading. Isso evita o "flicker" (piscadeira)
 * na transição para a próxima tela (ex: login screen).
 */
#include <gsKit.h>

#include "font.h"
#include "loading.h"

/* Dourado Tibia (título "tibia-ps2" do login screen) + trilho escuro. */
#define LOADING_BAR_COLOR UI_COLOR(0xFF, 0xD0, 0x40)
#define LOADING_TRACK_COLOR UI_COLOR(0x3E, 0x2E, 0x0E)

/* Métrica do gameshell_draw_progress: barra 304x34, fill = percent*3. */
#define BAR_W 304
#define BAR_H 34
#define BAR_FILL_W 300 /* BAR_W - 4 (borda de 2px) */

void loading_draw(GSGLOBAL *gs, const char *message, int percent)
{
    if (!gs)
        return;
    if (percent < 0)
        percent = 0;
    if (percent > 100)
        percent = 100;

    /* Desenha 2x: preenche Buffer A e Buffer B. */
    for (int i = 0; i < 2; i++) {
        /* Alpha 0x80 e OBRIGATORIO aqui: o main configura o alpha test
         * (ATE=1, ATST=GREATER, AREF=0), que descarta todo pixel com
         * alpha 0 — gsKit_clear com alpha 0 e um no-op silencioso e a
         * tela de loading nunca seria apagada (mesma licao do clear do
         * loop do jogo, que usa 0xFF). */
        gsKit_clear(gs, GS_SETREG_RGBAQ(0, 0, 0, 0x80, 0));

        int x = (LOADING_SCREEN_W - BAR_W) / 2;
        int y = LOADING_SCREEN_H / 2 - 18;

        /* Moldura e preenchimento. */
        ui_rect(gs, x, y, BAR_W, BAR_H, LOADING_BAR_COLOR);
        ui_rect(gs, x + 2, y + 2, BAR_FILL_W, BAR_H - 4, LOADING_TRACK_COLOR);
        ui_rect(gs, x + 2, y + 2, percent * 3, BAR_H - 4, LOADING_BAR_COLOR);

        if (message)
            font_draw(gs, message,
                      font_centered_x(message, LOADING_SCREEN_W, 2),
                      y + BAR_H + 10, UI_COLOR(0xFF, 0xFF, 0xFF));

        gsKit_queue_exec(gs);
        gsKit_sync_flip(gs);
    }
}

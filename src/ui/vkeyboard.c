/**
 * vkeyboard.c — teclado virtual on-screen navegável por D-pad.
 *
 * Layout: 4 linhas de teclas (letras/números/pontuação + ações).
 * Renderiza com a fonte 5x7; o loop é modal (vsync + pad).
 */
#include <kernel.h>
#include <gsKit.h>
#include <tamtypes.h>
#include <string.h>
#include <stdio.h>

#include "vkeyboard.h"
#include "font.h"
#include "pad.h"

#define VK_ROWS 4
#define VK_KEY_W 14
#define VK_KEY_H 18

static const char *vk_rows[VK_ROWS] = {
    "1234567890",
    "qwertyuiop",
    "asdfghjkl-",
    "zxcvbnm,._",
};

/* ações especiais por linha (colunas após o texto) */
#define VK_ACT_BACKSPACE 1
#define VK_ACT_SPACE 2
#define VK_ACT_OK 3

static void draw_panel(GSGLOBAL *gs)
{
    int w = 640, h = 180;
    int x = 0, y = 448 - h;
    ui_rect(gs, x, y, w, h, UI_COLOR(0x10, 0x10, 0x14));
    ui_rect(gs, x, y, w, 1, UI_COLOR(0x70, 0x70, 0x80));
}

static void draw_kb(GSGLOBAL *gs, int sel_row, int sel_col, int shift,
                     const char *buf, const char *title)
{
    gsKit_clear(gs, GS_SETREG_RGBAQ(0, 0, 0, 0, 0));

    draw_panel(gs);

    int y = 448 - 180 + 10;
    font_draw(gs, title, 16, y, UI_COLOR(0xFF, 0xD0, 0x40));
    y += FONT_GLYPH_H * 2 + 4;
    font_draw(gs, buf, 16, y, UI_COLOR(0xFF, 0xFF, 0xFF));
    ui_rect(gs, 16, y + FONT_GLYPH_H * 2, 200, 1, UI_COLOR(0x40, 0x40, 0x50));
    y += FONT_GLYPH_H * 2 + 12;

    for (int r = 0; r < VK_ROWS; r++) {
        int cols = (int)strlen(vk_rows[r]);
        for (int c = 0; c < cols; c++) {
            int x = 16 + c * (VK_KEY_W + 2);
            uint32_t bg = UI_COLOR(0x30, 0x30, 0x3A);
            if (r == sel_row && c == sel_col)
                bg = UI_COLOR(0x90, 0x70, 0x20);
            ui_rect(gs, x, y, VK_KEY_W, VK_KEY_H, bg);
            char s[2] = { vk_rows[r][c], 0 };
            if (shift && s[0] >= 'a' && s[0] <= 'z')
                s[0] = (char)(s[0] - 'a' + 'A');
            font_draw(gs, s, x + 1, y + 1, UI_COLOR(0xFF, 0xFF, 0xFF));
        }
        y += VK_KEY_H + 2;
    }

    /* ações */
    int ax = 300, ay = 448 - 180 + 10 + FONT_GLYPH_H * 2 + 4 +
                         FONT_GLYPH_H * 2 + 12;
    const char *acts[3] = { "DEL", "SPACE", "OK" };
    for (int i = 0; i < 3; i++) {
        int r = i;
        int c = -1;
        uint32_t bg = UI_COLOR(0x30, 0x30, 0x3A);
        if (sel_row == r && sel_col == (int)strlen(vk_rows[r])) {
            bg = UI_COLOR(0x20, 0x70, 0x90);
            c = 0;
        }
        ui_rect(gs, ax, ay + r * (VK_KEY_H + 2), 80, VK_KEY_H, bg);
        font_draw(gs, acts[i], ax + 2, ay + r * (VK_KEY_H + 2) + 2,
                  UI_COLOR(0xFF, 0xFF, 0xFF));
        (void)c;
    }

    font_draw_scaled(gs, "D-pad move  X insere  TRI del  L1 shift  START ok  O cancela",
                     16, 448 - 18, UI_COLOR(0xA0, 0xA0, 0xA0), 1);

    gsKit_queue_exec(gs);
    gsKit_sync_flip(gs);
}

int vkeyboard_run(GSGLOBAL *gs, const char *title, char *out, int cap,
                  const char *initial)
{
    char buf[64];
    int len = 0;
    int sel_row = 1, sel_col = 0;
    int shift = 0;
    int repeat_cooldown = 0;

    if (initial) {
        strncpy(buf, initial, sizeof(buf) - 1);
        buf[sizeof(buf) - 1] = 0;
        len = (int)strlen(buf);
    } else {
        buf[0] = 0;
    }

    for (;;) {
        int held;
        uint32_t edge = pad2_read_edge(&held);

        if (repeat_cooldown > 0)
            repeat_cooldown--;

        /* navegação (edge + repeat) */
        if ((edge & PAD_BTN_DOWN) ||
            ((held & PAD_BTN_DOWN) && repeat_cooldown == 0)) {
            sel_row = (sel_row + 1) % (VK_ROWS + 1);
            repeat_cooldown = 8;
        }
        if ((edge & PAD_BTN_UP) ||
            ((held & PAD_BTN_UP) && repeat_cooldown == 0)) {
            sel_row = (sel_row + VK_ROWS) % (VK_ROWS + 1);
            repeat_cooldown = 8;
        }
        int row_cols = (sel_row < VK_ROWS) ? (int)strlen(vk_rows[sel_row]) + 1
                                           : 1;
        if ((edge & PAD_BTN_RIGHT) ||
            ((held & PAD_BTN_RIGHT) && repeat_cooldown == 0)) {
            sel_col = (sel_col + 1) % row_cols;
            repeat_cooldown = 8;
        }
        if ((edge & PAD_BTN_LEFT) ||
            ((held & PAD_BTN_LEFT) && repeat_cooldown == 0)) {
            sel_col = (sel_col + row_cols - 1) % row_cols;
            repeat_cooldown = 8;
        }

        if (edge & PAD_BTN_L1)
            shift = !shift;
        if (edge & PAD_BTN_CIRCLE)
            return 0;
        if (edge & PAD_BTN_START) {
            if (len > 0) {
                strncpy(out, buf, (size_t)cap - 1);
                out[cap - 1] = 0;
                return 1;
            }
        }
        if (edge & PAD_BTN_TRI) {
            if (len > 0)
                buf[--len] = 0;
        }
        if (edge & PAD_BTN_CROSS) {
            if (sel_row < VK_ROWS) {
                if (sel_col < (int)strlen(vk_rows[sel_row]) && len < 63) {
                    char c = vk_rows[sel_row][sel_col];
                    if (shift && c >= 'a' && c <= 'z')
                        c = (char)(c - 'a' + 'A');
                    buf[len++] = c;
                    buf[len] = 0;
                }
            } else if (len < 63) {
                buf[len++] = ' ';
                buf[len] = 0;
            }
        }
        /* quadrado = espaço rápido */
        if (edge & PAD_BTN_SQUARE) {
            if (len < 63) {
                buf[len++] = ' ';
                buf[len] = 0;
            }
        }

        draw_kb(gs, sel_row, sel_col, shift, buf, title);
    }
}

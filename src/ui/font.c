/**
 * font.c — textura única 128x64 com os 95 glifos 5x7 (16 por linha),
 * desenhados como sub-regiões via UV. Alpha test recorta os pixels
 * vazios (mesmo esquema dos sprites do mapa).
 */
#include <kernel.h>
#include <gsKit.h>
#include <tamtypes.h>
#include <string.h>
#include <stdio.h>

#include "font.h"
#include "font5x7.h"

#define FONT_TEX_W 128
#define FONT_TEX_H 64
#define GLYPHS_PER_ROW 16

static GSTEXTURE font_tex;
static u32 font_pixels[FONT_TEX_W * FONT_TEX_H] __attribute__((aligned(16)));
static int font_ready = 0;

int font_init(GSGLOBAL *gs)
{
    if (font_ready)
        return 0;

    memset(font_pixels, 0, sizeof(font_pixels));
    for (int g = 0; g < 95; g++) {
        int cx = (g % GLYPHS_PER_ROW) * 8;
        int cy = (g / GLYPHS_PER_ROW) * 8;
        for (int col = 0; col < 5; col++) {
            uint8_t bits = font5x7[g * 5 + col];
            for (int row = 0; row < 7; row++) {
                if (bits & (1u << row)) {
                    int px = cx + col;
                    int py = cy + row;
                    font_pixels[py * FONT_TEX_W + px] =
                        0x80FFFFFFu; /* RGBA: branco opaco */
                }
            }
        }
    }

    FlushCache(0);

    font_tex.Width = FONT_TEX_W;
    font_tex.Height = FONT_TEX_H;
    font_tex.PSM = GS_PSM_CT32;
    font_tex.Filter = GS_FILTER_NEAREST;
    font_tex.Delayed = 0;
    font_tex.Mem = font_pixels;
    font_tex.Vram = gsKit_vram_alloc(
        gs, gsKit_texture_size(FONT_TEX_W, FONT_TEX_H, GS_PSM_CT32),
        GSKIT_ALLOC_USERBUFFER);
    if (font_tex.Vram == GSKIT_ALLOC_ERROR) {
        printf("font: alloc de VRAM falhou\n");
        return -1;
    }

    gsKit_texture_upload(gs, &font_tex);
    FlushCache(0);
    font_ready = 1;
    return 0;
}

int font_draw_scaled(GSGLOBAL *gs, const char *s, int x, int y,
                     uint32_t color, int scale)
{
    if (!font_ready || scale < 1)
        return 0;

    int r = (int)((color >> 16) & 0xFF);
    int g = (int)((color >> 8) & 0xFF);
    int b = (int)(color & 0xFF);
    u64 rgbaq = GS_SETREG_RGBAQ((u8)r, (u8)g, (u8)b, 0x80, 0x00);

    int px = x;
    for (const char *p = s; *p; p++) {
        unsigned char c = (unsigned char)*p;
        if (c == ' ') {
            px += FONT_GLYPH_W * scale;
            continue;
        }
        if (c < 32 || c > 126)
            continue;
        int gi = c - 32;
        float u0 = (float)((gi % GLYPHS_PER_ROW) * 8);
        float v0 = (float)((gi / GLYPHS_PER_ROW) * 8);

        gsKit_prim_sprite_texture(gs, &font_tex,
            (float)px, (float)y, u0, v0,
            (float)px + 5.0f * scale, (float)y + 7.0f * scale,
            u0 + 5.0f, v0 + 7.0f, 0, rgbaq);
        px += FONT_GLYPH_W * scale;
    }
    return px - x;
}

int font_draw(GSGLOBAL *gs, const char *s, int x, int y, uint32_t color)
{
    return font_draw_scaled(gs, s, x, y, color, 2);
}

void ui_rect(GSGLOBAL *gs, int x, int y, int w, int h, uint32_t color)
{
    int r = (int)((color >> 16) & 0xFF);
    int g = (int)((color >> 8) & 0xFF);
    int b = (int)(color & 0xFF);
    gsKit_prim_sprite(gs, (float)x, (float)y, (float)(x + w), (float)(y + h),
                      0, GS_SETREG_RGBAQ((u8)r, (u8)g, (u8)b, 0x80, 0));
}

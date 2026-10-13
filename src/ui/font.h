/**
 * font.h — render de texto 5x7 no PS2 (uma textura com todos os glifos).
 */
#ifndef UI_FONT_H
#define UI_FONT_H

#include <gsKit.h>
#include <tamtypes.h>
#include <stdint.h>

#define FONT_GLYPH_W 6
#define FONT_GLYPH_H 8

int font_init(GSGLOBAL *gs); /* sobe a textura p/ VRAM; 0 ok */

/* desenha string; retorna pixels usados. color = 0xRRGGBB */
int font_draw(GSGLOBAL *gs, const char *s, int x, int y, uint32_t color);
int font_draw_scaled(GSGLOBAL *gs, const char *s, int x, int y,
					 uint32_t color, int scale);

/* largura em px (glyph 6x8 * scale) e x p/ texto centralizado */
int font_text_width(const char *s, int scale);
int font_centered_x(const char *s, int screen_w, int scale);

/* painel: retângulo sólido (sem alpha — HUD) */
void ui_rect(GSGLOBAL *gs, int x, int y, int w, int h, uint32_t color);

/*Helpers de cor */
#define UI_COLOR(r, g, b) (((uint32_t)(r) << 16) | ((uint32_t)(g) << 8) | (uint32_t)(b))

#endif /* UI_FONT_H */

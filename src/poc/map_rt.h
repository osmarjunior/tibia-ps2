/**
 * map_rt.h — Offscreen render target in VRAM for the PS2 map.
 */
#ifndef MAP_RT_H
#define MAP_RT_H

#include <kernel.h>
#include <gsKit.h>
#include <tamtypes.h>

/*
 * Viewport rectangle, in screen pixels.
 * Screen is 640x448; viewport is 15x11 tiles of 32 px, origin (48, 48),
 * size 480x352. 15 and 11 are odd, so the center of the viewport is
 * the center of a tile (player lands tile-aligned). The right column
 * (x 528..640) stays free of the map for the HUD windows (battle list
 * and equipment).
 */
#define VIEW_X 48
#define VIEW_Y 48
#define VIEW_W 480
#define VIEW_H 352

int  map_framebuffer_init(GSGLOBAL *gs);
void map_framebuffer_begin(GSGLOBAL *gs);
void map_framebuffer_end(GSGLOBAL *gs);
void map_framebuffer_present(GSGLOBAL *gs);
u32  map_framebuffer_get_vram(void);

#endif /* MAP_RT_H */

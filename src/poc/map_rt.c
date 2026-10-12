/**
 * map_rt.c — Offscreen render target in VRAM for the PS2 map.
 *
 * Renders the map to a separate VRAM region (render target, "RT"),
 * then presents it as a single texture at the viewport's fixed screen
 * position (VIEW_X, VIEW_Y), 1:1, no scaling.
 *
 * Method B (queued GIF_AD packets) because gsKit_setactive() uses
 * direct DMA which does not guarantee ordering with queued commands.
 * Consequence: gsKit_setactive() is NOT used for the RT switch, so the
 * SCISSOR is NOT automatically re-sent — map_framebuffer_end() must
 * restore the full-screen scissor itself.
 *
 * RT layout:
 *   stride  = gsGlobal->Width (640) — FRAME FBW and TEX0 TBW both use
 *             Width/64 = 10, the same convention as gsKit_setactive().
 *   height  = VIEW_H (352) — allocation is viewport-sized: one CT16
 *             page is 64x64 px = 8 KB, so
 *             pages = ceil(640/64) * ceil(VIEW_H/64) = 10*6 = 60.
 *
 * The scissor in map_framebuffer_begin() is MANDATORY, not cosmetic:
 * the RT is only VIEW_H tall, and sprites that cross the bottom edge
 * (border tiles can extend up to 32 px past it) would write beyond the
 * allocation, over the tile textures that follow in VRAM.
 *
 * gsKit TW/TH quirk (gsKit_set_tw_th, gsTexture.c): the width/height of
 * a GSTEXTURE are rounded up to the next power of two with a single
 * increment, so 640 -> TW=9 (512) and 352 -> TH=8 (256). Presenting
 * 544x352 with such a descriptor would WRAP the UVs at 512x256 and
 * sample the wrong texels. The descriptor therefore keeps the
 * power-of-two dims 1024x512 (TW=10, TH=9, safe for UV 544x352; also
 * the UV clamp range). Width/Height are ONLY used for TW/TH and UV
 * clamping here — the RT is never uploaded (Mem = NULL) — while TBW
 * below carries the REAL stride.
 */
#include <kernel.h>
#include <gsKit.h>
#include <gsInline.h>
#include <dmaKit.h>
#include <tamtypes.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>

#include "map_rt.h"

/* Descriptor dimensions: power-of-2 only for gsKit's TW/TH math and
 * UV clamping (see file comment). NOT the allocation size. */
#define RT_TEX_W 1024
#define RT_TEX_H 512

#define RT_PSM GS_PSM_CT16

static GSTEXTURE mapFramebuffer;
static int map_rt_initialized = 0;
static u32 saved_screen_buffer = 0;
static u32 saved_psm = 0;

/*
 * Allocate VRAM for the RT. Must be called AFTER gsKit_init_screen
 * and BEFORE any sprite loading (so sprites don't overwrite the RT).
 *
 * FBP (frame buffer pointer) has 8192-byte granularity, so the
 * allocation must be aligned to a multiple of 8192.
 */
int map_framebuffer_init(GSGLOBAL *gs)
{
    if (map_rt_initialized) return 0;

    /* Pad CurrentPointer to an 8192-byte boundary before allocating */
    u32 pad = (0x2000 - (gs->CurrentPointer & 0x1FFF)) & 0x1FFF;
    if (pad) {
        u32 pad_addr = gsKit_vram_alloc(gs, pad, GSKIT_ALLOC_USERBUFFER);
        if (pad_addr == GSKIT_ALLOC_ERROR) {
            printf("map_rt: pad alloc failed (%u bytes)\n", pad);
            return -1;
        }
    }

    printf("map_rt: CurrentPointer before RT = 0x%08X\n", gs->CurrentPointer);

    /* Viewport-sized allocation, in 8 KB pages (CT16 page = 64x64 px):
     * stride = screen width, height = viewport height. */
    u32 pages = (((u32)gs->Width + 63) / 64) * (((u32)VIEW_H + 63) / 64);
    u32 size  = pages * 8192;

    memset(&mapFramebuffer, 0, sizeof(GSTEXTURE));
    mapFramebuffer.Width   = RT_TEX_W;      /* pow2: TW/TH + UV clamp only */
    mapFramebuffer.Height  = RT_TEX_H;      /* pow2: TW/TH + UV clamp only */
    mapFramebuffer.PSM     = RT_PSM;
    mapFramebuffer.Filter  = GS_FILTER_NEAREST;
    mapFramebuffer.Delayed = 0;
    mapFramebuffer.Mem     = NULL;
    mapFramebuffer.TBW     = gs->Width / 64; /* REAL stride, in 64-px units */
    mapFramebuffer.Vram    = gsKit_vram_alloc(gs, size, GSKIT_ALLOC_USERBUFFER);

    if (mapFramebuffer.Vram == GSKIT_ALLOC_ERROR) {
        printf("map_rt: RT alloc failed (%u bytes)\n", size);
        return -2;
    }
    if (mapFramebuffer.Vram & 0x1FFF) {
        printf("map_rt: RT not 8K-aligned: 0x%08X\n", mapFramebuffer.Vram);
        return -3;
    }

    printf("map_rt: RT allocated at 0x%08X (%u pages, %u bytes, stride %u px, %ux%u viewport)\n",
           mapFramebuffer.Vram, pages, size, gs->Width, VIEW_W, VIEW_H);
    printf("map_rt: CurrentPointer after RT = 0x%08X\n", gs->CurrentPointer);

    map_rt_initialized = 1;
    return 0;
}

/*
 * Queue a GIF_AD packet that changes FRAME_1 and FRAME_2.
 * This goes through the drawing queue, guaranteeing correct ordering
 * with other queued drawing commands.
 *
 * fbw is the target's stride in 64-px units: gs->Width / 64 for the
 * back buffer (same as gsKit_setactive), and the same value for the RT
 * (its stride is also gs->Width).
 *
 * Convention copied from gsKit_set_primalpha (gsCore.c):
 *   gsKit_heap_alloc(gs, N, N*16, GIF_AD)
 *   GIF_TAG_AD(N), GIF_AD, then N × (value, register) pairs.
 */
static void map_rt_queue_frame(GSGLOBAL *gs, u32 vram_addr, u32 fbw, u32 psm)
{
    u64 *p_data = gsKit_heap_alloc(gs, 2, 32, GIF_AD);

    *p_data++ = GIF_TAG_AD(2);
    *p_data++ = GIF_AD;

    /* Context 1 */
    *p_data++ = GS_SETREG_FRAME_1(vram_addr / 8192, fbw, psm, 0);
    *p_data++ = GS_FRAME_1;

    /* Context 2 (keep both in sync, same convention as gsKit_setactive) */
    *p_data++ = GS_SETREG_FRAME_1(vram_addr / 8192, fbw, psm, 0);
    *p_data++ = GS_FRAME_2;
}

/*
 * Queue a TEXFLUSH. Must be executed between the last write to the RT
 * and the first read as texture. Convention: single AD register write.
 */
static void map_rt_queue_texflush(GSGLOBAL *gs)
{
    u64 *p_data = gsKit_heap_alloc(gs, 1, 16, GIF_AD);

    *p_data++ = GIF_TAG_AD(1);
    *p_data++ = GIF_AD;
    *p_data++ = 0;
    *p_data++ = GS_TEXFLUSH;
}

/*
 * Begin rendering to the RT. Subsequent drawing commands target the RT
 * until map_framebuffer_end() is called. Coordinates inside the RT are
 * viewport-local: (0,0) = viewport origin.
 *
 * The SCISSOR is mandatory: the RT is only VIEW_H tall, and border
 * tiles drawn partially past the viewport would otherwise write over
 * the VRAM that follows (the tile textures).
 */
void map_framebuffer_begin(GSGLOBAL *gs)
{
    if (!map_rt_initialized) return;

    /* Save the real back buffer address and PSM */
    int idx = gs->ActiveBuffer & 1;
    saved_screen_buffer = gs->ScreenBuffer[idx];
    saved_psm = gs->PSM;

    /* Queue FRAME → RT (goes into the drawing queue, ordered) */
    map_rt_queue_frame(gs, mapFramebuffer.Vram, gs->Width / 64, RT_PSM);

    /* Clip everything to the viewport (queued, same order as above) */
    gsKit_set_scissor(gs, GS_SETREG_SCISSOR_1(0, VIEW_W - 1, 0, VIEW_H - 1));

    /* Clear the RT to black (draws a full-screen sprite; the scissor
     * above clips it to the viewport) */
    gsKit_clear(gs, GS_SETREG_RGBAQ(0x00, 0x00, 0x00, 0x80, 0x00));
}

/*
 * End rendering to the RT. Queue TEXFLUSH, queue FRAME change back to
 * the real back buffer, then restore the full-screen scissor (the
 * FRAME switch is queued, not gsKit_setactive, so nothing else re-sends
 * the scissor before the present and the HUD).
 */
void map_framebuffer_end(GSGLOBAL *gs)
{
    if (!map_rt_initialized) return;

    /* TEXFLUSH: ensures the RT data is visible for texture sampling */
    map_rt_queue_texflush(gs);

    /* Queue FRAME → back buffer (restored values, correct FBW) */
    map_rt_queue_frame(gs, saved_screen_buffer, gs->Width / 64, saved_psm);

    /* Full-screen scissor for present + HUD (back buffer coords) */
    gsKit_set_scissor(gs, GS_SCISSOR_RESET);
}

/*
 * Present the RT at the viewport's fixed screen position, 1:1, no
 * scaling. CT16 has only 1 bit of alpha, so blending must be OFF here
 * to avoid the alpha test discarding pixels with A=1.
 */
void map_framebuffer_present(GSGLOBAL *gs)
{
    if (!map_rt_initialized) return;

    /* Disable alpha blending for the present (CT16 alpha issues) */
    u8 prev_alpha = gs->PrimAlphaEnable;
    gs->PrimAlphaEnable = GS_SETTING_OFF;

    /* Draw the viewport region of the RT at its screen position */
    gsKit_prim_sprite_texture(
        gs, &mapFramebuffer,
        VIEW_X,          VIEW_Y,          0,     0,
        VIEW_X + VIEW_W, VIEW_Y + VIEW_H, VIEW_W, VIEW_H,
        0, GS_SETREG_RGBAQ(0x80, 0x80, 0x80, 0x80, 0x00));

    gs->PrimAlphaEnable = prev_alpha;
}

u32 map_framebuffer_get_vram(void)
{
    return map_rt_initialized ? mapFramebuffer.Vram : 0;
}

/**
 * tibia_sprites.c - gsKit texture manager for real Tibia 7.4 sprites
 *
 * PS2-specific fixes:
 * 1. gsKit_vram_alloc() to allocate VRAM (not Vram=0)
 * 2. FlushCache(0) after memcpy, before DMA upload
 *    (CPU writes via data cache; DMA reads directly from RAM —
 *     without flush, DMA sees stale zeros = black texture)
 */
#include <kernel.h>
#include <gsKit.h>
#include <dmaKit.h>
#include <tamtypes.h>
#include <stdint.h>
#include <string.h>

#include "spr_data_gen.h"

#define MAX_SPRITES NUM_VALID_SPRITES

static GSTEXTURE textures[MAX_SPRITES];
/* EE RAM staging buffer — 16-byte aligned for DMA */
static u32 pixel_buf[MAX_SPRITES][SPRITE_BYTES / 4] __attribute__((aligned(16)));
static int sprites_loaded = 0;

void tibia_sprites_init(GSGLOBAL *gsGlobal) {
    if (sprites_loaded) return;

    for (int i = 0; i < MAX_SPRITES; i++) {
        /* Copy const sprite data to writable EE buffer */
        memcpy(pixel_buf[i], SPRITE_DATA[i], SPRITE_BYTES);

        /* CRITICAL: flush data cache to RAM so DMA sees the real data */
        FlushCache(0); /* 0 = WRITEBACK_DCACHE */

        /* Allocate VRAM for this texture */
        u32 tex_vram_size = gsKit_texture_size(SPRITE_SIZE, SPRITE_SIZE, GS_PSM_CT32);
        u32 vram_addr = gsKit_vram_alloc(gsGlobal, tex_vram_size, GSKIT_ALLOC_USERBUFFER);

        if (vram_addr == GSKIT_ALLOC_ERROR) {
            continue; /* VRAM full — skip */
        }

        textures[i].Width = SPRITE_SIZE;
        textures[i].Height = SPRITE_SIZE;
        textures[i].PSM = GS_PSM_CT32;
        textures[i].Mem = pixel_buf[i];
        textures[i].Vram = vram_addr;
        textures[i].Filter = GS_FILTER_NEAREST;
        textures[i].Delayed = 0;

        /* Upload pixel data from EE RAM to GS VRAM via DMA */
        gsKit_texture_upload(gsGlobal, &textures[i]);

        /* Flush again after upload to ensure DMA completion is visible */
        FlushCache(0);
    }

    sprites_loaded = 1;
}

void tibia_sprite_draw(GSGLOBAL *gsGlobal, uint16_t spriteId, float x, float y, float size) {
    int idx = sprite_id_to_index(spriteId);
    if (idx < 0 || idx >= MAX_SPRITES) return;
    if (textures[idx].Vram == 0) return; /* allocation failed */

    gsKit_prim_sprite_texture(gsGlobal, &textures[idx],
        x, y, 0.0f, 0.0f,
        x + size, y + size, (float)SPRITE_SIZE, (float)SPRITE_SIZE,
        0, GS_SETREG_RGBAQ(0xFF, 0xFF, 0xFF, 0x80, 0x00));
}

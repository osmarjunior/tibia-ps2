/**
 * tibia_sprites.h - Runtime sprite generator + gsKit texture manager
 */
#ifndef TIBIA_SPRITES_H
#define TIBIA_SPRITES_H

#include <kernel.h>
#include <gsKit.h>
#include <tamtypes.h>

void tibia_sprites_init(GSGLOBAL *gsGlobal);
void tibia_sprite_draw(GSGLOBAL *gsGlobal, uint16_t spriteId, float x, float y, float size);

#endif /* TIBIA_SPRITES_H */

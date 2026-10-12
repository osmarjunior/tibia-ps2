/**
 * Sprites do Tibia 7.4 extraídos de Tibia.spr
 * Gerado por: python tools/extract_sprites.py Tibia.spr <ids...>
 *
 * IDs padrão para POC (Tibia 7.4):
 *   1  -> chão de grama       (ajustar conforme o .spr real)
 *   2  -> parede de pedra     (ajustar conforme o .spr real)
 *   3  -> item                (ajustar conforme o .spr real)
 *   4  -> criatura            (ajustar conforme o .spr real)
 */
#ifndef SPR_DATA_H
#define SPR_DATA_H

#define SPRITE_SIZE 32
#define PIXELS_PER_SPRITE (SPRITE_SIZE * SPRITE_SIZE)
#define SPRITE_BYTES (PIXELS_PER_SPRITE * 4)

/*
 * Estrutura de cada sprite extraído:
 *   - 32x32 pixels, 4 bytes por pixel (R, G, B, A)
 *   - Alpha 0 = transparente, Alpha 255 = opaco
 *
 * Para gerar os dados reais:
 *   python tools/extract_sprites.py Tibia.spr 98 1022 1053 1221 > src/poc/spr_data.h
 *
 * O arquivo gerado conterá arrays sprite_<id> com os pixels decodificados.
 */

/* Placeholder: será substituído pelo output do extrator */
#include "spr_data_gen.h"

#endif /* SPR_DATA_H */

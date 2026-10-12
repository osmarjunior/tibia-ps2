/**
 * render_game.h — renderer do GameMap (cliente de rede) sobre gsKit.
 *
 * Fase 3: desenha a GameMap que o game_protocol escreve (mesma struct
 * Tile/Thing; janela 18x14 com empilhamento diagonal já "pré-assado"
 * no armazenamento — cada floor desenha nas MESMAS coords de janela).
 * Fase 5: compute_floor_range() real (telhado corta floors de cima) +
 * fading de transição; profundidade: floor mais fundo primeiro.
 */
#ifndef RENDER_GAME_H
#define RENDER_GAME_H

#include <gsKit.h>
#include <tamtypes.h>
#include "../poc/game_map.h"

void render_game_map(GSGLOBAL *gs, GameMap *map, int cam_off_x, int cam_off_y,
                      int anim_tick, int player_walk_phase, int player_dir,
                      int viewport_local);

/* O player é desenhado DENTRO do render_game_map, no ponto correto do
 * scan diagonal (Tile::drawCreature do otclient) — não existe mais uma
 * passada separada, que quebrava o Z-order com paredes vizinhas. */

/* Resultado do compute_floor_range (exposto p/ HUD debug) */
typedef struct {
    int first; /* floor mais alto visível (z menor) */
    int last;  /* floor mais fundo visível (z maior) */
} FloorRange;
void compute_floor_range(GameMap *map, FloorRange *out);

#endif /* RENDER_GAME_H */

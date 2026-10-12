/**
 * hud.h — UI mínima jogável (Fase 8 do TASKS2.md):
 *   - barra de vida/mana
 *   - janela de chat (mensagens recebidas)
 *   - battle list (criaturas visíveis)
 *   - janela de container (itens)
 *   - janela de equipamentos (inventário do player, toggle L2)
 */
#ifndef UI_HUD_H
#define UI_HUD_H

#include <gsKit.h>
#include <tamtypes.h>
#include "../poc/game_map.h"

void hud_render(GSGLOBAL *gs, GameMap *map, int show_debug_stats);

/* Sobe as texturas dos placeholders de slot p/ VRAM. Chamar 1x no
 * boot, ANTES do primeiro sprite (o cache reserva a VRAM restante
 * na primeira carga — depois disso nao sobra espaco garantido). */
void hud_init(GSGLOBAL *gs);

/* Janela "Inventory" estilo Tibia 7.6: 10 slots equipados lidos de
 * gp_inventory[]. open = 0 fecha (não desenha nada). */
void hud_equipment_window(GSGLOBAL *gs, GameMap *map, int open);

#endif /* UI_HUD_H */

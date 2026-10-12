/**
 * vkeyboard.h — teclado virtual na tela (Fase 7 do TASKS2.md).
 *
 * Grid QWERTY navegável por D-pad; X insere, △ backspace, ○ cancela,
 * START confirma. Escolhido em vez de teclado USB por não depender de
 * periférico extra (o USB fica como alternativa futura).
 */
#ifndef UI_VKEYBOARD_H
#define UI_VKEYBOARD_H

#include <gsKit.h>
#include <tamtypes.h>

/* Executa o teclado modalmente (bloqueante). Retorna 1 se confirmado
 * (out preenchido, NUL garantido), 0 se cancelado. */
int vkeyboard_run(GSGLOBAL *gs, const char *title, char *out, int cap,
                  const char *initial);

#endif /* UI_VKEYBOARD_H */

/**
 * loading.h — tela de loading com barra de progresso.
 *
 * Espelho do gameshell_draw_progress do 2004sp-ps2port (repo de
 * referência), adaptado ao gsKit direto: função única e idempotente,
 * cada chamada limpa/desenha/apresenta UM frame — as fases de loading
 * são bloqueantes, não há loop de render concorrente.
 *
 * Barra 304x34 centrada, preenchimento = percent*3 (mesma métrica do
 * exemplo). Cor: dourado Tibia (mesma do título do login screen).
 */
#ifndef UI_LOADING_H
#define UI_LOADING_H

#include <gsKit.h>

/* Resolução do entry client_net.c (SCREEN_W/SCREEN_H). */
#define LOADING_SCREEN_W 640
#define LOADING_SCREEN_H 448

/* Desenha a tela de loading e apresenta o frame.
 * message: texto abaixo da barra (NULL = só a barra).
 * percent: 0..100. */
void loading_draw(GSGLOBAL *gs, const char *message, int percent);

#endif /* UI_LOADING_H */

/**
 * pad.h — helpers de controle (libpad).
 */
#ifndef UI_PAD_H
#define UI_PAD_H

#include <libpad.h>
#include <tamtypes.h>
#include <stdint.h>

#define PAD_BTN_SELECT (1u << 0)
#define PAD_BTN_L3     (1u << 1)
#define PAD_BTN_R3     (1u << 2)
#define PAD_BTN_START  (1u << 3)
#define PAD_BTN_UP     (1u << 4)
#define PAD_BTN_RIGHT  (1u << 5)
#define PAD_BTN_DOWN   (1u << 6)
#define PAD_BTN_LEFT   (1u << 7)
#define PAD_BTN_L2     (1u << 8)
#define PAD_BTN_R2     (1u << 9)
#define PAD_BTN_L1     (1u << 10)
#define PAD_BTN_R1     (1u << 11)
#define PAD_BTN_TRI    (1u << 12)
#define PAD_BTN_CROSS  (1u << 13)
#define PAD_BTN_CIRCLE (1u << 14)
#define PAD_BTN_SQUARE (1u << 15)

void pad2_init(void);

/* retorna bitmask dos botões PRESSIONADOS agora (0 se nada/erro) */
uint32_t pad2_read(void);

/* borda de subida: botões que acabaram de ser apertados (com hold) */
uint32_t pad2_read_edge(int *held_out);

#endif /* UI_PAD_H */

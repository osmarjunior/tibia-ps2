/**
 * pad.c — leitura do DualShock via libpad (com borda de subida + repeat).
 */
#include <kernel.h>
#include <loadfile.h>
#include <sifrpc.h>
#include <libpad.h>
#include <string.h>
#include <tamtypes.h>

#include "pad.h"

static u8 padBuf[256] __attribute__((aligned(64)));
static uint32_t prev_state = 0;

void pad2_init(void)
{
    SifLoadFileInit();
    SifLoadModule("rom0:SIO2MAN", 0, NULL);
    SifLoadModule("rom0:PADMAN", 0, NULL);
    padInit(0);
    padPortOpen(0, 0, padBuf);
    int tries = 0;
    while (padGetState(0, 0) != PAD_STATE_STABLE && tries < 200) {
        tries++;
        for (volatile int i = 0; i < 10000; i++)
            ;
    }
}

uint32_t pad2_read(void)
{
    struct padButtonStatus pad;
    u32 ret = padRead(0, 0, &pad);
    if (ret == 0)
        return 0;
    /* libpad: 0 = pressionado */
    return (uint32_t)(~pad.btns & 0xFFFF);
}

uint32_t pad2_read_edge(int *held_out)
{
    uint32_t now = pad2_read();
    uint32_t edge = now & ~prev_state;
    prev_state = now;
    if (held_out)
        *held_out = now;
    return edge;
}

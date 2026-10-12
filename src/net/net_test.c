/**
 * net_test.c — Fase 1 do TASKS2.md: conectividade TCP/IP crua.
 *
 * main() isolado (independente do renderer/poc.c do mapa): inicia a
 * stack de rede, abre um socket TCP para um servidor de echo e verifica
 * que os bytes enviados voltam identicos.
 *
 * Resultado em DOIS lugares:
 *  - printf -> log do PCSX2 (fonte "EE Console")
 *  - tela do PS2 -> ultimas linhas de log + veredito (fica em loop;
 *    nao retorna, entao o console nao reboota pra tela preta)
 *
 * Configuracao (defaults compilaveis via -D):
 *   NET_TEST_HOST  — IP do servidor de echo (rodar tools/echo_server.py nele)
 *   NET_TEST_PORT  — porta do echo (default 7777)
 *   ECHO_ROUNDS    — quantas rodadas de teste (default 3)
 *
 * Definicao de pronto da fase: log mostrando bytes enviados == bytes
 * recebidos de volta por um servidor de teste, sem timeout/erro.
 */
#include <kernel.h>
#include <loadfile.h>
#include <stdio.h>
#include <stdarg.h>
#include <string.h>
#include <tamtypes.h>

#include <gsKit.h>
#include <dmaKit.h>
#include <libpad.h>

#include "ps2_net.h"
#include "../platform.h"
#include "../ui/font.h"

extern int rs2_log_file_available(void);
extern void rs2_log_file_open_errors(int *host_error, int *relative_error);

#ifndef NET_TEST_HOST
#define NET_TEST_HOST "192.168.1.2"
#endif
#ifndef NET_TEST_PORT
#define NET_TEST_PORT 7777
#endif
#ifndef ECHO_ROUNDS
#define ECHO_ROUNDS 3
#endif

static const char payload[] =
    "PS2NET-ECHO-TEST 0123456789 ABCDEFGHIJKLMNOPQRSTUVWXYZ abcdefghijklmnop"
    "qrstuvwxyz - tibia-ps2 fase 1 - bytes fixos, ida e volta.";

/* ---- log que vai pro rs2_log E pra tela ---- */
#define SCR_LINES 12
#define SCR_LINE_LEN 50

static char scr_lines[SCR_LINES][SCR_LINE_LEN];
static int scr_count = 0;
static const char *verdict = NULL; /* linha final colorida */
static uint32_t verdict_color = 0;
static char pad_status[32] = "controle: aguardando";
static char network_stage[64] = "Aguardando inicio da rede...";

static GSGLOBAL *gs;
static u8 pad_dma_buf[256] __attribute__((aligned(64)));
static u16 pad_prev_buttons = 0xFFFF;

static void screen_update(void);

static void network_progress(const char *stage)
{
    strncpy(network_stage, stage, sizeof(network_stage) - 1);
    network_stage[sizeof(network_stage) - 1] = 0;
    screen_update();
    /* No next frame loop runs while ps2_net_init() is waiting. */
    gsKit_sync_flip(gs);
}

static void tlog(const char *fmt, ...)
{
    char line[192];
    va_list ap;

    va_start(ap, fmt);
    vsnprintf(line, sizeof(line), fmt, ap);
    va_end(ap);

    rs2_log("%s\n", line);

    if (scr_count < SCR_LINES) {
        strncpy(scr_lines[scr_count], line, SCR_LINE_LEN - 1);
        scr_lines[scr_count][SCR_LINE_LEN - 1] = 0;
        scr_count++;
    } else {
        memmove(scr_lines[0], scr_lines[1],
                (SCR_LINES - 1) * SCR_LINE_LEN);
        strncpy(scr_lines[SCR_LINES - 1], line, SCR_LINE_LEN - 1);
        scr_lines[SCR_LINES - 1][SCR_LINE_LEN - 1] = 0;
    }
}

static void screen_update(void)
{
    /* Standard gsKit cadence: synchronize/flip, then draw the new backbuffer. */
    gsKit_sync_flip(gs);
    gsKit_clear(gs, GS_SETREG_RGBAQ(0x00, 0x00, 0x10, 0xFF, 0x00));

    font_draw_scaled(gs, "tibia-ps2 - Fase 1: teste TCP/IP", 12, 12,
                     UI_COLOR(0xFF, 0xD0, 0x40), 2);
    font_draw_scaled(gs, network_stage, 12, 42,
                     UI_COLOR(0xFF, 0xE0, 0x70), 2);

    int y = 76;
    for (int i = 0; i < scr_count; i++) {
        font_draw_scaled(gs, scr_lines[i], 12, y,
                         UI_COLOR(0xD0, 0xF0, 0xD0), 2);
        y += 20;
    }

    if (verdict)
        font_draw_scaled(gs, verdict, 12, y + 4, verdict_color, 2);
    font_draw_scaled(gs, pad_status, 12, 420, UI_COLOR(0xC0, 0xC0, 0xC0), 2);

    gsKit_queue_exec(gs);
}

static void pad_init(void)
{
    /* Load pad drivers after network bring-up to avoid contention with SMAP. */
    SifLoadModule("rom0:SIO2MAN", 0, NULL);
    SifLoadModule("rom0:PADMAN", 0, NULL);
    padInit(0);
    padPortOpen(0, 0, pad_dma_buf);
}

static void poll_pad(void)
{
    struct padButtonStatus pad;
    if (padGetState(0, 0) != PAD_STATE_STABLE || padRead(0, 0, &pad) == 0)
        return;

    u16 pressed = (u16)(pad_prev_buttons & ~pad.btns);
    pad_prev_buttons = pad.btns;
    if (pressed & PAD_CROSS) {
        strncpy(pad_status, "controle: X recebido", sizeof(pad_status) - 1);
        pad_status[sizeof(pad_status) - 1] = 0;
    } else if (pressed & PAD_CIRCLE) {
        strncpy(pad_status, "controle: O recebido", sizeof(pad_status) - 1);
        pad_status[sizeof(pad_status) - 1] = 0;
    }
}

static void hexdump_row(const char *label, const unsigned char *buf, int len)
{
    char row[128];
    int p = 0;

    p += snprintf(row + p, sizeof(row) - (size_t)p, "%s:", label);
    for (int i = 0; i < len && p < (int)sizeof(row) - 4; i++)
        p += snprintf(row + p, sizeof(row) - (size_t)p, " %02X", buf[i]);

    tlog("%.56s", row);
}

int main(void)
{
    /* video (para o resultado aparecer na tela, nao so no log) */
    dmaKit_init(D_CTRL_RELE_OFF, D_CTRL_MFD_OFF, D_CTRL_STS_UNSPEC,
                D_CTRL_STD_OFF, D_CTRL_RCYC_8, 1 << DMA_CHANNEL_GIF);
    dmaKit_chan_init(DMA_CHANNEL_GIF);

    gs = gsKit_init_global();
    gs->Width = 640;
    gs->Height = 448;
    gs->PSM = GS_PSM_CT24;
    gs->DoubleBuffering = GS_SETTING_ON;
    gs->ZBuffering = GS_SETTING_OFF;
    gsKit_init_screen(gs);
    if (font_init(gs) != 0)
        rs2_log("fonte falhou — saida apenas no log\n");

    tlog("=== Fase 1: teste de TCP/IP ===");
    tlog("alvo: %s:%d", NET_TEST_HOST, NET_TEST_PORT);
    tlog("host log: %s", rs2_log_file_available() ? "aberto" : "FALHOU ao abrir");
    tlog("iniciando rede; aguarde resultado...");
    ps2_net_set_progress_callback(network_progress);
    screen_update();
    /* This call blocks for link/DHCP; present the queued frame first. */
    gsKit_sync_flip(gs);

    int net_r = ps2_net_init();
    pad_init();
    if (rs2_log_file_available()) {
        tlog("host log apos init: aberto (host:net_test.log)");
    } else {
        int host_error, relative_error;
        rs2_log_file_open_errors(&host_error, &relative_error);
        tlog("host log falhou: host errno=%d rel errno=%d",
             host_error, relative_error);
    }
    {
        /* diagnóstico do init na tela (passo a passo) */
           tlog("mod: DEV9=%d NM=%d SMAP=%d NetMan=%d",
               ps2_net_dbg.dev9, ps2_net_dbg.netman, ps2_net_dbg.smap,
               ps2_net_dbg.netman_init_ret);
        tlog("link=%d ps2ip=%d getcfg=%d setcfg=%d",
             ps2_net_dbg.link_state, ps2_net_dbg.ps2ip_ret,
             ps2_net_dbg.getconfig_ret, ps2_net_dbg.setconfig_ret);
        tlog("dhcp_status=%d ip=%x%s", ps2_net_dbg.dhcp_status,
             ps2_net_dbg.ipaddr,
             ps2_net_dbg.static_fallback ? " (estatico!)" : "");
        /* netif_default em offsets ESTAVEIS */
        tlog("ndflt=%08x ip=%x nm=%x gw=%x",
             ps2_net_dbg.netif_default_addr,
             ps2_net_dbg.nif_ip, ps2_net_dbg.nif_netmask,
             ps2_net_dbg.nif_gw);
        hexdump_row("nif0", ps2_net_dbg.netif_raw, 16);
        hexdump_row("nif1", ps2_net_dbg.netif_raw + 16, 16);
    }
    if (net_r != 0) {
        switch (net_r) {
        case -2: tlog("FALHOU: ps2ipInit (-2)");
                 tlog("=> conferir 'mod:' acima (DEV9/NETMAN/SMAP)");
                 break;
        case -3: tlog("FALHOU: link nunca subiu (-3)");
                 tlog("=> Ethernet Device Type = 'Sockets' no PCSX2?");
                 tlog("   (parar emulacao, aplicar, rodar ELF de novo)");
                 break;
        case -4: tlog("FALHOU: DHCP/IP (-4)");
                 tlog("=> status: 0=OFF 2=INIT 6=SELECT 10=BOUND");
                 tlog("   getcfg retorma pode nao ser 1!");
                 tlog("   ver ndflt= acima pra diagnosticar");
                 break;
        default: tlog("FALHOU: init da rede (%d)", net_r); break;
        }
        verdict = "Fase 1: FALHOU (rede nao subiu)";
        verdict_color = UI_COLOR(0xFF, 0x60, 0x60);
        goto end;
    }

    screen_update();
    gsKit_sync_flip(gs);

    {
        char myip[16] = "(erro)", mygw[16] = "", mymask[16] = "";
        ps2_net_get_ip(myip, sizeof(myip));
        tlog("PS2 IP: %s", myip);
        if (ps2_net_get_netmask(mymask, sizeof(mymask)) == 0)
            tlog("mascara: %s", mymask);
        if (ps2_net_get_gateway(mygw, sizeof(mygw)) == 0)
            tlog("gateway: %s", mygw);
    }
    screen_update();
        gsKit_sync_flip(gs);

        strncpy(network_stage, "Conectando ao servidor echo...",
            sizeof(network_stage) - 1);
        network_stage[sizeof(network_stage) - 1] = 0;
        screen_update();
        gsKit_sync_flip(gs);
    int fd = ps2_net_connect(NET_TEST_HOST, NET_TEST_PORT, 8000);
    if (fd < 0) {
        tlog("FALHOU: connect em %s:%d (errno=%d)",
             NET_TEST_HOST, NET_TEST_PORT, ps2_net_last_errno);
        tlog("=> server rodando? IP certo no build? firewall?");
        tlog("=> tente tambem -NetTestHost 127.0.0.1");
        verdict = "Fase 1: FALHOU (connect)";
        verdict_color = UI_COLOR(0xFF, 0x60, 0x60);
        goto end;
    }
    tlog("conectado.");
    screen_update();

    int len = (int)sizeof(payload) - 1;
    unsigned char echo_buf[512];
    int failures = 0;

    for (int round = 1; round <= ECHO_ROUNDS; round++) {
        tlog("--- rodada %d: enviando %d bytes", round, len);
        if (ps2_net_send_all(fd, payload, len) != 0) {
            tlog("FALHOU: send (rodada %d)", round);
            failures++;
            break;
        }
        hexdump_row("env", (const unsigned char *)payload, len > 16 ? 16 : len);

        int r = ps2_net_recv_all(fd, echo_buf, len, 8000);
        if (r == -2) {
            tlog("FALHOU: timeout esperando echo (r%d)", round);
            failures++;
            break;
        }
        if (r != 0) {
            tlog("FALHOU: recv (rodada %d)", round);
            failures++;
            break;
        }
        hexdump_row("rec", echo_buf, len > 16 ? 16 : len);

        if (memcmp(payload, echo_buf, len) == 0) {
            tlog("rodada %d: echo exato. OK", round);
        } else {
            tlog("rodada %d: DIVERGENCIA no echo!", round);
            failures++;
        }
        screen_update();
    }

    ps2_net_close(fd);
    ps2_net_deinit();

    if (failures == 0) {
        tlog("=== %d/%d rodadas com echo exato ===", ECHO_ROUNDS, ECHO_ROUNDS);
        verdict = "Fase 1: PASSOU (echo exato)";
        verdict_color = UI_COLOR(0x60, 0xFF, 0x60);
    } else {
        tlog("=== %d falhas ===", failures);
        verdict = "Fase 1: FALHOU";
        verdict_color = UI_COLOR(0xFF, 0x60, 0x60);
    }

end:
    /* Mantém o resultado visível e mostra se os botões são recebidos. */
    for (;;) {
        poll_pad();
        screen_update();
    }

    return 0;
}

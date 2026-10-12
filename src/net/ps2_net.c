/**
 * ps2_net.c -- TCP/IP stack init + socket helpers (ps2ip/lwIP).
 *
 * Modelo: 2004sp-ps2port/src/platform/ps2.c (5a tentativa, que
 * funciona no modo "Sockets" do PCSX2). Nenhuma leitura direta de
 * struct netif -- so a API publica ps2ip_getconfig/ps2ip_setconfig
 * (t_ip_info e estavel entre builds). Nome fixo "sm0", sem scan,
 * sem checar retorno do getconfig antes de setar dhcp_enabled=1.
 *
 * A ordem de init (validada empiricamente pelo RS2 port):
 *   SifInitRpc -> load DEV9/NETMAN/SMAP -> NetManInit -> wait link
 *   -> ps2ipInit(zeros) -> getconfig("sm0") -> set dhcp_enabled=1
 *   -> setconfig -> wait DHCP_STATE_BOUND (com fallback estatico).
 */
#include <kernel.h>
#include <sifrpc.h>
#include <iopcontrol.h>
#include <iopheap.h>
#include <loadfile.h>
#include <sbv_patches.h>
#include <netman.h>
#include <ps2ip.h>
#include <tamtypes.h>
#include <timer.h>
#include <delaythread.h>
#include <lwip/inet.h>
#include <lwip/ip4_addr.h>
#include <lwip/netif.h>
#include <lwip/sockets.h>
#include <errno.h>
#include <string.h>
#include <stdio.h>

/* Keep the socket aliases local: LWIP_POSIX_SOCKETS_IO_NAMES is disabled in PS2BUILD. */
#undef lwip_socket
#undef lwip_connect
#undef lwip_send
#undef lwip_recv
#undef lwip_select
#undef lwip_ioctl
#undef lwip_close
#undef lwip_shutdown
#undef lwip_getsockopt
#undef lwip_setsockopt

#include "ps2_net.h"
#include "net_stream.h"
#include "../platform.h"

Ps2NetDbg ps2_net_dbg;

extern unsigned char dev9_embed_irx[];
extern unsigned int size_dev9_embed_irx;
extern unsigned char netman_embed_irx[];
extern unsigned int size_netman_embed_irx;
extern unsigned char smap_embed_irx[];
extern unsigned int size_smap_embed_irx;

static int net_inited = 0;
static Ps2NetProgressCallback progress_callback;

void ps2_net_set_progress_callback(Ps2NetProgressCallback callback)
{
    progress_callback = callback;
}

static void report_progress(const char *stage)
{
    if (progress_callback)
        progress_callback(stage);
}

/* ---- "sm0" fixo (modelo do RS2 port) ---- */
#define PS2_NETIF_NAME "sm0"

/* ---- alarme para acordar a thread EE (evita busy-spin) ---- */
/* ps2sdk kernel.h: "Alarm value is in H-SYNC ticks" (~64 us por tick),
 * NAO microssegundos. 1600 ticks = ~100 ms: e' o intervalo de POLL do
 * wait_valid_net_state (link/DHCP, progresso a cada ~1 s). Reutilizar
 * esse valor como yield do hot path derrubou o jogo para 7-9 fps
 * (~100 ms por gp_poll()/send). Yield curto usa DelayThread (us). */
#define PS2_NET_POLL_ALARM_TICKS 1600

/* Custo por frame: 1x no gp_poll() + 1x por pacote enviado. 500 us
 * ~ 3% do orcamento de 16.6 ms e basta para as threads do ps2ip/NetMan
 * drenarem a fila SIF. */
#define PS2_NET_YIELD_US 500

static void net_alarm_cb(s32 alarm_id, u16 time, void *common)
{
    (void)alarm_id; (void)time;
    iWakeupThread(*(int *)common);
}

static void ps2_net_yield(void)
{
    DelayThread(PS2_NET_YIELD_US);
}

/* ---- relogio de hardware (timeout confiavel) ---- */
static void ps2_net_timer_init(void)
{
    StartTimerSystemTime();
}

static uint64_t ps2_net_now_ms(void)
{
    u64 clocks = GetTimerSystemTime();
    u32 sec, usec;
    TimerBusClock2USec(clocks, &sec, &usec);
    return (uint64_t)sec * 1000 + usec / 1000;
}

static int wait_valid_net_state(int (*check)(void), int timeout_ms,
                                const char *stage)
{
    uint64_t start = ps2_net_now_ms();
    int thread_id = GetThreadId();
    unsigned int polls = 0;
    while (!check()) {
        SetAlarm(PS2_NET_POLL_ALARM_TICKS, &net_alarm_cb, &thread_id);
        SleepThread();
        if (++polls % 10 == 0) {
            char status[64];
            uint64_t elapsed = ps2_net_now_ms() - start;
            snprintf(status, sizeof(status), "%s %d s", stage,
                     (int)(elapsed / 1000));
            report_progress(status);
        }
        if (ps2_net_now_ms() - start >= (uint64_t)timeout_ms)
            return -1;
    }
    return 0;
}

static int eth_link_up(void)
{
    ps2_net_dbg.link_state =
        NetManIoctl(NETMAN_NETIF_IOCTL_GET_LINK_STATUS, NULL, 0, NULL, 0);
    return ps2_net_dbg.link_state == NETMAN_NETIF_ETH_LINK_STATE_UP;
}

static int eth_dhcp_bound(void)
{
    t_ip_info ip_info;
    /* Modelo do RS2 port: chama getconfig SEM checar o retorno.
     * A convencao de retorno pode ser diferente entre builds do
     * ps2sdk -- o que importa sao os DADOS da struct. */
    ps2ip_getconfig(PS2_NETIF_NAME, &ip_info);
    ps2_net_dbg.dhcp_status = (int)ip_info.dhcp_status;
    ps2_net_dbg.ipaddr = ip_info.ipaddr.s_addr;
    return ip_info.dhcp_status == DHCP_STATE_BOUND &&
           ip_info.ipaddr.s_addr != 0;
}

int ps2_net_init(void)
{
    if (net_inited)
        return 0;

    /* Match the known-working 2004sp-ps2port setup: reset the IOP before
     * loading any network modules, then reinitialize RPC from a clean slate. */
    report_progress("Iniciando RPC / IOP...");
    SifInitRpc(0);
    while (!SifIopReset("", 0)) {
    }
    while (!SifIopSync()) {
    }
    SifInitRpc(0);
    SifLoadFileInit();
    SifInitIopHeap();
    sbv_patch_enable_lmb();
    ps2_net_timer_init();

    /* Network modules (embutidos, via SifExecModuleBuffer) */
    report_progress("Carregando DEV9...");
    ps2_net_dbg.dev9   = SifExecModuleBuffer(dev9_embed_irx, size_dev9_embed_irx, 0, NULL, NULL);
    report_progress("Carregando NETMAN...");
    ps2_net_dbg.netman = SifExecModuleBuffer(netman_embed_irx, size_netman_embed_irx, 0, NULL, NULL);
    report_progress("Carregando SMAP...");
    ps2_net_dbg.smap   = SifExecModuleBuffer(smap_embed_irx, size_smap_embed_irx, 0, NULL, NULL);

    report_progress("Iniciando NetMan...");
    ps2_net_dbg.netman_init_ret = NetManInit();
    rs2_log("ps2_net: NetManInit=%d\n", ps2_net_dbg.netman_init_ret);

    /* 1) esperar LINK antes de qualquer ps2ipInit */
    report_progress("Esperando link Ethernet...");
    if (wait_valid_net_state(eth_link_up, PS2_NET_LINK_TIMEOUT_MS,
                             "Sem link Ethernet; tempo") != 0) {
        report_progress("Timeout: link Ethernet nao subiu");
        return -3;
    }

    /* 2) ps2ipInit com enderecos zerados */
    report_progress("Inicializando TCP/IP...");
    struct ip4_addr ip = {0}, nm = {0}, gw = {0};
    ps2_net_dbg.ps2ip_ret = ps2ipInit(&ip, &nm, &gw);
    if (ps2_net_dbg.ps2ip_ret < 0)
        return -2;

    /* 3) DIAGNOSTICO: ler netif_default em offsets ESTAVEIS
     * (ip_addr@4, netmask@8, gw@12 -- nao dependem de lwipopts) */
    {
        struct netif *nd = netif_default;
        ps2_net_dbg.netif_default_addr = (uint32_t)(uintptr_t)nd;
        if (nd != NULL) {
            uint32_t *w = (uint32_t *)nd;
            ps2_net_dbg.nif_ip      = w[1]; /* ip_addr   @ +4  */
            ps2_net_dbg.nif_netmask = w[2]; /* netmask   @ +8  */
            ps2_net_dbg.nif_gw      = w[3]; /* gw        @ +12 */
            memcpy(ps2_net_dbg.netif_raw, nd, sizeof(ps2_net_dbg.netif_raw));
        } else {
            ps2_net_dbg.nif_ip = 0;
            ps2_net_dbg.nif_netmask = 0;
            ps2_net_dbg.nif_gw = 0;
            memset(ps2_net_dbg.netif_raw, 0, sizeof(ps2_net_dbg.netif_raw));
        }
        rs2_log("ps2_net: ndflt=%p ip=%08x nm=%08x gw=%08x\n",
            nd, (unsigned)ps2_net_dbg.nif_ip,
            (unsigned)ps2_net_dbg.nif_netmask,
            (unsigned)ps2_net_dbg.nif_gw);
    }

    /* 4) DIAGNOSTICO: valor cru do getconfig("sm0") */
    report_progress("Lendo configuracao IP...");
    {
        t_ip_info probe;
        memset(&probe, 0xAA, sizeof(probe));
        int r = ps2ip_getconfig(PS2_NETIF_NAME, &probe);
        rs2_log("ps2_net: getconfig(sm0)=%d status=%d ip=0x%08x dhcp_en=%d\n",
            r, (int)probe.dhcp_status,
            (unsigned int)probe.ipaddr.s_addr,
            (int)probe.dhcp_enabled);
        ps2_net_dbg.getconfig_ret = r;
        ps2_net_dbg.dhcp_status = (int)probe.dhcp_status;
    }

    /* 5) Match the working reference: start with the live interface config,
     * preserving its interface name and other fields, then enable DHCP. */
    if (PS2_NET_USE_DHCP) {
        report_progress("Habilitando DHCP...");
        t_ip_info ip_info;
        memset(&ip_info, 0, sizeof(ip_info));
        ps2_net_dbg.getconfig_ret = ps2ip_getconfig(PS2_NETIF_NAME, &ip_info);
        ip_info.dhcp_enabled = 1;
        int r_set = ps2ip_setconfig(&ip_info);
        ps2_net_dbg.setconfig_ret = r_set;
        rs2_log("ps2_net: getconfig=%d setconfig=%d\n",
                ps2_net_dbg.getconfig_ret, r_set);
    }

    /* 6) Esperar DHCP BOUND (nunca aceitar OFF) */
    report_progress("Esperando endereco DHCP...");
    if (wait_valid_net_state(eth_dhcp_bound, 20000,
                             "DHCP pendente; tempo") != 0) {
        report_progress("Timeout DHCP; usando fallback...");
        rs2_log("ps2_net: DHCP timed out (status=%d)\n",
            ps2_net_dbg.dhcp_status);
        if (!PS2_NET_USE_DHCP)
            return -4;

        /* Fallback estatico via ps2ip_setconfig (API publica) */
        t_ip_info ip_info = {0};
        ps2ip_getconfig(PS2_NETIF_NAME, &ip_info);
        ip_info.dhcp_enabled = 0;
        ip_info.ipaddr.s_addr  = inet_addr(PS2_NET_FALLBACK_IP);
        ip_info.netmask.s_addr = inet_addr(PS2_NET_FALLBACK_NETMASK);
        ip_info.gw.s_addr      = inet_addr(PS2_NET_FALLBACK_GATEWAY);
        ps2ip_setconfig(&ip_info);
        ps2_net_dbg.static_fallback = 1;
        ps2_net_dbg.ipaddr = ip_info.ipaddr.s_addr;
        rs2_log("ps2_net: fallback estatico %s\n", PS2_NET_FALLBACK_IP);
    }

    /* 7) Gateway ausente: heuristica do ".1" via setconfig */
    {
        t_ip_info ip_info = {0};
        ps2ip_getconfig(PS2_NETIF_NAME, &ip_info);
        if (ip_info.ipaddr.s_addr != 0 && ip_info.gw.s_addr == 0) {
            uint32_t nip = ip_info.ipaddr.s_addr;
            uint32_t nmask = ip_info.netmask.s_addr;
            if (nmask == 0) nmask = 0x00FFFFFF;
            ip_info.gw.s_addr = (nip & nmask) | 0x01000000;
            ps2ip_setconfig(&ip_info);
        }
    }

    report_progress(ps2_net_dbg.static_fallback
                    ? "DHCP falhou; IP fixo fallback ativo"
                    : "DHCP concluido; rede pronta");
    net_inited = 1;
    return 0;
}

void ps2_net_deinit(void)
{
    if (!net_inited) return;
    ps2ipDeinit();
    NetManDeinit();
    net_inited = 0;
}

int ps2_net_get_ip(char *out, size_t out_len)
{
    t_ip_info ip_info;
    ps2ip_getconfig(PS2_NETIF_NAME, &ip_info);
    if (ip_info.ipaddr.s_addr == 0 && netif_default != NULL) {
        uint32_t *w = (uint32_t *)netif_default;
        ip_info.ipaddr.s_addr = w[1];
    }
    struct in_addr in;
    in.s_addr = ip_info.ipaddr.s_addr;
    strncpy(out, inet_ntoa(in), out_len - 1);
    out[out_len - 1] = 0;
    return 0;
}

int ps2_net_get_gateway(char *out, size_t out_len)
{
    t_ip_info ip_info;
    ps2ip_getconfig(PS2_NETIF_NAME, &ip_info);
    if (ip_info.gw.s_addr == 0 && netif_default != NULL) {
        uint32_t *w = (uint32_t *)netif_default;
        ip_info.gw.s_addr = w[3];
    }
    struct in_addr in;
    in.s_addr = ip_info.gw.s_addr;
    strncpy(out, inet_ntoa(in), out_len - 1);
    out[out_len - 1] = 0;
    return 0;
}

int ps2_net_get_netmask(char *out, size_t out_len)
{
    t_ip_info ip_info;
    ps2ip_getconfig(PS2_NETIF_NAME, &ip_info);
    if (ip_info.netmask.s_addr == 0 && netif_default != NULL) {
        uint32_t *w = (uint32_t *)netif_default;
        ip_info.netmask.s_addr = w[2];
    }
    struct in_addr in;
    in.s_addr = ip_info.netmask.s_addr;
    strncpy(out, inet_ntoa(in), out_len - 1);
    out[out_len - 1] = 0;
    return 0;
}

int ps2_net_last_errno = 0;

int ps2_net_connect(const char *ip, uint16_t port, int timeout_ms)
{
    struct sockaddr_in addr;
    int fd, err;

    fd = lwip_socket(AF_INET, SOCK_STREAM, IPPROTO_TCP);
    if (fd < 0) { ps2_net_last_errno = errno; return -1; }

    memset(&addr, 0, sizeof(addr));
    addr.sin_family = AF_INET;
    addr.sin_port = htons(port);
    addr.sin_addr.s_addr = inet_addr(ip);
    if (addr.sin_addr.s_addr == INADDR_NONE) {
        lwip_close(fd);
        ps2_net_last_errno = -1;
        return -1;
    }

    /* Bound connect time so an unavailable echo server cannot look like a hang. */
    unsigned long nonblocking = 1;
    if (lwip_ioctl(fd, FIONBIO, &nonblocking) < 0) {
        ps2_net_last_errno = errno;
        lwip_close(fd);
        return -1;
    }

    err = lwip_connect(fd, (struct sockaddr *)&addr, sizeof(addr));
    if (err != 0 && errno == EINPROGRESS) {
        fd_set wfds;
        struct timeval tv;
        FD_ZERO(&wfds);
        FD_SET(fd, &wfds);
        tv.tv_sec = timeout_ms / 1000;
        tv.tv_usec = (timeout_ms % 1000) * 1000;
        err = lwip_select(fd + 1, NULL, &wfds, NULL, &tv);
        if (err > 0) {
            int socket_error = 0;
            socklen_t optlen = sizeof(socket_error);
            if (lwip_getsockopt(fd, SOL_SOCKET, SO_ERROR,
                                &socket_error, &optlen) != 0) {
                err = -1;
            } else {
                err = socket_error == 0 ? 0 : -1;
                if (socket_error != 0)
                    errno = socket_error;
            }
        } else if (err == 0) {
            errno = ETIMEDOUT;
            err = -1;
        }
    }

    if (err != 0) {
        ps2_net_last_errno = errno;
        rs2_log("ps2_net: connect(%s:%u) failed, errno=%d\n", ip, port,
                ps2_net_last_errno);
        lwip_close(fd);
        return -1;
    }

    nonblocking = 0;
    lwip_ioctl(fd, FIONBIO, &nonblocking);
    {
        int one = 1;
        if (lwip_setsockopt(fd, IPPROTO_TCP, TCP_NODELAY,
                            &one, sizeof(one)) != 0) {
            rs2_log("ps2_net: TCP_NODELAY failed, errno=%d\n", errno);
        }
    }
    ps2_net_last_errno = 0;
    return fd;
}

int ps2_net_send_all(int fd, const void *buf, size_t len)
{
    const char *p = buf;
    size_t left = len;
    while (left > 0) {
        int n = lwip_send(fd, p, left, 0);
        if (n <= 0) return -1;
        p += n;
        left -= (size_t)n;
    }
    ps2_net_yield();
    return 0;
}

static int wait_readable(int fd, int timeout_ms)
{
    fd_set rfds;
    struct timeval tv;
    FD_ZERO(&rfds);
    FD_SET(fd, &rfds);
    tv.tv_sec = timeout_ms / 1000;
    tv.tv_usec = (timeout_ms % 1000) * 1000;
    return lwip_select(fd + 1, &rfds, NULL, NULL, &tv);
}

int ps2_net_recv_all(int fd, void *buf, size_t len, int timeout_ms)
{
    char *p = buf;
    size_t left = len;
    while (left > 0) {
        int r = wait_readable(fd, timeout_ms);
        if (r == 0) return -2;
        if (r < 0) return -1;
        int n = lwip_recv(fd, p, left, 0);
        if (n == 0) return -1;
        if (n < 0) return -1;
        p += n;
        left -= (size_t)n;
    }
    return 0;
}

int ps2_net_recv_some(int fd, void *buf, size_t len, int timeout_ms)
{
    int r = wait_readable(fd, timeout_ms);
    if (r == 0) return -2;
    if (r < 0) return -1;
    int n = lwip_recv(fd, buf, len, 0);
    if (n == 0) return -1;
    return n;
}

void ps2_net_close(int fd)
{
    lwip_shutdown(fd, SHUT_RDWR);
    lwip_close(fd);
}

/* ---- NetStreamVt ---- */
static int pnet_connect(const char *ip, uint16_t port, int t)
    { return ps2_net_connect(ip, port, t); }
static int pnet_send_all(int fd, const void *buf, size_t len)
    { return ps2_net_send_all(fd, buf, len); }
static int pnet_recv_all(int fd, void *buf, size_t len, int t)
    { return ps2_net_recv_all(fd, buf, len, t); }
static int pnet_recv_some(int fd, void *buf, size_t len, int t)
    { return ps2_net_recv_some(fd, buf, len, t); }
static void pnet_close(int fd)
    { ps2_net_close(fd); }
static void pnet_yield(void)
    { ps2_net_yield(); }

const NetStreamVt *ps2_net_stream(void)
{
    static const NetStreamVt vt = {
        pnet_connect, pnet_send_all, pnet_recv_all, pnet_recv_some,
        pnet_close, pnet_yield,
    };
    return &vt;
}

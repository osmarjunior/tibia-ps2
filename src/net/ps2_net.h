/**
 * ps2_net.h — TCP/IP stack init + socket helpers for the PS2 (ps2ip/lwIP).
 *
 * Fase 1 do TASKS2.md: "o cano abre e passa dado".
 *
 * Modules (embedded via bin2c at build time, from $PS2SDK/iop/irx):
 *   ps2dev9.irx + netman.irx + smap.irx  (Network Adaptor / PCSX2 dev9)
 * IP config: DHCP by default; static via PS2_NET_STATIC_IP/... defines.
 */
#ifndef PS2_NET_H
#define PS2_NET_H

#include <stddef.h>
#include <stdint.h>

#include "net_stream.h"

/* Config (override with -D at build time) */
#ifndef PS2_NET_USE_DHCP
#define PS2_NET_USE_DHCP 1
#endif
#ifndef PS2_NET_STATIC_IP
#define PS2_NET_STATIC_IP "192.168.0.100"
#endif
#ifndef PS2_NET_STATIC_NETMASK
#define PS2_NET_STATIC_NETMASK "255.255.255.0"
#endif
#ifndef PS2_NET_STATIC_GATEWAY
#define PS2_NET_STATIC_GATEWAY "192.168.0.1"
#endif
#ifndef PS2_NET_LINK_TIMEOUT_MS
#define PS2_NET_LINK_TIMEOUT_MS 15000
#endif

/* Init the network stack. Returns 0 on success (link up, IP configured). */
int ps2_net_init(void);

/* Optional startup progress hook; invoked synchronously on the EE thread. */
typedef void (*Ps2NetProgressCallback)(const char *stage);
void ps2_net_set_progress_callback(Ps2NetProgressCallback callback);

/* Diagnóstico passo a passo do init (exibido na tela pelo net_test). */
typedef struct {
    int dev9, netman, smap;   /* retorno de SifExecModuleBuffer */
    int netman_init_ret;      /* NetManInit */
    int link_state;           /* último NetManIoctl(GET_LINK_STATUS) */
    int ps2ip_ret;            /* ps2ipInit */
    int getconfig_ret;        /* valor cru de ps2ip_getconfig("sm0") */
    int setconfig_ret;        /* valor cru de ps2ip_setconfig */
    int dhcp_status;          /* último DHCP_STATE_* visto */
    uint32_t ipaddr;          /* último IP visto (network order) */
    int static_fallback;      /* 1 = DHCP falhou, usando IP estático */
    /* diagnóstico direto de netif_default (offsets ESTÁVEIS) */
    uint32_t netif_default_addr; /* endereço do ponteiro (0 = NULL) */
    uint32_t nif_ip;             /* ip_addr.addr @ offset 4 */
    uint32_t nif_netmask;        /* netmask.addr @ offset 8 */
    uint32_t nif_gw;             /* gw.addr @ offset 12 */
    /* hexdump cru */
    uint8_t netif_raw[56];
} Ps2NetDbg;
extern Ps2NetDbg ps2_net_dbg;

/* Fallback estático quando o DHCP não responde (tipicamente emuladores
 * sem servidor DHCP interno configurável). Sub-rede "padrão de LAN". */
#ifndef PS2_NET_FALLBACK_IP
#define PS2_NET_FALLBACK_IP "192.168.100.73"
#endif
#ifndef PS2_NET_FALLBACK_NETMASK
#define PS2_NET_FALLBACK_NETMASK "255.255.255.0"
#endif
#ifndef PS2_NET_FALLBACK_GATEWAY
#define PS2_NET_FALLBACK_GATEWAY "192.168.100.1"
#endif

/* NetStreamVt sobre lwIP (registre com net_set_stream antes do login). */
const NetStreamVt *ps2_net_stream(void);

/* Gateway atual ("sm0") em dotted quad — diagnóstico. */
int ps2_net_get_gateway(char *out, size_t out_len);

/* Netmask atual ("sm0") em dotted quad — diagnóstico. */
int ps2_net_get_netmask(char *out, size_t out_len);

/* errno da última falha de ps2_net_connect (diagnóstico na tela). */
extern int ps2_net_last_errno;

/* Deinit (NetManDeinit + ps2ipDeinit). */
void ps2_net_deinit(void);

/* Write the current IP of "sm0" as dotted quad into out. 0 on success. */
int ps2_net_get_ip(char *out, size_t out_len);

/* TCP connect with timeout. Returns socket fd >= 0, or -1 on error. */
int ps2_net_connect(const char *ip, uint16_t port, int timeout_ms);

/* Send exactly len bytes. 0 on success, -1 on error. */
int ps2_net_send_all(int fd, const void *buf, size_t len);

/* Receive exactly len bytes. 0 on success, -1 on error/EOF, -2 on timeout. */
int ps2_net_recv_all(int fd, void *buf, size_t len, int timeout_ms);

/* Receive whatever is available (up to len), >=0 bytes read, -1 error, -2 timeout. */
int ps2_net_recv_some(int fd, void *buf, size_t len, int timeout_ms);

void ps2_net_close(int fd);

#endif /* PS2_NET_H */

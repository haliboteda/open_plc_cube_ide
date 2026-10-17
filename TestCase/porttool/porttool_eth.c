// porttool_eth.c
//
// Ethernet session: a TCP server on the RJ45. Starting it is what brings lwIP
// up (eth_lwip_up). eth.link, on the same caps row, reads the PHY over MDIO
// only and needs no stack (DECISIONS.md 28).
//
// Parameters:
//   mode=echo    echo | sink | source | mirror | file   (default echo)
//   port=5000    TCP port to listen on           (default 5000)
//   ip=dhcp      dhcp, or a static address       (default dhcp)
//   period=1000  milliseconds between frames     (default 1000, floor 50)
//
// Modes:
//
//   echo    the board sends its counter, the peer sends it straight back: the
//           only mode where seq is the verdict.
//   sink    the peer pushes at the board, which counts bytes and the rate.
//   source  the board pushes as fast as the window allows.
//   mirror  the board sends back every byte unchanged; the PC floods numbered
//           packets and measures rate, loss and latency in one run
//           (DECISIONS.md 98, PORTTOOL-FLOW.md B.9).
//   file    the link carries an SD file transfer (porttool_sdfile.c,
//           PORTTOOL-FLOW.md B.11): bytes in go to the card, the card's
//           bytes go out. No counter is sent - it would land in the file.
//
// Frame:
//   !eth t=48213 seq=1 rx=1 miss=0 ip=192.168.1.50 link=1 conn=1 mode=echo
//        port=5000 rx_bytes=0 tx_bytes=0 kbps=0 drop=0
//
// drop= is bytes mirror mode could not send back; it stays 0, since unsent
// bytes are held unacknowledged and the peer's window stops first.
//
// rx_bytes/tx_bytes are totals since start; kbps is the last period only.
//
// A static ip= assumes a /24 with the gateway at .1 (a test bench without DHCP).

#include "porttool.h"
#include "porttool_cmd.h"

#include "main.h"
#include "lwip.h"
#include "lwip/tcp.h"
#include "lwip/netif.h"
#include "lwip/dhcp.h"

#include <stdio.h>
#include <string.h>

#if PORTTOOL_ENABLE

#define ETH_PORT_DEF      5000U
#define ETH_PERIOD_DEF_MS 1000U

/* One client at a time, so a second peer cannot mix into the measurement. */
static struct tcp_pcb *eth_listen_pcb;
static struct tcp_pcb *eth_client_pcb;

typedef enum {
    ETH_MODE_ECHO = 0,
    ETH_MODE_SINK,
    ETH_MODE_SOURCE,
    ETH_MODE_MIRROR,
    ETH_MODE_FILE,
} eth_mode_t;

static eth_mode_t eth_mode = ETH_MODE_ECHO;
static uint32_t   eth_port = ETH_PORT_DEF;
static uint32_t   eth_period_ms = ETH_PERIOD_DEF_MS;
static uint32_t   eth_due_ms;

/* dhcp, or the static address the operator asked for. */
static int      eth_static;
static ip4_addr_t eth_static_addr;

static int eth_lwip_started;    /* MX_LWIP_Init has run - it may only run once */

static uint32_t eth_rx_bytes;
static uint32_t eth_tx_bytes;
static uint32_t eth_window_bytes;  /* bytes moved since the last frame */
static uint32_t eth_drop;

/* Mirror backlog. Bytes held here are not yet tcp_recved, so the advertised
 * window never lets the peer send more than this holds. */
static uint8_t  eth_mirror_buf[TCP_WND];
static uint32_t eth_mirror_len;

static porttool_echo_t eth_echo;

/* What source mode pushes; only the rate matters. */
static const char eth_filler[256] = { 0 };

static const char *eth_mode_name(eth_mode_t m)
{
    switch (m) {
    case ETH_MODE_SINK:   return "sink";
    case ETH_MODE_SOURCE: return "source";
    case ETH_MODE_MIRROR: return "mirror";
    case ETH_MODE_FILE:   return "file";
    default:              return "echo";
    }
}

static void eth_take_echo(const uint8_t *data, uint16_t len)
{
    static porttool_numscan_t scan;

    PortTool_EchoScan(&eth_echo, &scan, data, len);
}

static void eth_close_client(struct tcp_pcb *pcb)
{
    if (pcb == NULL) {
        return;
    }
    tcp_arg(pcb, NULL);
    tcp_recv(pcb, NULL);
    tcp_err(pcb, NULL);
    tcp_sent(pcb, NULL);
    tcp_close(pcb);
    if (pcb == eth_client_pcb) {
        eth_client_pcb = NULL;
        eth_mirror_len = 0;
    }
}

/* Sends back as much of the backlog as the send buffer takes, and only then
 * opens the receive window by that much. */
static void eth_mirror_drain(void)
{
    u16_t room;
    u16_t n;

    if ((eth_client_pcb == NULL) || (eth_mirror_len == 0U)) {
        return;
    }
    room = tcp_sndbuf(eth_client_pcb);
    n = (eth_mirror_len < room) ? (u16_t)eth_mirror_len : room;
    if (n == 0U) {
        return;
    }
    if (tcp_write(eth_client_pcb, eth_mirror_buf, n, TCP_WRITE_FLAG_COPY) != ERR_OK) {
        return;
    }
    tcp_output(eth_client_pcb);
    tcp_recved(eth_client_pcb, n);
    eth_tx_bytes += n;
    eth_mirror_len -= n;
    memmove(eth_mirror_buf, eth_mirror_buf + n, eth_mirror_len);
}

static err_t eth_recv_cb(void *arg, struct tcp_pcb *pcb, struct pbuf *p, err_t err)
{
    (void)arg;

    if ((err != ERR_OK) || (p == NULL)) {
        eth_close_client(pcb);
        return ERR_OK;
    }

    if (pcb != eth_client_pcb) {
        tcp_recved(pcb, p->tot_len);
        pbuf_free(p);
        return ERR_OK;
    }

    eth_rx_bytes     += p->tot_len;
    eth_window_bytes += p->tot_len;

    /* Only echo parses the payload; sink only counts, to measure the rate. */
    if (eth_mode == ETH_MODE_ECHO) {
        for (struct pbuf *q = p; q != NULL; q = q->next) {
            eth_take_echo((const uint8_t *)q->payload, q->len);
        }
    }

    if (eth_mode == ETH_MODE_FILE) {
        for (struct pbuf *q = p; q != NULL; q = q->next) {
            PortSdFile_Rx((const uint8_t *)q->payload, q->len);
        }
    }

    if (eth_mode == ETH_MODE_MIRROR) {
        uint32_t room = (uint32_t)sizeof(eth_mirror_buf) - eth_mirror_len;
        uint32_t take = (p->tot_len < room) ? p->tot_len : room;

        pbuf_copy_partial(p, eth_mirror_buf + eth_mirror_len, (u16_t)take, 0);
        eth_mirror_len += take;
        if (take < p->tot_len) {
            /* Only a peer ignoring the advertised window gets here. */
            eth_drop += p->tot_len - take;
            tcp_recved(pcb, (u16_t)(p->tot_len - take));
        }
        pbuf_free(p);
        eth_mirror_drain();
        return ERR_OK;
    }

    tcp_recved(pcb, p->tot_len);
    pbuf_free(p);
    return ERR_OK;
}

static void eth_err_cb(void *arg, err_t err)
{
    (void)err;
    /* lwIP has already freed the pcb; arg says which one it was. */
    if (arg == (void *)eth_client_pcb) {
        eth_client_pcb = NULL;
        eth_mirror_len = 0;
    }
}

static err_t eth_accept_cb(void *arg, struct tcp_pcb *pcb, err_t err)
{
    (void)arg;

    if ((err != ERR_OK) || (pcb == NULL)) {
        return ERR_VAL;
    }
    if (eth_client_pcb != NULL) {
        return ERR_MEM;     /* lwIP aborts the refused pcb for us */
    }

    eth_client_pcb = pcb;
    eth_mirror_len = 0;
    tcp_arg(pcb, pcb);
    tcp_recv(pcb, eth_recv_cb);
    tcp_err(pcb, eth_err_cb);
    return ERR_OK;
}

/* Brings the stack up once, then applies the addressing. A static address is
 * set after MX_LWIP_Init (which starts DHCP), so the CubeMX code stays as is. */
static int eth_lwip_up(char *err, uint32_t err_len)
{
    if (!eth_lwip_started) {
        MX_LWIP_Init();
        eth_lwip_started = 1;
    }

    if (netif_default == NULL) {
        snprintf(err, err_len, "lwIP came up with no default interface");
        return 0;
    }

    if (eth_static) {
        ip4_addr_t mask, gw;

        dhcp_stop(netif_default);
        IP4_ADDR(&mask, 255, 255, 255, 0);
        gw = eth_static_addr;
        ip4_addr_set_u32(&gw, (ip4_addr_get_u32(&eth_static_addr) & PP_HTONL(0xFFFFFF00UL))
                              | PP_HTONL(1UL));
        netif_set_addr(netif_default, &eth_static_addr, &mask, &gw);
    } else if (dhcp_supplied_address(netif_default) == 0) {
        /* Restart negotiation: a previous static setting stopped the client. */
        dhcp_start(netif_default);
    }

    return 1;
}

static int eth_listen(char *err, uint32_t err_len)
{
    struct tcp_pcb *pcb;

    if (eth_listen_pcb != NULL) {
        tcp_close(eth_listen_pcb);
        eth_listen_pcb = NULL;
    }

    pcb = tcp_new();
    if (pcb == NULL) {
        snprintf(err, err_len, "lwIP had no pcb left to listen with");
        return 0;
    }
    if (tcp_bind(pcb, IP_ADDR_ANY, (u16_t)eth_port) != ERR_OK) {
        tcp_abort(pcb);
        snprintf(err, err_len, "port %lu is already bound", (unsigned long)eth_port);
        return 0;
    }
    pcb = tcp_listen(pcb);
    if (pcb == NULL) {
        snprintf(err, err_len, "lwIP would not listen on port %lu",
                 (unsigned long)eth_port);
        return 0;
    }
    tcp_accept(pcb, eth_accept_cb);
    eth_listen_pcb = pcb;
    return 1;
}

static int eth_apply(const char *args, char *err, uint32_t err_len)
{
    char probe[64];
    uint32_t v;

    eth_mode_t want_mode   = eth_mode;
    uint32_t   want_port   = eth_port;
    uint32_t   want_period = eth_period_ms;
    int        want_static = eth_static;
    ip4_addr_t want_addr   = eth_static_addr;

    if (PortCmd_GetStr(args, "mode", probe, sizeof(probe))) {
        if      (strcmp(probe, "echo")   == 0) { want_mode = ETH_MODE_ECHO; }
        else if (strcmp(probe, "sink")   == 0) { want_mode = ETH_MODE_SINK; }
        else if (strcmp(probe, "source") == 0) { want_mode = ETH_MODE_SOURCE; }
        else if (strcmp(probe, "mirror") == 0) { want_mode = ETH_MODE_MIRROR; }
        else if (strcmp(probe, "file")   == 0) { want_mode = ETH_MODE_FILE; }
        else {
            snprintf(err, err_len, "mode=\"%s\" must be echo, sink, source, mirror or file", probe);
            return 0;
        }
    }

    if (PortCmd_GetStr(args, "port", probe, sizeof(probe))) {
        if (!PortCmd_GetU32(args, "port", &v) || v == 0U || v > 65535U) {
            snprintf(err, err_len, "port=\"%s\" must be 1..65535", probe);
            return 0;
        }
        want_port = v;
    }

    if (PortCmd_GetStr(args, "ip", probe, sizeof(probe))) {
        if (strcmp(probe, "dhcp") == 0) {
            want_static = 0;
        } else if (ip4addr_aton(probe, &want_addr)) {
            want_static = 1;
        } else {
            snprintf(err, err_len,
                     "ip=\"%s\" must be dhcp or a dotted address like 192.168.1.50",
                     probe);
            return 0;
        }
    }

    if (PortCmd_GetStr(args, "period", probe, sizeof(probe))) {
        if (!PortCmd_GetU32(args, "period", &v)) {
            snprintf(err, err_len, "period=\"%s\" is not a number", probe);
            return 0;
        }
        want_period = PortTool_ClampPeriod(v);
    }

    eth_mode        = want_mode;
    eth_port        = want_port;
    eth_period_ms   = want_period;
    eth_static      = want_static;
    eth_static_addr = want_addr;
    return 1;
}

static int eth_start(const char *args, char *err, uint32_t err_len)
{
    if (!eth_apply(args, err, err_len)) {
        return 0;
    }
    if (!eth_lwip_up(err, err_len)) {
        return 0;
    }
    if (!eth_listen(err, err_len)) {
        return 0;
    }

    PortTool_EchoReset(&eth_echo);
    eth_rx_bytes     = 0;
    eth_tx_bytes     = 0;
    eth_window_bytes = 0;
    eth_drop         = 0;
    eth_mirror_len   = 0;
    eth_due_ms       = HAL_GetTick();
    return 1;
}

static int eth_set(const char *args, char *err, uint32_t err_len)
{
    uint32_t was_port = eth_port;

    if (!eth_apply(args, err, err_len)) {
        return 0;
    }
    /* A changed address or port is acted on, not just recorded. */
    if (!eth_lwip_up(err, err_len)) {
        return 0;
    }
    if ((eth_port != was_port) && !eth_listen(err, err_len)) {
        return 0;
    }
    return 1;
}

/* Aborts rather than closes: SO_REUSE is off, and TIME_WAIT would hold the
 * listen port against the next pt.start eth. */
static void eth_drop_client(void)
{
    struct tcp_pcb *pcb = eth_client_pcb;

    if (pcb == NULL) {
        return;
    }
    eth_client_pcb = NULL;
    eth_mirror_len = 0;
    tcp_arg(pcb, NULL);
    tcp_recv(pcb, NULL);
    tcp_err(pcb, NULL);
    tcp_sent(pcb, NULL);
    tcp_abort(pcb);
}

static void eth_stop(void)
{
    if (eth_mode == ETH_MODE_FILE) {
        PortSdFile_Abort("stopped");
    }
    eth_drop_client();
    if (eth_listen_pcb != NULL) {
        tcp_close(eth_listen_pcb);
        eth_listen_pcb = NULL;
    }
    /* lwIP itself is left up: nothing else contends for it. */
}

/* The SD transfer's sender: all of it into the send window, or none. */
static uint32_t eth_file_send(const uint8_t *data, uint32_t len)
{
    if ((eth_client_pcb == NULL) || (len > 0xFFFFU) || (tcp_sndbuf(eth_client_pcb) < len)) {
        return 0;
    }
    if (tcp_write(eth_client_pcb, data, (u16_t)len, TCP_WRITE_FLAG_COPY) != ERR_OK) {
        return 0;
    }
    tcp_output(eth_client_pcb);
    eth_tx_bytes     += len;
    eth_window_bytes += len;
    return len;
}

/* Pushes as much as the send window holds. Called only in source mode. */
static void eth_pump(void)
{
    if (eth_client_pcb == NULL) {
        return;
    }

    for (;;) {
        u16_t room = tcp_sndbuf(eth_client_pcb);
        u16_t n = (room > sizeof(eth_filler)) ? (u16_t)sizeof(eth_filler) : room;

        if (n == 0U) {
            break;
        }
        if (tcp_write(eth_client_pcb, eth_filler, n, TCP_WRITE_FLAG_COPY) != ERR_OK) {
            break;
        }
        eth_tx_bytes     += n;
        eth_window_bytes += n;
    }
    tcp_output(eth_client_pcb);
}

static void eth_tick(uint32_t now_ms)
{
    char body[PORTTOOL_REPLY_MAX];
    char line[32];
    uint32_t n;
    uint32_t kbps;
    int link;

    if (eth_mode == ETH_MODE_SOURCE) {
        eth_pump();
    }
    if (eth_mode == ETH_MODE_MIRROR) {
        eth_mirror_drain();
    }
    if (eth_mode == ETH_MODE_FILE) {
        PortSdFile_Pump(eth_file_send, now_ms);
    }

    if ((int32_t)(now_ms - eth_due_ms) < 0) {
        return;
    }
    eth_due_ms = now_ms + eth_period_ms;

    /* Only echo gives the peer a number to answer, so only echo counts misses. */
    if (eth_mode == ETH_MODE_ECHO) {
        PortTool_EchoTick(&eth_echo);
    }

    /* Sent on the link under test; the peer returns it unchanged
     * (DECISIONS.md 18). */
    if ((eth_mode == ETH_MODE_ECHO) && (eth_client_pcb != NULL)) {
        n = (uint32_t)snprintf(line, sizeof(line), "%lu\n",
                               (unsigned long)eth_echo.seq);
        if (tcp_write(eth_client_pcb, line, (u16_t)n, TCP_WRITE_FLAG_COPY) == ERR_OK) {
            tcp_output(eth_client_pcb);
            eth_tx_bytes += n;
        }
    }

    /* Rate over the period just ended, in kilobits per second. */
    kbps = (eth_period_ms > 0U)
         ? ((eth_window_bytes * 8U) / eth_period_ms)
         : 0U;
    eth_window_bytes = 0;

    link = (netif_default != NULL) && netif_is_link_up(netif_default);

    n = PortTool_EchoFields(&eth_echo, body, sizeof(body));
    snprintf(body + n, sizeof(body) - n,
             " ip=%s link=%d conn=%d mode=%s port=%lu"
             " rx_bytes=%lu tx_bytes=%lu kbps=%lu drop=%lu",
             (netif_default != NULL) ? ip4addr_ntoa(netif_ip4_addr(netif_default))
                                     : "0.0.0.0",
             link,
             (eth_client_pcb != NULL),
             eth_mode_name(eth_mode),
             (unsigned long)eth_port,
             (unsigned long)eth_rx_bytes,
             (unsigned long)eth_tx_bytes,
             (unsigned long)kbps,
             (unsigned long)eth_drop);

    PortTool_Frame("eth", "%s", body);
}

static void eth_caps(char *out, uint32_t out_len)
{
    char addr[20];

    if (eth_static) {
        snprintf(addr, sizeof(addr), "%s", ip4addr_ntoa(&eth_static_addr));
    } else {
        snprintf(addr, sizeof(addr), "dhcp");
    }

    snprintf(out, out_len, "mode=%s port=%lu ip=%s period=%lu",
             eth_mode_name(eth_mode), (unsigned long)eth_port, addr,
             (unsigned long)eth_period_ms);
}

static void eth_limits(char *out, uint32_t out_len)
{
    /* ip= is left out: "dhcp or a dotted address" fits no spec shape. */
    snprintf(out, out_len, "mode:echo|sink|source|mirror|file port:1..65535 period:%lu..",
             (unsigned long)PORTTOOL_PERIOD_MIN_MS);
}

/* Serviced whether or not a session runs, so DHCP and ARP keep the address. */
void PortEth_Poll(void)
{
    if (eth_lwip_started) {
        MX_LWIP_Process();
    }
}

porttool_port_t porttool_eth = {
    .name     = "eth",
    .board    = PORTTOOL_BOARD_BRIDGE,
    .blk      = "-",
    .term     = "J1",
    .terms    = NULL,
    .params   = "mode,port,ip,period",
    .loop     = PORTTOOL_LOOP_LINK,
    .channels = 1,
    .start    = eth_start,
    .set      = eth_set,
    .stop     = eth_stop,
    .tick     = eth_tick,
    /* No echo hook: the reply comes back over the link (DECISIONS.md 9). */
    .echo     = NULL,
    .caps     = eth_caps,
    .limits   = eth_limits,
};

#endif /* PORTTOOL_ENABLE */

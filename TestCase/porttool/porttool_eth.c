// porttool_eth.c
//
// Ethernet session: a TCP server on the RJ45, for watching a link carry real
// traffic rather than only asking the PHY whether a cable is in.
//
// *** This is the one session that needs a network stack. Everything else in
// *** the tool talks to a peripheral directly, but a TCP server needs lwIP, so
// *** starting this session is what brings lwIP up - see eth_lwip_up(). An
// *** image whose operator never starts it never runs a line of the stack.
// ***
// *** eth.link stays a pt.run target on this same caps row (DECISIONS.md 28).
// *** It reads the PHY over MDIO only and needs none of this, which is why it
// *** still answers on a bench with no cable, no peer and no DHCP server.
//
// Parameters:
//   mode=echo    echo | sink | source            (default echo)
//   port=5000    TCP port to listen on           (default 5000)
//   ip=dhcp      dhcp, or a static address       (default dhcp)
//   period=1000  milliseconds between frames     (default 1000, floor 50)
//
// The three modes answer three different questions:
//
//   echo    the board sends its counter, the peer sends it straight back.
//           Proves the link carries traffic both ways. This is the loop=link
//           counter and the only mode where seq is the verdict.
//   sink    the peer pushes a large file at the board, which counts bytes and
//           reports the rate. Proves sustained receive throughput.
//   source  the board pushes as fast as the window allows and counts what it
//           handed to the stack. Proves sustained transmit throughput.
//
// Frame:
//   !eth t=48213 seq=1 rx=1 miss=0 ip=192.168.1.50 link=1 conn=1 mode=echo
//        port=5000 rx_bytes=0 tx_bytes=0 kbps=0
//
// *** rx_bytes and tx_bytes are totals since the session started; kbps is over
// *** the last period only. Both are there because a transfer that stalls
// *** halfway shows as a total that stopped climbing while the rate went to
// *** zero, which a single number could not distinguish from a slow link.
//
// *** A static ip= assumes a /24 with the gateway at .1. That is the shape of
// *** a test bench, not a general configuration: a production station without
// *** a DHCP server needs an address, and a board that never gets one turns
// *** the whole port into "could not test" rather than "failed".

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

/* One session at a time, like the IAP server's: a second peer would otherwise
 * mix its bytes into the transfer being measured. */
static struct tcp_pcb *eth_listen_pcb;
static struct tcp_pcb *eth_client_pcb;

typedef enum {
    ETH_MODE_ECHO = 0,
    ETH_MODE_SINK,
    ETH_MODE_SOURCE,
} eth_mode_t;

static eth_mode_t eth_mode = ETH_MODE_ECHO;
static uint32_t   eth_port = ETH_PORT_DEF;
static uint32_t   eth_period_ms = ETH_PERIOD_DEF_MS;
static uint32_t   eth_due_ms;

/* dhcp, or the static address the operator asked for. */
static int      eth_static;
static ip4_addr_t eth_static_addr;

static int eth_lwip_started;    /* MX_LWIP_Init has run - it may only run once */
static int eth_running;

static uint32_t eth_rx_bytes;
static uint32_t eth_tx_bytes;
static uint32_t eth_window_bytes;  /* bytes moved since the last frame */

static porttool_echo_t eth_echo;

/* What source mode pushes. Content is irrelevant - only the rate is - so it is
 * a fixed pattern rather than anything the peer has to check. */
static const char eth_filler[256] = { 0 };

static const char *eth_mode_name(eth_mode_t m)
{
    switch (m) {
    case ETH_MODE_SINK:   return "sink";
    case ETH_MODE_SOURCE: return "source";
    default:              return "echo";
    }
}

/* Reads back the number the peer echoed. Only whole decimal runs count; any
 * other byte just separates them, so a peer that adds a newline or a space is
 * accepted and one that sends garbage is ignored rather than miscounted. */
static void eth_take_echo(const uint8_t *data, uint16_t len)
{
    static uint32_t acc;
    static int      in_number;

    for (uint16_t i = 0; i < len; i++) {
        uint8_t c = data[i];
        if ((c >= '0') && (c <= '9')) {
            acc = (acc * 10U) + (uint32_t)(c - '0');
            in_number = 1;
        } else if (in_number) {
            PortTool_EchoGot(&eth_echo, acc);
            acc = 0;
            in_number = 0;
        }
    }
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
    }
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

    /* Only echo mode reads the payload. sink deliberately does not look at it:
     * the question there is how fast bytes arrive, and parsing them would put
     * this loop's own cost into the measurement. */
    if (eth_mode == ETH_MODE_ECHO) {
        for (struct pbuf *q = p; q != NULL; q = q->next) {
            eth_take_echo((const uint8_t *)q->payload, q->len);
        }
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
    tcp_arg(pcb, pcb);
    tcp_recv(pcb, eth_recv_cb);
    tcp_err(pcb, eth_err_cb);
    return ERR_OK;
}

/* Brings the stack up once, then applies whichever addressing was asked for.
 * MX_LWIP_Init registers the netif and starts DHCP, so a static address is a
 * change made after the fact rather than a different init path - that keeps
 * the CubeMX-generated function untouched. */
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
        else {
            snprintf(err, err_len, "mode=\"%s\" must be echo, sink or source", probe);
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
    eth_running      = 1;
    eth_due_ms       = HAL_GetTick();
    return 1;
}

static int eth_set(const char *args, char *err, uint32_t err_len)
{
    uint32_t was_port = eth_port;

    if (!eth_apply(args, err, err_len)) {
        return 0;
    }
    /* A changed address or port has to be acted on, not just recorded: the
     * panel shows what it sent, and a session still listening on the old port
     * would make that display a lie. */
    if (!eth_lwip_up(err, err_len)) {
        return 0;
    }
    if ((eth_port != was_port) && !eth_listen(err, err_len)) {
        return 0;
    }
    return 1;
}

/* Drops the client without leaving it in TIME_WAIT holding the listen port.
 * SO_REUSE is off in this lwIP build, so a politely closed connection makes
 * the next pt.start eth fail with "port is already bound" for a minute. */
static void eth_drop_client(void)
{
    struct tcp_pcb *pcb = eth_client_pcb;

    if (pcb == NULL) {
        return;
    }
    eth_client_pcb = NULL;
    tcp_arg(pcb, NULL);
    tcp_recv(pcb, NULL);
    tcp_err(pcb, NULL);
    tcp_sent(pcb, NULL);
    tcp_abort(pcb);
}

static void eth_stop(void)
{
    eth_drop_client();
    if (eth_listen_pcb != NULL) {
        tcp_close(eth_listen_pcb);
        eth_listen_pcb = NULL;
    }
    eth_running = 0;

    /* lwIP itself is left up. Bringing it down would need the netif and the
     * ETH DMA torn down, and the next start would have to rebuild all of it -
     * for no gain, since nothing else in the image contends for it. */
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

    if ((int32_t)(now_ms - eth_due_ms) < 0) {
        return;
    }
    eth_due_ms = now_ms + eth_period_ms;

    /* Only echo hands the peer a number to send back, so only echo may count a
     * miss. sink and source push bytes one way and never ask for an answer;
     * advancing the counter there reported misses against a peer that was
     * never given anything to answer - a working transfer read as a dead link.
     * This is the same rule usb's info mode already followed. */
    if (eth_mode == ETH_MODE_ECHO) {
        PortTool_EchoTick(&eth_echo);
    }

    /* Sent on the link under test. The peer sends this number straight back -
     * unchanged, like every other loop=link port (DECISIONS.md 18). */
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
             " rx_bytes=%lu tx_bytes=%lu kbps=%lu",
             (netif_default != NULL) ? ip4addr_ntoa(netif_ip4_addr(netif_default))
                                     : "0.0.0.0",
             link,
             (eth_client_pcb != NULL),
             eth_mode_name(eth_mode),
             (unsigned long)eth_port,
             (unsigned long)eth_rx_bytes,
             (unsigned long)eth_tx_bytes,
             (unsigned long)kbps);

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
    /* ip= takes a dotted address as well as the word dhcp, which none of the
     * four spec shapes can express. Stating the enumeration alone would make
     * the panel refuse a static address the firmware accepts, so it is left
     * out and the firmware's own check is the only gate. */
    snprintf(out, out_len, "mode:echo|sink|source port:1..65535 period:%lu..",
             (unsigned long)PORTTOOL_PERIOD_MIN_MS);
}

/* The stack needs servicing whether or not a session is running: it keeps DHCP
 * renewing and ARP answering, and once it is up, stopping that would strand
 * the board's address. PortTool_Run calls this every pass. */
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
    /* No echo hook on purpose: the reply comes back over the link under test,
     * so accepting one on the control port would let the counter climb while
     * the network is dead. Same contract as rs485 and can. */
    .echo     = NULL,
    .caps     = eth_caps,
    .limits   = eth_limits,
};

#endif /* PORTTOOL_ENABLE */

// porttool_can.c
//
// CAN session: send a frame a period, count what comes back.
//
// Parameters:
//   baud=500000  125000 / 250000 / 500000 / 1000000  (default 500000)
//   mode=normal  normal | listen | loopback | extloop | echo | bus (default normal)
//   period=1000  milliseconds between frames         (default 1000, floor 50)
//   tx=<id>:<hex>  pt.set only, mode=bus only: send one standard frame
//
// mode=bus is the CAN analyser window's (DECISIONS.md 111, PORTTOOL-FLOW.md
// B.10): it starts no frame of its own, sends what tx= asks for and reports
// every frame both ways as an event:
//   !can.tx t=48213 id=0x123 dlc=3 data=112233 ok=1
//   !can.rx t=48214 id=0x456 dlc=0 data=-
// and its periodic frame adds lost= (received, not reported) and bus=.
//
// Frame:
//   !can t=48213 seq=1 rx=0 miss=0 bps=500000 mode=normal alive=1
//        tx=12 rx_frames=11 junk=0 replied=0 tec=0 rec=0 lec=none
//
// loop=link: the counter travels over the CAN pair, so seq/rx/miss are this
// port's verdict and pt.echo is refused (DECISIONS.md 9).
//
// What each mode proves:
//   loopback  controller-internal, never reaches a pin: peripheral, clock and
//             bit timing only.
//   extloop   TX fed back to RX inside the controller, RX pin ignored: also that
//             CAN_TX is driven - nothing about ISO1044 (U8), U7 or the wiring
//             (RM0433, FDCAN test modes).
//   listen    bus monitoring, transmits nothing: judge rx_frames and junk.
//   normal    needs a real second node to acknowledge.
//   echo      the board responds, never originates: each frame on the session
//             ID goes back with its payload +1 (big-endian), same ID and
//             length (DECISIONS.md 40). Judge replied= against rx_frames=;
//             miss climbs on a healthy bus.
//
// The bus side of ISO1044 is powered by U7: a dead U7 shows alive=1, tx
// climbing, rx_frames stuck at 0.
//
// alive= (FDCAN1->ENDN) only says the peripheral is clocked. tec=/rec= read 0
// on an empty bus with automatic retransmission off; treat them as recorded
// values, not criteria (cause not confirmed against RM0433). The criterion is
// rx_frames against tx.

#include "port_can.h"        /* must precede main.h - see its header */

#include "porttool.h"
#include "porttool_cmd.h"

#include <stdio.h>
#include <string.h>

#if PORTTOOL_ENABLE

#define CAN_SESSION_ID 0x123u

/* How long a tick waits for a loopback frame to come back; one is back in
 * about 1 ms. */
#define CAN_RX_WAIT_MS 20u

/* A hard bound on one drain: mode=echo transmits inside the drain, so a bus
 * that hands sent frames back would spin forever. 64 = the RX FIFO depth. */
#define CAN_DRAIN_MAX 64u

/* Who starts frames, on top of the HAL mode; echo and bus open NORMAL. */
typedef enum { CAN_ROLE_SEND = 0, CAN_ROLE_ECHO, CAN_ROLE_BUS } can_role_t;

/* mode=bus reports at most this many received frames per pass (the control
 * line cannot carry a busy bus); the rest are counted in lost=. */
#define CAN_BUS_REPORT_MAX 8u

static uint8_t  can_rate = PORT_CAN_RATE_DEFAULT;
static uint32_t can_mode = FDCAN_MODE_NORMAL;
static can_role_t can_role = CAN_ROLE_SEND;
static uint32_t can_period_ms = 1000u;
static uint32_t can_due_ms;

static uint32_t can_tx_count;
static uint32_t can_rx_count;
static uint32_t can_junk;      /* frames that were not ours */
static uint32_t can_replied;   /* frames answered in mode=echo */
static uint32_t can_lost;      /* mode=bus: received but not reported */

static porttool_echo_t can_echo;

/* The payload is the echo counter, big end first. */
static void can_put_u32(uint8_t *d, uint32_t v)
{
    d[0] = (uint8_t)(v >> 24);
    d[1] = (uint8_t)(v >> 16);
    d[2] = (uint8_t)(v >> 8);
    d[3] = (uint8_t)v;
}

static uint32_t can_get_u32(const uint8_t *d)
{
    return ((uint32_t)d[0] << 24) | ((uint32_t)d[1] << 16) |
           ((uint32_t)d[2] << 8)  | (uint32_t)d[3];
}

/* The role wins: echo and bus open the controller in NORMAL. */
static const char *can_mode_name(uint32_t mode)
{
    if (can_role == CAN_ROLE_ECHO) {
        return "echo";
    }
    if (can_role == CAN_ROLE_BUS) {
        return "bus";
    }
    switch (mode) {
    case FDCAN_MODE_BUS_MONITORING:    return "listen";
    case FDCAN_MODE_INTERNAL_LOOPBACK: return "loopback";
    case FDCAN_MODE_EXTERNAL_LOOPBACK: return "extloop";
    default:                           return "normal";
    }
}

static int can_apply(const char *args, char *err, uint32_t err_len)
{
    char probe[64];
    uint32_t v;

    uint8_t  want_rate   = can_rate;
    uint32_t want_mode   = can_mode;
    can_role_t want_role = can_role;
    uint32_t want_period = can_period_ms;

    if (PortCmd_GetStr(args, "baud", probe, sizeof(probe))) {
        uint8_t idx;
        if (!PortCmd_GetU32(args, "baud", &v) || !PortCan_RateIndex(v, &idx)) {
            snprintf(err, err_len,
                     "baud=\"%s\" must be one of 125000, 250000, 500000, 1000000",
                     probe);
            return 0;
        }
        want_rate = idx;
    }

    if (PortCmd_GetStr(args, "mode", probe, sizeof(probe))) {
        /* Every branch sets the role too. */
        want_role = CAN_ROLE_SEND;
        if (strcmp(probe, "normal") == 0) {
            want_mode = FDCAN_MODE_NORMAL;
        } else if (strcmp(probe, "listen") == 0) {
            want_mode = FDCAN_MODE_BUS_MONITORING;
        } else if (strcmp(probe, "loopback") == 0) {
            want_mode = FDCAN_MODE_INTERNAL_LOOPBACK;
        } else if (strcmp(probe, "extloop") == 0) {
            want_mode = FDCAN_MODE_EXTERNAL_LOOPBACK;
        } else if (strcmp(probe, "echo") == 0) {
            want_mode = FDCAN_MODE_NORMAL;
            want_role = CAN_ROLE_ECHO;
        } else if (strcmp(probe, "bus") == 0) {
            want_mode = FDCAN_MODE_NORMAL;
            want_role = CAN_ROLE_BUS;
        } else {
            snprintf(err, err_len,
                     "mode=\"%s\" must be normal, listen, loopback, extloop, "
                     "echo or bus", probe);
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

    can_rate      = want_rate;
    can_mode      = want_mode;
    can_role      = want_role;
    can_period_ms = want_period;
    return 1;
}

static int can_start(const char *args, char *err, uint32_t err_len)
{
    if (!can_apply(args, err, err_len)) {
        return 0;
    }

    if (!PortCan_Open(can_rate, can_mode, 0)) {
        snprintf(err, err_len,
                 "the FDCAN controller would not start at %lu bit/s",
                 (unsigned long)PortCan_RateBps(can_rate));
        return 0;
    }
    if (!PortCan_Alive()) {
        /* ENDN wrong = no kernel clock, which would look like a dead
         * transceiver on the bus. */
        PortCan_Close();
        snprintf(err, err_len,
                 "FDCAN1 is not clocked (ENDN wrong), so nothing would reach the bus");
        return 0;
    }

    can_tx_count = 0;
    can_rx_count = 0;
    can_junk     = 0;
    can_replied  = 0;
    can_lost     = 0;
    PortTool_EchoReset(&can_echo);
    can_due_ms = HAL_GetTick();
    return 1;
}

/* "123:112233", "0x123:11", "7FF" -> a standard ID and 0-8 data bytes. */
static int can_parse_tx(const char *v, uint32_t *id, uint8_t *d, uint8_t *len)
{
    uint32_t n = 0;
    int digits = 0;

    if (v[0] == '0' && (v[1] == 'x' || v[1] == 'X')) {
        v += 2;
    }
    for (; *v != '\0' && *v != ':'; v++) {
        int h = PortCmd_HexDigit(*v);
        if (h < 0 || ++digits > 3) { return 0; }
        n = (n << 4) | (uint32_t)h;
    }
    if (digits == 0 || n > 0x7FFu) { return 0; }
    *id = n;
    *len = 0;
    if (*v == ':') {
        v++;
    }
    while (*v != '\0') {
        int hi = PortCmd_HexDigit(v[0]);
        int lo = (hi < 0) ? -1 : PortCmd_HexDigit(v[1]);
        if (lo < 0 || *len >= 8u) { return 0; }
        d[(*len)++] = (uint8_t)((hi << 4) | lo);
        v += 2;
    }
    return 1;
}

static void can_hexdump(char *out, uint32_t out_len, const uint8_t *d, uint8_t len)
{
    uint32_t n = 0;

    if (len == 0u) {
        (void) snprintf(out, out_len, "-");
        return;
    }
    for (uint8_t i = 0; i < len && n + 3u <= out_len; i++) {
        n += (uint32_t) snprintf(out + n, out_len - n, "%02X", d[i]);
    }
}

/* Sends one frame for the analyser window and says so as an event, whether
 * the TX FIFO took it or not. */
static void can_bus_send(uint32_t id, const uint8_t *d, uint8_t len)
{
    char hex[20];
    int ok = PortCan_Send(id, d, len);

    if (ok) {
        can_tx_count++;
    }
    can_hexdump(hex, sizeof(hex), d, len);
    PortTool_Frame("can.tx", "id=0x%03lX dlc=%u data=%s ok=%d",
                   (unsigned long)id, (unsigned)len, hex, ok);
}

static int can_set(const char *args, char *err, uint32_t err_len)
{
    char probe[64];
    uint32_t id = 0;
    uint8_t d[8];
    uint8_t len = 0;
    int tx;

    /* baud and mode are taken only at init, so refused rather than ignored. */
    if (PortCmd_GetStr(args, "baud", probe, sizeof(probe)) ||
        PortCmd_GetStr(args, "mode", probe, sizeof(probe))) {
        snprintf(err, err_len,
                 "baud and mode are set when the controller opens - stop the "
                 "session and start it again to change them");
        return 0;
    }

    /* Checked before anything is applied: a refused line changes nothing. */
    tx = PortCmd_GetStr(args, "tx", probe, sizeof(probe));
    if (tx && can_role != CAN_ROLE_BUS) {
        snprintf(err, err_len, "tx= is for mode=bus only");
        return 0;
    }
    if (tx && !can_parse_tx(probe, &id, d, &len)) {
        snprintf(err, err_len,
                 "tx=\"%s\" must be <id>:<data>, id hex up to 7FF, data 0-8 hex bytes",
                 probe);
        return 0;
    }
    if (!can_apply(args, err, err_len)) {
        return 0;
    }
    if (tx) {
        can_bus_send(id, d, len);
    }
    return 1;
}

static void can_stop(void)
{
    PortCan_Close();
}

/* Frames on our ID carry an echo value; anything else counts as junk. */
static void can_collect(void)
{
    uint32_t id = 0;
    uint8_t  d[8] = {0};
    uint8_t  len = 0;
    uint32_t drained = 0;

    uint32_t reported = 0;

    while (drained++ < CAN_DRAIN_MAX && PortCan_Receive(&id, d, &len)) {
        if (can_role == CAN_ROLE_BUS) {
            char hex[20];

            can_rx_count++;
            if (reported++ >= CAN_BUS_REPORT_MAX) {
                can_lost++;
                continue;
            }
            can_hexdump(hex, sizeof(hex), d, len);
            PortTool_Frame("can.rx", "id=0x%03lX dlc=%u data=%s",
                           (unsigned long)id, (unsigned)len, hex);
            continue;
        }
        if (id == CAN_SESSION_ID && len >= 4u) {
            can_rx_count++;

            /* Replies count in can_replied, not can_tx_count, so tx stays
             * frames this session started. */
            if (can_role == CAN_ROLE_ECHO) {
                can_put_u32(d, can_get_u32(d) + 1u);
                if (PortCan_Send(CAN_SESSION_ID, d, len)) {
                    can_replied++;
                }
            } else {
                PortTool_EchoGot(&can_echo, can_get_u32(d));
            }
        } else {
            can_junk++;
        }
    }
}

static void can_tick(uint32_t now_ms)
{
    char body[PORTTOOL_REPLY_MAX];
    uint32_t n;
    uint32_t tec = 0, rec = 0, lec;
    int bus;
    uint8_t d[8] = {0};

    can_collect();

    if ((int32_t)(now_ms - can_due_ms) < 0) {
        return;
    }
    can_due_ms = now_ms + can_period_ms;

    /* Listen-only and the responder roles never start a frame. */
    if (can_mode != FDCAN_MODE_BUS_MONITORING && can_role == CAN_ROLE_SEND) {
        uint32_t sent_seq = can_echo.seq;

        can_put_u32(d, sent_seq);

        /* Drop a stale reply, or the wait below returns on the previous
         * frame and the loop runs one frame behind. */
        can_echo.have = 0;

        if (PortCan_Send(CAN_SESSION_ID, d, 4u)) {
            can_tx_count++;
        }
        /* Wait briefly: the send only queues into the TX FIFO, so an
         * immediate collect misses the loopback frame. */
        uint32_t start = HAL_GetTick();
        while ((HAL_GetTick() - start) < CAN_RX_WAIT_MS) {
            can_collect();
            if (can_echo.have && can_echo.got == sent_seq) {
                break;
            }
        }
    }

    PortCan_Counters(&tec, &rec);
    lec = PortCan_LastError();
    bus = PortCan_State();   /* after LastError: reading PSR clears LEC */

    PortTool_EchoTick(&can_echo);
    n = PortTool_EchoFields(&can_echo, body, sizeof(body));
    (void) snprintf(body + n, sizeof(body) - n,
                    " bps=%lu mode=%s alive=%d tx=%lu rx_frames=%lu junk=%lu"
                    " replied=%lu tec=%lu rec=%lu lec=%lu",
                    (unsigned long)PortCan_RateBps(can_rate),
                    can_mode_name(can_mode), PortCan_Alive(),
                    (unsigned long)can_tx_count, (unsigned long)can_rx_count,
                    (unsigned long)can_junk, (unsigned long)can_replied,
                    (unsigned long)tec, (unsigned long)rec,
                    (unsigned long)lec);
    if (can_role == CAN_ROLE_BUS) {
        static const char *const names[] = { "active", "warning", "passive", "off" };
        uint32_t m = (uint32_t) strlen(body);

        (void) snprintf(body + m, sizeof(body) - m, " lost=%lu bus=%s",
                        (unsigned long)can_lost, names[bus & 3]);
    }

    PortTool_Frame("can", "%s", body);
}

static void can_caps(char *out, uint32_t out_len)
{
    snprintf(out, out_len, "baud=%lu mode=%s period=%lu",
             (unsigned long)PortCan_RateBps(can_rate),
             can_mode_name(can_mode), (unsigned long)can_period_ms);
}

/* The rate list comes from the driver's table. */
static void can_limits(char *out, uint32_t out_len)
{
    uint32_t n = (uint32_t) snprintf(out, out_len, "baud:");

    for (uint8_t i = 0; i < PORT_CAN_RATE_COUNT && n < out_len; i++) {
        int w = snprintf(out + n, out_len - n, "%s%lu",
                         (i == 0) ? "" : "|",
                         (unsigned long)PortCan_RateBps(i));
        if (w < 0) { break; }
        n += (uint32_t)w;
    }
    if (n < out_len) {
        (void) snprintf(out + n, out_len - n,
                        " mode:normal|listen|loopback|extloop|echo|bus period:%lu..",
                        (unsigned long)PORTTOOL_PERIOD_MIN_MS);
    }
}

/* Clock source and bit timing behind the current baud=, for pt.caps
 * (DECISIONS.md 87). The timing table is exact only on HSE, so any other
 * source reports clk_hz=0 and bps=0. */
static void can_info(char *out, uint32_t out_len)
{
    uint16_t presc = 0, seg1 = 0, seg2 = 0, sjw = 0;
    uint32_t hz = PortCan_ClockHz();
    uint32_t tq;

    (void)PortCan_Timing(can_rate, &presc, &seg1, &seg2, &sjw);
    tq = 1U + seg1 + seg2;
    snprintf(out, out_len,
             "clk=%s clk_hz=%lu presc=%u seg1=%u seg2=%u sjw=%u tq=%lu bps=%lu",
             PortCan_ClockName(), (unsigned long)hz, (unsigned)presc,
             (unsigned)seg1, (unsigned)seg2, (unsigned)sjw, (unsigned long)tq,
             (unsigned long)((presc != 0U) ? hz / ((uint32_t)presc * tq) : 0U));
}

porttool_port_t porttool_can = {
    .name     = "can",
    .board    = PORTTOOL_BOARD_UPPER,
    .blk      = "C",
    .term     = "C07,C08",
    .terms    = "C07+C08",
    .params   = "baud,mode,period",
    .loop     = PORTTOOL_LOOP_LINK,
    .channels = 1,
    .start    = can_start,
    .set      = can_set,
    .stop     = can_stop,
    .tick     = can_tick,
    /* No echo hook: the loop travels over the CAN pair (DECISIONS.md 9). */
    .echo     = NULL,
    .caps     = can_caps,
    .limits   = can_limits,
    .info     = can_info,
};

#endif /* PORTTOOL_ENABLE */

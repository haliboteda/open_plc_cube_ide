// porttool_can.c
//
// CAN session: send a frame a period, count what comes back.
//
// Parameters:
//   baud=500000  125000 / 250000 / 500000 / 1000000  (default 500000)
//   mode=normal  normal | listen | loopback | extloop | echo (default normal)
//   period=1000  milliseconds between frames         (default 1000, floor 50)
//
// Frame:
//   !can t=48213 seq=1 rx=0 miss=0 bps=500000 mode=normal alive=1
//        tx=12 rx_frames=11 junk=0 replied=0 tec=0 rec=0 lec=none
//
// *** loop=link. *** The counter travels over the CAN pair, not over the
// control port, so seq/rx/miss ARE this port's verdict - and pt.echo is
// refused here for the same reason it is refused for rs485: answering on the
// control port would let the counter climb with the CAN pair dead
// (DECISIONS.md 9).
//
// The four modes prove four different things, and the difference matters more
// than any of them. They are the same four the jointly-debugged bring-up test
// uses (../CAN/can_test.c phases P1-P4), so a session result and that test's
// result are about the same thing.
//
//   loopback  controller-internal. Never reaches a pin. Proves the peripheral,
//             the clock and the bit timing - and NOTHING about the transceiver
//             or the bus.
//   extloop   *** through the transceiver, still no peer and no second node. ***
//             The frame leaves on CAN_TX, crosses the isolator, drives the bus,
//             and comes back in on CAN_RX. So it proves the whole chain
//             including ISO1044 and its isolated supply U7 - the one thing
//             loopback cannot see. Needs bus termination to be present.
//             *** This is the mode a production station wants: full transceiver
//             coverage with no golden node on the bench. ***
//   listen    bus monitoring. Transmits nothing, so its loop counter never
//             moves; judge rx_frames and junk instead.
//   normal    a real bus with a real second node. On a bus with no second node
//             there is no acknowledge - see the warning below about what that
//             does and does not show.
//   echo      *** the board is the responder, not the originator. *** The
//             controller runs NORMAL and the session never starts a frame of
//             its own: every frame that arrives on the session identifier has
//             its payload incremented as one big-endian number and goes back
//             out on the same identifier and length. This replaces the
//             standalone echo entry that used to require handing the board
//             over (DECISIONS.md 40).
//
// *** seq/rx/miss are NOT the verdict in mode=echo, the same way they are not
// *** in mode=listen: nothing originates here, so the loop never closes and
// *** miss climbs every period on a perfectly healthy bus. The judgeable
// *** number is replied= against rx_frames=.
//
// ⚠️ The transceiver is isolated (ISO1044) and its bus side is powered by U7.
// If U7 is dead the controller looks perfectly healthy and the bus does
// nothing at all - alive=1 with tx climbing and rx_frames stuck at 0. extloop
// is what tells that apart from a healthy board with nobody to talk to.
//
// ⚠️ alive=, tec= and rec= are NOT verdicts on the bus.
//   alive=  reads FDCAN1->ENDN, so it says the peripheral is clocked and
//           readable. A dead bus reads alive=1.
//   tec= rec= measured 0 on this board (2026-09-08) on an empty bus in
//           mode=normal with tx climbing to 8 - the controller is opened with
//           automatic retransmission disabled. Whether that is why has NOT
//           been confirmed against the reference manual, so until it is,
//           treat both as recorded values and not as criteria.
// The criterion that does hold is rx_frames against tx.

#include "port_can.h"        /* must precede main.h - see its header */

#include "porttool.h"
#include "porttool_cmd.h"

#include <stdio.h>
#include <string.h>

#if PORTTOOL_ENABLE

#define CAN_SESSION_ID 0x123u

/* How long a tick waits for a loopback frame to come back. Same value and same
 * reason as ../CAN/can_test.c: a loopback frame is back in about 1 ms. */
#define CAN_RX_WAIT_MS 20u

/* *** A hard bound on one drain, not an optimisation. *** mode=echo transmits
 * from inside the drain loop, so anything that hands a sent frame back as a
 * received one - the contract test's stub, or a bus wired into its own
 * receiver - turns an unbounded while into a spin that no timeout escapes.
 * The FDCAN RX FIFO is 64 deep, so a full FIFO still empties in one call and
 * whatever arrives after that is the next tick's business. */
#define CAN_DRAIN_MAX 64u

/* What the session does with the bus, on top of the controller's HAL mode.
 * echo is the only one that is not a HAL mode of its own: the controller runs
 * NORMAL and the difference is entirely in who starts a frame. Kept separate
 * so that adding a role never silently changes which HAL mode is opened. */
typedef enum { CAN_ROLE_SEND = 0, CAN_ROLE_ECHO } can_role_t;

static uint8_t  can_rate = PORT_CAN_RATE_DEFAULT;
static uint32_t can_mode = FDCAN_MODE_NORMAL;
static can_role_t can_role = CAN_ROLE_SEND;
static uint32_t can_period_ms = 1000u;
static uint32_t can_due_ms;

static uint32_t can_tx_count;
static uint32_t can_rx_count;
static uint32_t can_junk;      /* frames that were not ours */
static uint32_t can_replied;   /* frames answered in mode=echo */

static porttool_echo_t can_echo;

/* The payload is the echo counter, big end first, so a peer that returns the
 * payload unchanged closes the loop without having to understand anything. */
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

/* The role wins when it is set: mode=echo opens the controller in NORMAL, so
 * reporting the HAL mode here would print "normal" for a session that is not
 * originating anything - a frame that disagrees with the command that started
 * it. */
static const char *can_mode_name(uint32_t mode)
{
    if (can_role == CAN_ROLE_ECHO) {
        return "echo";
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
        /* Every branch sets the role too. Leaving it alone would let a session
         * started as echo stay a responder after being set back to normal. */
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
        } else {
            snprintf(err, err_len,
                     "mode=\"%s\" must be normal, listen, loopback, extloop or "
                     "echo", probe);
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
        /* ENDN reads wrong when the kernel clock never arrived, which on the
         * bus looks exactly like a dead transceiver. Saying so here saves an
         * hour with a scope on the wrong side of the isolator. */
        PortCan_Close();
        snprintf(err, err_len,
                 "FDCAN1 is not clocked (ENDN wrong), so nothing would reach the bus");
        return 0;
    }

    can_tx_count = 0;
    can_rx_count = 0;
    can_junk     = 0;
    can_replied  = 0;
    PortTool_EchoReset(&can_echo);
    can_due_ms = HAL_GetTick();
    return 1;
}

static int can_set(const char *args, char *err, uint32_t err_len)
{
    char probe[64];

    /* baud and mode are peripheral configuration, and the HAL only takes them
     * at init. Changing them under a running session would report one thing
     * and do another, so they are refused rather than silently ignored. */
    if (PortCmd_GetStr(args, "baud", probe, sizeof(probe)) ||
        PortCmd_GetStr(args, "mode", probe, sizeof(probe))) {
        snprintf(err, err_len,
                 "baud and mode are set when the controller opens - stop the "
                 "session and start it again to change them");
        return 0;
    }
    return can_apply(args, err, err_len);
}

static void can_stop(void)
{
    PortCan_Close();
}

/* Anything arriving with our ID carries an echo value; anything else is
 * another node's traffic and is counted separately rather than being taken
 * for a reply. */
static void can_collect(void)
{
    uint32_t id = 0;
    uint8_t  d[8] = {0};
    uint8_t  len = 0;
    uint32_t drained = 0;

    while (drained++ < CAN_DRAIN_MAX && PortCan_Receive(&id, d, &len)) {
        if (id == CAN_SESSION_ID && len >= 4u) {
            can_rx_count++;

            /* As the responder, answer on the same identifier and length with
             * the payload incremented, so the node that sent it can tell its
             * own frame coming back from somebody else's traffic. Counted
             * separately from can_tx_count: a reply is not a frame this
             * session started, and folding the two together would make the
             * tx-against-rx_frames criterion meaningless here. */
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
    uint8_t d[8] = {0};

    can_collect();

    if ((int32_t)(now_ms - can_due_ms) < 0) {
        return;
    }
    can_due_ms = now_ms + can_period_ms;

    /* Listen-only must not transmit: the whole point of bus monitoring is to
     * watch a live bus without disturbing it. The responder must not either -
     * it answers from can_collect and starts nothing of its own. */
    if (can_mode != FDCAN_MODE_BUS_MONITORING && can_role != CAN_ROLE_ECHO) {
        uint32_t sent_seq = can_echo.seq;

        can_put_u32(d, sent_seq);

        /* Anything already sitting in `got` answers an earlier frame, not the
         * one about to go out. Dropping it here is what keeps the wait below
         * from returning on a stale value: on the board 2026-09-08 the first
         * tick left a reply uncollected (EchoTick returns early on the very
         * first tick by design), and every tick after that collected the
         * PREVIOUS tick's frame - the loop ran permanently one frame behind
         * and reported miss on alternate periods with a healthy transceiver. */
        can_echo.have = 0;

        if (PortCan_Send(CAN_SESSION_ID, d, 4u)) {
            can_tx_count++;
        }
        /* Wait briefly for a loopback frame rather than polling once: the
         * send above only queues into the TX FIFO, so the frame has not left
         * the controller yet and an immediate collect always misses it. The
         * bound and the reasoning are the jointly-debugged test's
         * (../CAN/can_test.c CAN_RX_WAIT_MS, "a loopback frame is back in
         * ~1 ms").
         *
         * Without this the loop closed only every second period - measured on
         * the board 2026-09-08, miss alternating 0,1,0,1 on a healthy
         * transceiver, which is a counter that cannot be judged.
         *
         * In mode=normal there is nothing to wait for, so this costs the
         * bound once per period and changes nothing else. */
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

    PortTool_Frame("can", "%s", body);
}

static void can_caps(char *out, uint32_t out_len)
{
    snprintf(out, out_len, "baud=%lu mode=%s period=%lu",
             (unsigned long)PortCan_RateBps(can_rate),
             can_mode_name(can_mode), (unsigned long)can_period_ms);
}

/* The rate list is generated from the driver's table rather than typed out, so
 * adding a rate there is the only edit needed for the panel to offer it. */
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
                        " mode:normal|listen|loopback|extloop|echo period:%lu..",
                        (unsigned long)PORTTOOL_PERIOD_MIN_MS);
    }
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
    /* No echo hook: the loop travels over the CAN pair. Offering pt.echo here
     * would let the counter climb on a dead bus - DECISIONS.md 9. */
    .echo     = NULL,
    .caps     = can_caps,
    .limits   = can_limits,
};

#endif /* PORTTOOL_ENABLE */

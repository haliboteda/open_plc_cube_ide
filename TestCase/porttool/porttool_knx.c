// porttool_knx.c
//
// KNX session: watch the TP1 bus, or send a character a period and see it
// come back.
//
// Parameters:
//   mode=loopback  loopback | listen              (default loopback)
//   period=1000    milliseconds between sends     (default 1000, floor 50)
//
// Frame:
//   !knx t=48213 seq=1 rx=0 miss=0 mode=loopback bus=ok vcc=1 ok=1 idle=0
//        pulses=26 dropped=0 chars=2 bad=0 w_avg=35 d_avg=7 quiet_ms=12
//
// *** loop=link, and the link is the real one. *** The round trip is
// MCU -> STKNX -> bus -> STKNX -> MCU, so a closed loop proves the transceiver
// and the bus, not just the controller. pt.echo is refused for the same reason
// as rs485 and can (DECISIONS.md 9).
//
// *** mode=listen never transmits, and its loop counter does not move. ***
// Judge pulses=, chars= and the width statistics instead. A counter that
// climbed while nothing was sent would be a number with no meaning.
//
// ⚠️ Sending spins for about 1.35 ms - see KNX_Test_SessionSendChar. One
// character a period is fine; the frame's own console output already costs
// several times that.
//
// ⚠️ w_avg is the active pulse width and should sit near KNX_PULSE_US (35 us).
// A bus with no power reads as pulses=0 with bus=dead, which is the honest
// answer rather than a timing complaint.

#include "porttool.h"
#include "porttool_cmd.h"
#include "KNX/knx_test.h"

#include "main.h"
#include <stdio.h>
#include <string.h>

#if PORTTOOL_ENABLE

/* Walks so a stuck value is visible, and avoids 0x00/0xFF which are the two
 * patterns a dead line can imitate. */
static const uint8_t KNX_PATTERN[] = { 0xAAu, 0x55u, 0x5Au, 0xA5u };
#define KNX_PATTERN_COUNT (sizeof(KNX_PATTERN) / sizeof(KNX_PATTERN[0]))

static int      knx_listen_only;
static uint32_t knx_period_ms = 1000u;
static uint32_t knx_due_ms;
static int      knx_inited;

static uint32_t knx_chars;      /* characters that decoded cleanly */
static uint32_t knx_bad;        /* characters that closed but failed framing */
static uint32_t knx_mismatch;   /* decoded fine, but not what was sent */
static uint8_t  knx_sent_byte;
static uint8_t  knx_pattern_i;

static porttool_echo_t knx_echo;

static int knx_apply(const char *args, char *err, uint32_t err_len)
{
    char probe[64];
    uint32_t v;

    int      want_listen = knx_listen_only;
    uint32_t want_period = knx_period_ms;

    if (PortCmd_GetStr(args, "mode", probe, sizeof(probe))) {
        if (strcmp(probe, "loopback") == 0) {
            want_listen = 0;
        } else if (strcmp(probe, "listen") == 0) {
            want_listen = 1;
        } else {
            snprintf(err, err_len, "mode=\"%s\" must be loopback or listen", probe);
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

    knx_listen_only = want_listen;
    knx_period_ms   = want_period;
    return 1;
}

static int knx_start(const char *args, char *err, uint32_t err_len)
{
    if (!knx_apply(args, err, err_len)) {
        return 0;
    }
    if (!knx_inited) {
        if (!KNX_Test_SessionInit()) {
            snprintf(err, err_len, "the KNX capture and bit engine would not start");
            return 0;
        }
        knx_inited = 1;
    }
    KNX_Test_SessionStatsReset();

    knx_chars = 0;
    knx_bad = 0;
    knx_mismatch = 0;
    knx_pattern_i = 0;
    knx_sent_byte = 0;
    PortTool_EchoReset(&knx_echo);
    knx_due_ms = HAL_GetTick();
    return 1;
}

static int knx_set(const char *args, char *err, uint32_t err_len)
{
    return knx_apply(args, err, err_len);
}

static void knx_stop(void)
{
    /* Nothing is driven between characters: the bit engine idles the line low
     * by itself, which is what PB14 must never be left floating from. */
}

/* Takes every character the capture ISR has completed. Called each superloop
 * pass, not once a period: the decoder closes a character on its own timeout,
 * and one only drained per period would age out of the ring. */
static void knx_collect(void)
{
    uint8_t got = 0, framing_ok = 0;

    while (KNX_Test_SessionPollChar(&got, &framing_ok)) {
        if (!framing_ok) {
            knx_bad++;
            continue;
        }
        knx_chars++;
        if (knx_listen_only) {
            continue;
        }
        if (got == knx_sent_byte) {
            /* The character we put on the bus came back intact, so this period
             * closed. The echo counter carries the sequence, not the byte. */
            PortTool_EchoGot(&knx_echo, knx_echo.seq);
        } else {
            knx_mismatch++;
        }
    }
}

static const char *knx_bus_name(uint8_t bus)
{
    switch (bus) {
    case 0:  return "ok";
    case 1:  return "dead";
    default: return "odd";
    }
}

static void knx_tick(uint32_t now_ms)
{
    char body[PORTTOOL_REPLY_MAX];
    knx_session_stats_t st;
    uint32_t n;

    knx_collect();

    if ((int32_t)(now_ms - knx_due_ms) < 0) {
        return;
    }
    knx_due_ms = now_ms + knx_period_ms;

    if (!knx_listen_only) {
        knx_sent_byte = KNX_PATTERN[knx_pattern_i];
        knx_pattern_i = (uint8_t)((knx_pattern_i + 1u) % KNX_PATTERN_COUNT);
        (void) KNX_Test_SessionSendChar(knx_sent_byte);
        /* The reply is back within a couple of character times, so draining
         * here lets a period close in the same tick it opened. */
        knx_collect();
        PortTool_EchoTick(&knx_echo);
    }

    KNX_Test_SessionStats(&st);

    n = PortTool_EchoFields(&knx_echo, body, sizeof(body));
    (void) snprintf(body + n, sizeof(body) - n,
                    " mode=%s bus=%s vcc=%u ok=%u idle=%u pulses=%lu dropped=%lu"
                    " chars=%lu bad=%lu mismatch=%lu w_avg=%lu d_avg=%lu quiet_ms=%lu",
                    knx_listen_only ? "listen" : "loopback",
                    knx_bus_name(st.bus),
                    (unsigned)st.vcc_ok, (unsigned)st.bus_ok, (unsigned)st.rx_idle,
                    (unsigned long)st.pulses, (unsigned long)st.dropped,
                    (unsigned long)knx_chars, (unsigned long)knx_bad,
                    (unsigned long)knx_mismatch,
                    (unsigned long)st.w_avg, (unsigned long)st.d_avg,
                    (unsigned long)st.ms_since_edge);

    PortTool_Frame("knx", "%s", body);
}

static void knx_caps(char *out, uint32_t out_len)
{
    snprintf(out, out_len, "mode=%s period=%lu",
             knx_listen_only ? "listen" : "loopback",
             (unsigned long)knx_period_ms);
}

static void knx_limits(char *out, uint32_t out_len)
{
    snprintf(out, out_len, "mode:loopback|listen period:%lu..",
             (unsigned long)PORTTOOL_PERIOD_MIN_MS);
}

porttool_port_t porttool_knx = {
    .name     = "knx",
    .board    = PORTTOOL_BOARD_UPPER,
    .blk      = "C",
    .term     = "C03,C04",
    .terms    = "C03+C04",
    .params   = "mode,period",
    .loop     = PORTTOOL_LOOP_LINK,
    .channels = 1,
    .start    = knx_start,
    .set      = knx_set,
    .stop     = knx_stop,
    .tick     = knx_tick,
    /* The loop travels over the KNX pair; answering on the control port would
     * let it climb with the bus dead - DECISIONS.md 9. */
    .echo     = NULL,
    .caps     = knx_caps,
    .limits   = knx_limits,
};

#endif /* PORTTOOL_ENABLE */

// porttool_knx.c
//
// KNX session: watch the TP1 bus, send a character a period and see it come
// back, or exchange real KNX frames.
//
// Parameters:
//   mode=loopback  loopback | listen | frames     (default loopback)
//   period=1000    milliseconds between sends     (default 1000, floor 50)
//   ga=31/7/255    destination group address      (default 31/7/255, or none)
//   src=15.15.250  source individual address      (default 15.15.250)
//   val=toggle     toggle | 0 | 1                 (default toggle)
//
// Frames:
//   !knx t=48213 seq=1 rx=0 miss=0 mode=loopback bus=ok vcc=1 ok=1 idle=0
//        pulses=26 dropped=0 chars=2 bad=0 w_avg=35 d_avg=7 quiet_ms=12
//   !knx.rx t=48310 n=9 crc=raw src=15.15.250 dst=1/0/1 apci=gvwrite val=1
//        raw=BC.FF.FA.08.01.E1.00.81.7F inv=43.00.05.F7.FE.1E.FF.7E.80
//   !knx.tx t=48250 n=9 dst=1/0/1 val=1 idle=1 raw=BC.FF.FA.08.01.E1.00.81.7F
//
// *** A received frame is an event, not a periodic reading, so it gets its own
// line. *** The periodic !knx line is already 150 characters; two readings of
// a frame's octets would not fit beside it in PORTTOOL_REPLY_MAX.
//
// *** crc= is the machine's answer to "which reading is the real frame". ***
// raw = the octets as received pass the check octet, inv = they pass only
// bit-inverted, bad = neither. Both readings' octets are reported either way,
// so a person can still look; but nobody has to decide by eye. The RX chain's
// polarity is unsettled on this board - see $PROD/docs/hardware/HARDWARE-FACTS.md.
//
// *** The default group address is 31/7/255, the last one that exists. ***
// A GroupValueWrite actuates whatever subscribes to the address it carries, so
// the default has to be one a real installation is least likely to have
// assigned: main groups are handed out from 0 upwards by function, so the top
// of the range is the quiet corner - the same reasoning that picked
// 15.15.250 for the source address. ga=none stops sending altogether.
//
// ⚠️ 31/7/255 packs to 0xFFFF. Do not use that value as a "no address"
// sentinel - it is a legal address, and knx_ga_set is the flag that says
// whether one was given.
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

typedef enum {
    KNX_MODE_LOOPBACK = 0,
    KNX_MODE_LISTEN,
    KNX_MODE_FRAMES
} knx_mode_t;

static knx_mode_t knx_mode = KNX_MODE_LOOPBACK;
static uint32_t knx_period_ms = 1000u;
static uint32_t knx_due_ms;
static int      knx_inited;

/* 31/7/255 packs to 0xFFFF, so every one of the 65536 bit patterns is a legal
 * group address and "no address" needs its own flag, not a sentinel value.
 * ga=none is what clears it. */
#define KNX_SRC_DEFAULT   0xFFFAu   /* 15.15.250 - no real device owns this */
#define KNX_GA_DEFAULT    0xFFFFu   /* 31/7/255 - the last address there is */

static uint16_t knx_ga     = KNX_GA_DEFAULT;
static int      knx_ga_set = 1;
static uint16_t knx_src = KNX_SRC_DEFAULT;
static int      knx_val_toggle = 1;
static uint8_t  knx_val;

static uint32_t knx_chars;      /* characters that decoded cleanly */
static uint32_t knx_bad;        /* characters that closed but failed framing */
static uint32_t knx_mismatch;   /* decoded fine, but not what was sent */
static uint8_t  knx_sent_byte;
static uint8_t  knx_pattern_i;

static uint32_t knx_rx_frames;  /* frames assembled, whatever their crc */
static uint32_t knx_tx_frames;
static uint32_t knx_crc_raw;    /* frames whose octets passed as received */
static uint32_t knx_crc_inv;    /* frames that passed only bit-inverted */
static uint32_t knx_crc_bad;    /* frames neither reading could check */
static uint32_t knx_acks;       /* L_Ack ACK octets - a peer accepted a frame */
static uint32_t knx_ack_other;  /* lone octets that were NAK, BUSY or neither */

static porttool_echo_t knx_echo;

/* --- Address text ------------------------------------------------------- */

/* Group addresses are written main/middle/sub, individual ones area.line.dev.
 * Both pack into 16 bits, and both are parsed here rather than as plain
 * numbers because that is how they appear in ETS and on a wiring diagram.
 * eth's ip= sets the precedent for a dotted parameter (DECISIONS.md 28). */
static int knx_parse_addr(const char *s, char sep, uint32_t hi_max,
                          uint32_t mid_max, uint8_t hi_shift, uint8_t mid_shift,
                          uint16_t *out)
{
    uint32_t part[3] = { 0u, 0u, 0u };
    uint8_t  n = 0u;
    int      digits = 0;

    for (; *s != '\0'; s++) {
        if ((*s >= '0') && (*s <= '9')) {
            part[n] = (part[n] * 10u) + (uint32_t)(*s - '0');
            if (part[n] > 255u) { return 0; }
            digits = 1;
        } else if ((*s == sep) && (n < 2u) && (digits != 0)) {
            n++;
            digits = 0;
        } else {
            return 0;
        }
    }
    if ((n != 2u) || (digits == 0)) {
        return 0;
    }
    if ((part[0] > hi_max) || (part[1] > mid_max) || (part[2] > 255u)) {
        return 0;
    }
    *out = (uint16_t)((part[0] << hi_shift) | (part[1] << mid_shift) | part[2]);
    return 1;
}

static int knx_parse_ga(const char *s, uint16_t *out)
{
    return knx_parse_addr(s, '/', 31u, 7u, 11u, 8u, out);
}

static int knx_parse_ia(const char *s, uint16_t *out)
{
    return knx_parse_addr(s, '.', 15u, 15u, 12u, 8u, out);
}

static void knx_ga_text(uint16_t a, char *out, uint32_t out_len)
{
    snprintf(out, out_len, "%u/%u/%u", (unsigned)((a >> 11) & 0x1Fu),
             (unsigned)((a >> 8) & 0x07u), (unsigned)(a & 0xFFu));
}

static void knx_ia_text(uint16_t a, char *out, uint32_t out_len)
{
    snprintf(out, out_len, "%u.%u.%u", (unsigned)((a >> 12) & 0x0Fu),
             (unsigned)((a >> 8) & 0x0Fu), (unsigned)(a & 0xFFu));
}

static int knx_apply(const char *args, char *err, uint32_t err_len)
{
    char probe[64];
    uint32_t v;

    knx_mode_t want_mode   = knx_mode;
    uint32_t   want_period = knx_period_ms;
    uint16_t   want_ga     = knx_ga;
    int        want_ga_set = knx_ga_set;
    uint16_t   want_src    = knx_src;
    int        want_toggle = knx_val_toggle;
    uint8_t    want_val    = knx_val;

    if (PortCmd_GetStr(args, "mode", probe, sizeof(probe))) {
        if      (strcmp(probe, "loopback") == 0) { want_mode = KNX_MODE_LOOPBACK; }
        else if (strcmp(probe, "listen")   == 0) { want_mode = KNX_MODE_LISTEN; }
        else if (strcmp(probe, "frames")   == 0) { want_mode = KNX_MODE_FRAMES; }
        else {
            snprintf(err, err_len,
                     "mode=\"%s\" must be loopback, listen or frames", probe);
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

    if (PortCmd_GetStr(args, "ga", probe, sizeof(probe))) {
        if (strcmp(probe, "none") == 0) {
            want_ga_set = 0;
        } else if (knx_parse_ga(probe, &want_ga)) {
            want_ga_set = 1;
        } else {
            snprintf(err, err_len,
                     "ga=\"%s\" must be main/middle/sub with main 0..31, "
                     "middle 0..7, sub 0..255 - or none", probe);
            return 0;
        }
    }

    if (PortCmd_GetStr(args, "src", probe, sizeof(probe))) {
        if (!knx_parse_ia(probe, &want_src)) {
            snprintf(err, err_len,
                     "src=\"%s\" must be area.line.device with area 0..15, "
                     "line 0..15, device 0..255", probe);
            return 0;
        }
    }

    if (PortCmd_GetStr(args, "val", probe, sizeof(probe))) {
        if (strcmp(probe, "toggle") == 0) {
            want_toggle = 1;
        } else if ((strcmp(probe, "0") == 0) || (strcmp(probe, "1") == 0)) {
            want_toggle = 0;
            want_val    = (uint8_t)(probe[0] - '0');
        } else {
            snprintf(err, err_len, "val=\"%s\" must be toggle, 0 or 1", probe);
            return 0;
        }
    }

    knx_mode       = want_mode;
    knx_period_ms  = want_period;
    knx_ga         = want_ga;
    knx_ga_set     = want_ga_set;
    knx_src        = want_src;
    knx_val_toggle = want_toggle;
    knx_val        = want_val;
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

    KNX_Test_SessionFrameReset();

    knx_chars = 0;
    knx_bad = 0;
    knx_mismatch = 0;
    knx_pattern_i = 0;
    knx_sent_byte = 0;
    knx_rx_frames = 0;
    knx_tx_frames = 0;
    knx_crc_raw = 0;
    knx_crc_inv = 0;
    knx_crc_bad = 0;
    knx_acks = 0;
    knx_ack_other = 0;
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
/* One line per received frame, with both readings of its octets. */
static void knx_report_rx(const uint8_t *f, uint8_t n, uint8_t which)
{
    char body[PORTTOOL_REPLY_MAX];
    char addr[16];
    uint32_t k = 0;

    /* The frame that passed is the one worth describing. */
    static uint8_t g[KNX_FRAME_MAX];
    const uint8_t *d = f;

    if (which == KNX_FRAME_CRC_INV) {
        for (uint8_t i = 0; i < n; i++) { g[i] = (uint8_t)~f[i]; }
        d = g;
    }

    /* A lone octet is an acknowledge: no check octet, so crc= names what the
     * acknowledge said instead of judging a frame that is not there. */
    if (n == 1u) {
        uint8_t kind = KNX_Test_AckKind(f[0]);
        PortTool_Frame("knx.rx", "n=1 crc=%s raw=%02X inv=%02X",
                       (kind == KNX_ACK_ACK)  ? "ack"
                     : (kind == KNX_ACK_NAK)  ? "nak"
                     : (kind == KNX_ACK_BUSY) ? "busy" : "bad",
                       (unsigned)f[0], (unsigned)(uint8_t)~f[0]);
        return;
    }

    k += (uint32_t)snprintf(body + k, sizeof(body) - k, "n=%u crc=%s",
                            (unsigned)n,
                            (which == KNX_FRAME_CRC_RAW) ? "raw"
                          : (which == KNX_FRAME_CRC_INV) ? "inv" : "bad");

    /* Octet 5 bit 7 says whether the destination is a group or an individual
     * address; octet 7 carries GroupValueWrite plus a 6-bit value. The same
     * unpacking is printed for a person by knx_frame_report() in
     * TestCase/KNX/knx_test.c - that one builds prose, this one builds
     * key=value for the host, so the bit shifts appear in both places. */
    if ((which != KNX_FRAME_CRC_BAD) && (n >= 8u)) {
        uint16_t sa = (uint16_t)(((uint16_t)d[1] << 8) | d[2]);
        uint16_t da = (uint16_t)(((uint16_t)d[3] << 8) | d[4]);

        knx_ia_text(sa, addr, sizeof(addr));
        k += (uint32_t)snprintf(body + k, sizeof(body) - k, " src=%s", addr);

        if ((d[5] & 0x80u) != 0u) { knx_ga_text(da, addr, sizeof(addr)); }
        else                      { knx_ia_text(da, addr, sizeof(addr)); }
        k += (uint32_t)snprintf(body + k, sizeof(body) - k, " dst=%s", addr);

        if ((d[7] & 0xC0u) == 0x80u) {
            k += (uint32_t)snprintf(body + k, sizeof(body) - k,
                                    " apci=gvwrite val=%u",
                                    (unsigned)(d[7] & 0x3Fu));
        }
    }

    k += (uint32_t)snprintf(body + k, sizeof(body) - k, " raw=");
    for (uint8_t i = 0; i < n; i++) {
        k += (uint32_t)snprintf(body + k, sizeof(body) - k, "%s%02X",
                                (i == 0u) ? "" : ".", (unsigned)f[i]);
    }
    k += (uint32_t)snprintf(body + k, sizeof(body) - k, " inv=");
    for (uint8_t i = 0; i < n; i++) {
        k += (uint32_t)snprintf(body + k, sizeof(body) - k, "%s%02X",
                                (i == 0u) ? "" : ".", (unsigned)(uint8_t)~f[i]);
    }

    PortTool_Frame("knx.rx", "%s", body);
}

static void knx_collect_frames(void)
{
    uint8_t f[KNX_FRAME_MAX];
    uint8_t n = 0, which = 0, bad = 0;

    for (;;) {
        int got = KNX_Test_SessionPollFrame(f, &n, (uint8_t)sizeof(f),
                                            &which, &bad);
        knx_bad += bad;
        if (!got) {
            return;
        }
        knx_chars += n;
        if (n == 1u) {
            /* Counted as an acknowledge, not a frame: a real installation
             * answers every frame it accepts, so counting these as bad frames
             * would make a working bus look broken. */
            uint8_t kind = KNX_Test_AckKind(f[0]);
            if (kind == KNX_ACK_ACK) { knx_acks++; }
            else                     { knx_ack_other++; }
        } else {
            knx_rx_frames++;
            if      (which == KNX_FRAME_CRC_RAW) { knx_crc_raw++; }
            else if (which == KNX_FRAME_CRC_INV) { knx_crc_inv++; }
            else                                 { knx_crc_bad++; }
        }
        knx_report_rx(f, n, which);
    }
}

static void knx_collect(void)
{
    uint8_t got = 0, framing_ok = 0;

    if (knx_mode == KNX_MODE_FRAMES) {
        /* The frame layer takes the characters instead: both draining the same
         * decoder queue would race for them. */
        knx_collect_frames();
        return;
    }

    while (KNX_Test_SessionPollChar(&got, &framing_ok)) {
        if (!framing_ok) {
            knx_bad++;
            continue;
        }
        knx_chars++;
        if (knx_mode == KNX_MODE_LISTEN) {
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

static const char *knx_mode_name(knx_mode_t m)
{
    switch (m) {
    case KNX_MODE_LISTEN: return "listen";
    case KNX_MODE_FRAMES: return "frames";
    default:              return "loopback";
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

    if (knx_mode == KNX_MODE_LOOPBACK) {
        knx_sent_byte = KNX_PATTERN[knx_pattern_i];
        knx_pattern_i = (uint8_t)((knx_pattern_i + 1u) % KNX_PATTERN_COUNT);
        (void) KNX_Test_SessionSendChar(knx_sent_byte);
        /* The reply is back within a couple of character times, so draining
         * here lets a period close in the same tick it opened. */
        knx_collect();
        PortTool_EchoTick(&knx_echo);
    } else if ((knx_mode == KNX_MODE_FRAMES) && (knx_ga_set != 0)) {
        uint8_t f[KNX_FRAME_MAX];
        uint8_t idle = 0;
        uint8_t sent;

        if (knx_val_toggle) {
            knx_val = (uint8_t)(knx_val ^ 1u);
        }
        sent = KNX_Test_SessionSendGroupWrite(knx_src, knx_ga, knx_val,
                                              f, (uint8_t)sizeof(f), &idle);
        if (sent != 0u) {
            char line[PORTTOOL_REPLY_MAX];
            char addr[16];
            uint32_t k;

            knx_tx_frames++;
            knx_ga_text(knx_ga, addr, sizeof(addr));
            k = (uint32_t)snprintf(line, sizeof(line),
                                   "n=%u dst=%s val=%u idle=%u raw=",
                                   (unsigned)sent, addr, (unsigned)knx_val,
                                   (unsigned)idle);
            for (uint8_t i = 0; i < sent; i++) {
                k += (uint32_t)snprintf(line + k, sizeof(line) - k, "%s%02X",
                                        (i == 0u) ? "" : ".", (unsigned)f[i]);
            }
            PortTool_Frame("knx.tx", "%s", line);
        }
        /* The frame comes back over the bus a few character times later. */
        knx_collect();
    }

    KNX_Test_SessionStats(&st);

    n = PortTool_EchoFields(&knx_echo, body, sizeof(body));
    n += (uint32_t) snprintf(body + n, sizeof(body) - n,
                    " mode=%s bus=%s vcc=%u ok=%u idle=%u pulses=%lu dropped=%lu"
                    " chars=%lu bad=%lu mismatch=%lu w_avg=%lu d_avg=%lu quiet_ms=%lu",
                    knx_mode_name(knx_mode),
                    knx_bus_name(st.bus),
                    (unsigned)st.vcc_ok, (unsigned)st.bus_ok, (unsigned)st.rx_idle,
                    (unsigned long)st.pulses, (unsigned long)st.dropped,
                    (unsigned long)knx_chars, (unsigned long)knx_bad,
                    (unsigned long)knx_mismatch,
                    (unsigned long)st.w_avg, (unsigned long)st.d_avg,
                    (unsigned long)st.ms_since_edge);

    /* Frame-mode totals only, so the other two modes keep the line they had.
     * crc_raw / crc_inv are the running answer to which reading is real. */
    if (knx_mode == KNX_MODE_FRAMES) {
        (void) snprintf(body + n, sizeof(body) - n,
                        " rx_frames=%lu tx_frames=%lu crc_raw=%lu crc_inv=%lu"
                        " crc_bad=%lu acks=%lu ack_other=%lu partial=%lu",
                        (unsigned long)knx_rx_frames,
                        (unsigned long)knx_tx_frames,
                        (unsigned long)knx_crc_raw,
                        (unsigned long)knx_crc_inv,
                        (unsigned long)knx_crc_bad,
                        (unsigned long)knx_acks,
                        (unsigned long)knx_ack_other,
                        (unsigned long)KNX_Test_SessionPartialFrames());
    }

    PortTool_Frame("knx", "%s", body);
}

static void knx_caps(char *out, uint32_t out_len)
{
    char ga[16], src[16];

    knx_ia_text(knx_src, src, sizeof(src));
    if (knx_ga_set == 0) {
        snprintf(ga, sizeof(ga), "none");
    } else {
        knx_ga_text(knx_ga, ga, sizeof(ga));
    }

    snprintf(out, out_len, "mode=%s period=%lu ga=%s src=%s val=%s",
             knx_mode_name(knx_mode), (unsigned long)knx_period_ms, ga, src,
             knx_val_toggle ? "toggle" : (knx_val ? "1" : "0"));
}

/* ga and src are deliberately absent: a limits entry is a range or a set of
 * values, and a formatted address is neither. Spelling one as
 * "ga:main/middle/sub|none" would make the panel read the vertical bar as a
 * two-item choice and offer those words instead of an address field. eth's
 * ip= sets the same precedent - the current value echoed in vals= is what
 * shows the format. */
static void knx_limits(char *out, uint32_t out_len)
{
    snprintf(out, out_len, "mode:loopback|listen|frames period:%lu.. "
             "val:toggle|0|1",
             (unsigned long)PORTTOOL_PERIOD_MIN_MS);
}

porttool_port_t porttool_knx = {
    .name     = "knx",
    .board    = PORTTOOL_BOARD_UPPER,
    .blk      = "C",
    .term     = "C03,C04",
    .terms    = "C03+C04",
    .params   = "mode,period,ga,src,val",
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

// porttool_din.c
//
// Digital In session: reports the selected channels every period, and decodes
// the same eight pins as four quadrature encoders when asked to.
//
// Parameters:
//   ch=1,3,5     which of the eight to report   (default: all)
//   mode=level   report the pins, or decode them (default level)
//   mode=quad    four encoders, A/B on consecutive channels
//   period=200   milliseconds between frames    (default 200, floor 50)
//
// Frame:
//   !din t=48213 v=0x16 mode=level ch1=1 ch3=0 ch5=1
//   !din t=48213 v=0x16 mode=quad g1=124 d1=1 g2=0 d2=0 g3=0 d3=0 g4=0 d4=0 err=0
//
// *** v is the whole eight-bit field in terminal order, bit 0 = DI1, whatever
// *** was selected - so a reading is never ambiguous about which pin it came
// *** from. The ch<n>= pairs are only the selected ones.
//
// *** The encoders are NOT a port of their own, and this is why: the eight
// *** digital inputs and the four encoder A/B pairs are the same eight pins.
// *** Their signal names say so - DIN1_PC6-Enc1a through DIN8_PI6-Enc4b. Two
// *** ports would suggest both can be measured at once, and physically they
// *** cannot.
//
// *** Encoder n is channels 2n-1 (A) and 2n (B), so encoder 1 is DI1/DI2.
// *** g<n> is a signed count and d<n> the last direction seen, +1 or -1, or 0
// *** for an encoder that has not moved since the session started. Restarting
// *** the session is what zeroes them.
//
// *** err counts transitions where both A and B changed between two samples.
// *** That is not a direction anything can infer - it means a step was missed,
// *** either because the encoder turned faster than this loop samples or
// *** because a phase is not connected. A count that climbs while err climbs
// *** with it is not a count anybody should believe, which is why it is
// *** reported rather than quietly resolved one way or the other.

#include "porttool.h"
#include "porttool_cmd.h"
#include "port_din.h"

#include "main.h"
#include <stdio.h>
#include <string.h>

#if PORTTOOL_ENABLE

typedef enum { DIN_MODE_LEVEL = 0, DIN_MODE_QUAD } din_mode_t;

#define DIN_ENC_COUNT (PORT_DIN_COUNT / 2)

static uint32_t   din_mask = (1U << PORT_DIN_COUNT) - 1U;
static din_mode_t din_mode = DIN_MODE_LEVEL;
static uint32_t   din_period_ms = PORTTOOL_PERIOD_DEF_MS;
static uint32_t   din_due_ms;
static int        din_inited;

static int32_t din_enc_count[DIN_ENC_COUNT];
static int8_t   din_enc_dir[DIN_ENC_COUNT];
static uint8_t  din_enc_prev[DIN_ENC_COUNT];
static uint32_t din_enc_err;
static int      din_enc_primed;   /* the first sample sets prev, it is not a step */

/* One step per transition of the A/B pair, indexed prev*4 + now with the pair
 * read as (A<<1)|B. 2 marks a transition where both changed at once: no
 * direction can be read out of it, so it is counted as a miss instead of being
 * guessed at. */
static const int8_t din_quad_step[16] = {
     0,  1, -1,  2,
    -1,  0,  2,  1,
     1,  2,  0, -1,
     2, -1,  1,  0,
};

/* The echo counter rides the RS232 control port for this port, so it says the
 * loop is alive and nothing more. Whether the eight inputs work is decided by
 * v= below, never by this number. */
static porttool_echo_t din_echo;

static void din_echo_got(uint32_t value)
{
    PortTool_EchoGot(&din_echo, value);
}

/* A parameter that is present but malformed is refused rather than ignored -
 * silently keeping the previous channel set would report the wrong pins.
 *
 * Parsed into locals and committed at the end, so a line refused on its second
 * parameter has not already applied its first. */
static int din_apply(const char *args, char *err, uint32_t err_len)
{
    char probe[64];
    uint32_t v;

    uint32_t   want_mask   = din_mask;
    din_mode_t want_mode   = din_mode;
    uint32_t   want_period = din_period_ms;

    if (PortCmd_GetStr(args, "ch", probe, sizeof(probe))) {
        if (!PortCmd_GetMask(args, "ch", PORT_DIN_COUNT, &v) || v == 0U) {
            snprintf(err, err_len, "ch=\"%s\" must be channels 1..%d separated by commas",
                     probe, PORT_DIN_COUNT);
            return 0;
        }
        want_mask = v;
    }

    if (PortCmd_GetStr(args, "mode", probe, sizeof(probe))) {
        if (strcmp(probe, "level") == 0) {
            want_mode = DIN_MODE_LEVEL;
        } else if (strcmp(probe, "quad") == 0) {
            want_mode = DIN_MODE_QUAD;
        } else {
            snprintf(err, err_len, "mode=\"%s\" is not level or quad", probe);
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

    din_mask      = want_mask;
    din_mode      = want_mode;
    din_period_ms = want_period;
    return 1;
}

/* Zeroed by starting the session, never by a parameter: a count that could be
 * cleared mid-run is a count a report cannot be read back from. */
static void din_quad_reset(void)
{
    for (int i = 0; i < DIN_ENC_COUNT; i++) {
        din_enc_count[i] = 0;
        din_enc_dir[i] = 0;
        din_enc_prev[i] = 0;
    }
    din_enc_err = 0;
    din_enc_primed = 0;
}

/* Called on every pass of the super loop, not once per frame: at the frame
 * period a turning encoder would be several steps further on by the time it
 * was next looked at, and every one of those would read as a both-changed
 * miss. */
static void din_quad_sample(uint8_t bits)
{
    for (int i = 0; i < DIN_ENC_COUNT; i++) {
        uint8_t a = (bits >> (2 * i)) & 1U;
        uint8_t b = (bits >> (2 * i + 1)) & 1U;
        uint8_t now = (uint8_t)((a << 1) | b);

        if (!din_enc_primed) {
            din_enc_prev[i] = now;
            continue;
        }
        int8_t step = din_quad_step[(din_enc_prev[i] << 2) | now];
        din_enc_prev[i] = now;
        if (step == 2) {
            din_enc_err++;
        } else if (step != 0) {
            din_enc_count[i] += step;
            din_enc_dir[i] = step;
        }
    }
    din_enc_primed = 1;
}

static int din_start(const char *args, char *err, uint32_t err_len)
{
    if (!din_apply(args, err, err_len)) {
        return 0;
    }
    if (!din_inited) {
        PortDin_Init();
        din_inited = 1;
    }
    din_quad_reset();
    PortTool_EchoReset(&din_echo);
    din_due_ms = HAL_GetTick();
    return 1;
}

static int din_set(const char *args, char *err, uint32_t err_len)
{
    return din_apply(args, err, err_len);
}

static void din_stop(void)
{
    /* The pins are high-impedance inputs; there is nothing to put back. */
}

static void din_tick(uint32_t now_ms)
{
    char body[PORTTOOL_REPLY_MAX];
    uint32_t n;

    uint8_t bits = PortDin_ReadBits();

    /* Before the due check, so it runs at loop rate and not at frame rate. */
    if (din_mode == DIN_MODE_QUAD) {
        din_quad_sample(bits);
    }

    if ((int32_t)(now_ms - din_due_ms) < 0) {
        return;
    }
    din_due_ms = now_ms + din_period_ms;

    PortTool_EchoTick(&din_echo);
    n = PortTool_EchoFields(&din_echo, body, sizeof(body));
    n += (uint32_t)snprintf(body + n, sizeof(body) - n, " v=0x%02X mode=%s",
                            bits,
                            (din_mode == DIN_MODE_QUAD) ? "quad" : "level");

    if (din_mode == DIN_MODE_QUAD) {
        for (int i = 0; i < DIN_ENC_COUNT && n < sizeof(body); i++) {
            int w = snprintf(body + n, sizeof(body) - n, " g%d=%ld d%d=%d",
                             i + 1, (long)din_enc_count[i], i + 1, (int)din_enc_dir[i]);
            if (w < 0) { break; }
            n += (uint32_t)w;
        }
        n += (uint32_t)snprintf(body + n, sizeof(body) - n, " err=%lu",
                                (unsigned long)din_enc_err);
    } else {
        /* *** Only in level mode, and that is a line-length decision, not a
         * *** taste one. With all eight ch<n>= pairs alongside the four
         * *** counters the worst case is 226 bytes against a
         * *** PORTTOOL_LINE_MAX of 192 - ten-digit tick, ten-digit counters,
         * *** eleven-digit counts - and the frame would be silently truncated
         * *** into something the parser cannot read. Dropping them costs
         * *** nothing: v= above is all eight pins, so no reading is lost. */
        for (uint32_t i = 0; i < PORT_DIN_COUNT && n < sizeof(body); i++) {
            if ((din_mask & (1U << i)) == 0U) { continue; }
            int w = snprintf(body + n, sizeof(body) - n, " ch%lu=%d",
                             (unsigned long)(i + 1U), (bits >> i) & 1U);
            if (w < 0) { break; }
            n += (uint32_t)w;
        }
    }

    PortTool_Frame("din", "%s", body);
}

static void din_caps(char *out, uint32_t out_len)
{
    char sel[40];

    PortCmd_FormatMask(din_mask, PORT_DIN_COUNT, sel, sizeof(sel));
    snprintf(out, out_len, "ch=%s mode=%s period=%lu",
             sel,
             (din_mode == DIN_MODE_QUAD) ? "quad" : "level",
             (unsigned long)din_period_ms);
}

static void din_limits(char *out, uint32_t out_len)
{
    snprintf(out, out_len, "ch:1..%u mode:level|quad period:%lu..",
             (unsigned)PORT_DIN_COUNT, (unsigned long)PORTTOOL_PERIOD_MIN_MS);
}

porttool_port_t porttool_din = {
    .name     = "din",
    .board    = PORTTOOL_BOARD_UPPER,
    .blk      = "D",
    .term     = "D02-D09",
    .terms    = NULL,          /* a plain run: DI<n> is terminal D0<n+1> */
    .params   = "ch,mode,period",
    .loop     = PORTTOOL_LOOP_CTRL,
    .channels = PORT_DIN_COUNT,
    .start    = din_start,
    .set      = din_set,
    .stop     = din_stop,
    .tick     = din_tick,
    .echo     = din_echo_got,
    .caps     = din_caps,
    .limits   = din_limits,
};

#endif /* PORTTOOL_ENABLE */

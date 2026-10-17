// porttool_dout.c
//
// Digital Out session: eight 24 V outputs, each with its own duty cycle.
//
// Parameters:
//   ch=1,5        which of the eight to drive          (default: all)
//   mode=hold     hold each duty, or blink             (default hold)
//   mode=quad     quadrature bursts on four pairs, see below
//   cycles=100    quad only: cycles per pair, then stop (default 100)
//   res=8         duty resolution in bits, 1..16, shared by all eight (default 8)
//   duty=255      one duty for every selected channel  (default 0)
//   duty=1:51,5:191  or a duty per channel, raw count 0..2^res-1
//   freq=1000     one PWM frequency for every selected channel (default 1000)
//   freq=1:1000,5:250  or a frequency per channel, in Hz, up to 256000 >> res
//   period=1000   blink half period, ms                (default 1000, floor 50)
//
// Frame:
//   !dout t=48213 seq=1 rx=0 miss=0 mode=hold res=8 tick=256000 ch1=51 ch5=191
//   !dout t=48213 seq=1 rx=0 miss=0 mode=quad cycles=100 sent=400 done=1
//
// duty is a raw count, as in analogWrite, not rescaled when res changes
// (DECISIONS.md 101). duty=0 is off and full scale is on; there is no on=.
//
// ch<n> is the duty only; per-channel frequency is in pt.caps, as the value
// each channel actually landed on (frame length, and duty criteria stay
// independent of frequency). tick= is the shared interrupt rate: fastest
// channel times 2^res.
//
// mode=blink: one cable A03-A10 to D02-D09 makes DI visibly follow DO.
//
// mode=quad feeds the far board's DI encoders (decision 107): DO(2n-1)/DO(2n)
// are A/B of encoder n, all four pairs at 100 Hz, B a quarter period ahead (far
// end counts up), `cycles` cycles then all low. At most one edge per superloop
// pass, so a slow loop stretches the burst rather than drops an edge. ch, duty
// and freq do not apply. See PORTTOOL-FLOW.md B.6.

#include "porttool.h"
#include "porttool_cmd.h"
#include "port_dout.h"

#include "main.h"
#include <stdio.h>
#include <string.h>

#if PORTTOOL_ENABLE

typedef enum { DOUT_MODE_HOLD = 0, DOUT_MODE_BLINK, DOUT_MODE_QUAD } dout_mode_t;

#define DOUT_QUAD_CYCLES_DEF 100U
#define DOUT_QUAD_CYCLES_MAX 10000U
/* 100 Hz is 10 ms a cycle, an edge every 5/2 ms. */
#define DOUT_QUAD_EDGE_NUM_MS 5U
#define DOUT_QUAD_EDGE_DEN    2U

static uint32_t    dout_mask = (1U << PORT_DOUT_COUNT) - 1U;
static dout_mode_t dout_mode = DOUT_MODE_HOLD;
static uint32_t    dout_res  = PORT_DOUT_RES_DEF;
static uint32_t    dout_duty[PORT_DOUT_COUNT];
/* Not zero: caps is read before any session starts, and 0 Hz is below the
 * floor the panel would send back. */
_Static_assert(PORT_DOUT_COUNT == 8, "the frequency defaults below list eight");
static uint32_t    dout_freq_hz[PORT_DOUT_COUNT] = {
    PORT_DOUT_FREQ_DEF_HZ, PORT_DOUT_FREQ_DEF_HZ,
    PORT_DOUT_FREQ_DEF_HZ, PORT_DOUT_FREQ_DEF_HZ,
    PORT_DOUT_FREQ_DEF_HZ, PORT_DOUT_FREQ_DEF_HZ,
    PORT_DOUT_FREQ_DEF_HZ, PORT_DOUT_FREQ_DEF_HZ,
};
static uint32_t    dout_period_ms = 1000U;
static uint32_t    dout_due_ms;
static int         dout_dark;         /* blink phase: driving 0 rather than duty */
static int         dout_inited;
static uint32_t    dout_cycles = DOUT_QUAD_CYCLES_DEF;
static uint32_t    dout_sent;         /* quad: edges sent per pair */
static uint32_t    dout_quad_t0;      /* quad: when the burst started */
static int         dout_report_now;   /* quad: the burst just ended, frame now */

/* loop=ctrl: says the control loop is alive, nothing about the outputs
 * (DECISIONS.md 9). */
static porttool_echo_t dout_echo;

static void dout_echo_got(uint32_t value)
{
    PortTool_EchoGot(&dout_echo, value);
}

/* Edge k of a cycle as (A,B): 00, 01, 11, 10 - the order the DI decoder
 * counts up (porttool_din.c, din_quad_step). */
static void dout_quad_drive(uint32_t edge)
{
    static const uint8_t ab[4] = { 0x0U, 0x1U, 0x3U, 0x2U };   /* bit1 = A, bit0 = B */
    const uint32_t full = (1UL << dout_res) - 1UL;
    uint8_t now = ab[edge % 4U];
    for (int pair = 0; pair < PORT_DOUT_COUNT / 2; pair++) {
        PortDout_SetDuty(2 * pair + 1, (now & 0x2U) ? full : 0U);
        PortDout_SetDuty(2 * pair + 2, (now & 0x1U) ? full : 0U);
    }
}

/* Pushes the wanted duties at the hardware; unselected channels are never
 * touched. In the dark half of a blink the selected ones go to zero. */
static void dout_drive(void)
{
    if (dout_mode == DOUT_MODE_QUAD) {
        dout_quad_drive(dout_sent);
        return;
    }
    for (int i = 0; i < PORT_DOUT_COUNT; i++) {
        if ((dout_mask & (1U << i)) == 0U) { continue; }
        uint32_t want = dout_dark ? 0U : dout_duty[i];
        PortDout_SetDuty(i + 1, want);
    }
}

/* Committed only at the end, so a refused line applies nothing. */
static int dout_apply(const char *args, char *err, uint32_t err_len)
{
    char probe[96];
    uint32_t v;

    uint32_t    want_mask   = dout_mask;
    dout_mode_t want_mode   = dout_mode;
    uint32_t    want_period = dout_period_ms;
    uint32_t    want_res    = dout_res;
    uint32_t    want_duty[PORT_DOUT_COUNT];
    uint32_t    want_freq[PORT_DOUT_COUNT];

    for (int i = 0; i < PORT_DOUT_COUNT; i++) {
        want_duty[i] = dout_duty[i];
        want_freq[i] = dout_freq_hz[i];
    }

    if (PortCmd_GetStr(args, "ch", probe, sizeof(probe))) {
        if (!PortCmd_GetMask(args, "ch", PORT_DOUT_COUNT, &v) || v == 0U) {
            snprintf(err, err_len, "ch=\"%s\" must be channels 1..%d separated by commas",
                     probe, PORT_DOUT_COUNT);
            return 0;
        }
        want_mask = v;
    }

    if (PortCmd_GetStr(args, "mode", probe, sizeof(probe))) {
        if (strcmp(probe, "hold") == 0) {
            want_mode = DOUT_MODE_HOLD;
        } else if (strcmp(probe, "blink") == 0) {
            want_mode = DOUT_MODE_BLINK;
        } else if (strcmp(probe, "quad") == 0) {
            want_mode = DOUT_MODE_QUAD;
        } else {
            snprintf(err, err_len, "mode=\"%s\" is not hold, blink or quad", probe);
            return 0;
        }
    }

    uint32_t want_cycles = dout_cycles;
    if (PortCmd_GetStr(args, "cycles", probe, sizeof(probe))) {
        if (!PortCmd_GetU32(args, "cycles", &v) || v < 1U || v > DOUT_QUAD_CYCLES_MAX) {
            snprintf(err, err_len, "cycles=\"%s\" must be 1..%lu",
                     probe, (unsigned long)DOUT_QUAD_CYCLES_MAX);
            return 0;
        }
        want_cycles = v;
    }

    /* res before duty and freq: both are checked against it. */
    if (PortCmd_GetStr(args, "res", probe, sizeof(probe))) {
        if (!PortCmd_GetU32(args, "res", &v) ||
            v < PORT_DOUT_RES_MIN || v > PORT_DOUT_RES_MAX) {
            snprintf(err, err_len, "res=\"%s\" must be %lu..%lu bits",
                     probe, (unsigned long)PORT_DOUT_RES_MIN,
                     (unsigned long)PORT_DOUT_RES_MAX);
            return 0;
        }
        want_res = v;
    }
    const uint32_t full     = (1UL << want_res) - 1UL;
    const uint32_t freq_max = PORT_DOUT_FREQ_MAX_HZ(want_res);

    if (PortCmd_GetStr(args, "duty", probe, sizeof(probe))) {
        if (strchr(probe, ':') != NULL) {
            if (!PortCmd_GetPairs(args, "duty", PORT_DOUT_COUNT, full,
                                  want_duty, NULL)) {
                snprintf(err, err_len,
                         "duty=\"%s\" must be like 1:51,5:191 - outputs 1..%d, "
                         "0..%lu at res=%lu, each output named at most once",
                         probe, PORT_DOUT_COUNT,
                         (unsigned long)full, (unsigned long)want_res);
                return 0;
            }
        } else {
            if (!PortCmd_GetU32(args, "duty", &v) || v > full) {
                snprintf(err, err_len, "duty=\"%s\" must be 0..%lu at res=%lu",
                         probe, (unsigned long)full, (unsigned long)want_res);
                return 0;
            }
            for (int i = 0; i < PORT_DOUT_COUNT; i++) {
                want_duty[i] = v;
            }
        }
    }

    /* Same two shapes as duty (port_dout.h). */
    if (PortCmd_GetStr(args, "freq", probe, sizeof(probe))) {
        if (strchr(probe, ':') != NULL) {
            if (!PortCmd_GetPairs(args, "freq", PORT_DOUT_COUNT,
                                  freq_max, want_freq, NULL)) {
                snprintf(err, err_len,
                         "freq=\"%s\" must be like 1:1000,5:250 - outputs 1..%d, "
                         "%lu..%lu Hz at res=%lu, each output named at most once",
                         probe, PORT_DOUT_COUNT,
                         (unsigned long)PORT_DOUT_FREQ_MIN_HZ,
                         (unsigned long)freq_max, (unsigned long)want_res);
                return 0;
            }
            for (int i = 0; i < PORT_DOUT_COUNT; i++) {
                if (want_freq[i] < PORT_DOUT_FREQ_MIN_HZ) {
                    snprintf(err, err_len,
                             "freq=\"%s\" names output %d below the %lu Hz floor",
                             probe, i + 1, (unsigned long)PORT_DOUT_FREQ_MIN_HZ);
                    return 0;
                }
            }
        } else {
            if (!PortCmd_GetU32(args, "freq", &v) ||
                v < PORT_DOUT_FREQ_MIN_HZ || v > freq_max) {
                snprintf(err, err_len, "freq=\"%s\" must be %lu..%lu Hz at res=%lu",
                         probe, (unsigned long)PORT_DOUT_FREQ_MIN_HZ,
                         (unsigned long)freq_max, (unsigned long)want_res);
                return 0;
            }
            for (int i = 0; i < PORT_DOUT_COUNT; i++) {
                want_freq[i] = v;
            }
        }
    }

    if (PortCmd_GetStr(args, "period", probe, sizeof(probe))) {
        if (!PortCmd_GetU32(args, "period", &v)) {
            snprintf(err, err_len, "period=\"%s\" is not a number", probe);
            return 0;
        }
        want_period = PortTool_ClampPeriod(v);
    }

    /* Values kept from before must fit a new res too: duty is not rescaled,
     * and the interrupt rate covers all eight frequencies, selected or not. */
    for (int i = 0; i < PORT_DOUT_COUNT; i++) {
        if (want_duty[i] > full) {
            snprintf(err, err_len, "output %d keeps duty %lu, over %lu at res=%lu"
                     " - give duty= with res=",
                     i + 1, (unsigned long)want_duty[i],
                     (unsigned long)full, (unsigned long)want_res);
            return 0;
        }
        if (want_freq[i] > freq_max) {
            snprintf(err, err_len, "output %d keeps %lu Hz, over %lu Hz at res=%lu"
                     " - give freq= with res=",
                     i + 1, (unsigned long)want_freq[i],
                     (unsigned long)freq_max, (unsigned long)want_res);
            return 0;
        }
    }

    dout_mask      = want_mask;
    dout_cycles    = want_cycles;
    dout_res       = want_res;
    dout_mode      = want_mode;
    dout_period_ms = want_period;
    for (int i = 0; i < PORT_DOUT_COUNT; i++) {
        dout_duty[i] = want_duty[i];
        dout_freq_hz[i] = want_freq[i];
    }
    return 1;
}

static int dout_drive_res(char *err, uint32_t err_len)
{
    if (!PortDout_SetRes(dout_res)) {
        snprintf(err, err_len, "the PWM timer would not take res=%lu",
                 (unsigned long)dout_res);
        return 0;
    }
    return 1;
}

/* Pushes res and all eight frequencies (the interrupt rate is shared). Lowering
 * res goes before the frequencies, raising it after, so no pair is ever over
 * the rate cap. */
static int dout_drive_freq(char *err, uint32_t err_len)
{
    int res_first = dout_res < PortDout_Res();
    if (res_first && !dout_drive_res(err, err_len)) {
        return 0;
    }
    for (int i = 0; i < PORT_DOUT_COUNT; i++) {
        if (!PortDout_SetFreq(i + 1, dout_freq_hz[i])) {
            snprintf(err, err_len, "the PWM timer would not take %lu Hz on output %d",
                     (unsigned long)dout_freq_hz[i], i + 1);
            return 0;
        }
    }
    if (!res_first && !dout_drive_res(err, err_len)) {
        return 0;
    }
    return 1;
}

static int dout_start(const char *args, char *err, uint32_t err_len)
{
    if (!dout_apply(args, err, err_len)) {
        return 0;
    }
    if (!PortDout_Init()) {
        snprintf(err, err_len, "the PWM timer would not start");
        return 0;
    }
    if (!dout_drive_freq(err, err_len)) {
        return 0;
    }
    dout_inited = 1;
    dout_dark = 0;
    dout_sent = 0;
    dout_report_now = 0;
    dout_quad_t0 = HAL_GetTick();
    dout_drive();
    PortTool_EchoReset(&dout_echo);
    dout_due_ms = HAL_GetTick();
    return 1;
}

static int dout_set(const char *args, char *err, uint32_t err_len)
{
    if (!dout_apply(args, err, err_len)) {
        return 0;
    }
    if (!dout_drive_freq(err, err_len)) {
        return 0;
    }
    dout_drive();
    return 1;
}

static void dout_stop(void)
{
    /* 24 V outputs: never left energised. */
    PortDout_Stop();
    dout_inited = 0;
    dout_dark = 0;
}

/* At most one edge per pass, run before the frame check so it goes at loop
 * rate; the last edge pulls all eight low and reports at once. */
static void dout_quad_step(uint32_t now_ms)
{
    const uint32_t total = 4U * dout_cycles;
    if (dout_sent >= total) {
        return;
    }
    uint32_t due = dout_quad_t0 + ((dout_sent + 1U) * DOUT_QUAD_EDGE_NUM_MS) / DOUT_QUAD_EDGE_DEN;
    if ((int32_t)(now_ms - due) < 0) {
        return;
    }
    dout_sent++;
    if (dout_sent < total) {
        dout_quad_drive(dout_sent);
        return;
    }
    for (int i = 0; i < PORT_DOUT_COUNT; i++) {
        PortDout_SetDuty(i + 1, 0U);
    }
    dout_report_now = 1;
}

static void dout_tick(uint32_t now_ms)
{
    char body[PORTTOOL_REPLY_MAX];
    uint32_t n;

    if (dout_mode == DOUT_MODE_QUAD) {
        dout_quad_step(now_ms);
    }
    if (dout_report_now) {
        dout_report_now = 0;
    } else if ((int32_t)(now_ms - dout_due_ms) < 0) {
        return;
    }
    dout_due_ms = now_ms + dout_period_ms;

    if (dout_mode == DOUT_MODE_BLINK) {
        dout_dark = !dout_dark;
        dout_drive();
    }

    PortTool_EchoTick(&dout_echo);
    n = PortTool_EchoFields(&dout_echo, body, sizeof(body));
    if (dout_mode == DOUT_MODE_QUAD) {
        (void)snprintf(body + n, sizeof(body) - n, " mode=quad cycles=%lu sent=%lu done=%d",
                       (unsigned long)dout_cycles, (unsigned long)dout_sent,
                       dout_sent >= 4U * dout_cycles);
        PortTool_Frame("dout", "%s", body);
        return;
    }
    n += (uint32_t)snprintf(body + n, sizeof(body) - n, " mode=%s res=%lu tick=%lu",
                            (dout_mode == DOUT_MODE_BLINK) ? "blink" : "hold",
                            (unsigned long)dout_res,
                            (unsigned long)PortDout_TickHz());

    for (int i = 0; i < PORT_DOUT_COUNT && n < sizeof(body); i++) {
        if ((dout_mask & (1U << i)) == 0U) { continue; }
        /* What is on the pin now (0 in the dark half of a blink). */
        uint32_t now = dout_dark ? 0U : dout_duty[i];
        int w = snprintf(body + n, sizeof(body) - n, " ch%d=%lu",
                         i + 1, (unsigned long)now);
        if (w < 0) { break; }
        n += (uint32_t)w;
    }

    PortTool_Frame("dout", "%s", body);
}

static void dout_caps(char *out, uint32_t out_len)
{
    char sel[40];
    char duty[80];
    char freq[96];

    PortCmd_FormatMask(dout_mask, PORT_DOUT_COUNT, sel, sizeof(sel));
    PortCmd_FormatPairs(dout_duty, dout_mask, PORT_DOUT_COUNT, duty, sizeof(duty));
    /* What each channel landed on, not what was asked for. */
    uint32_t actual[PORT_DOUT_COUNT];
    for (int i = 0; i < PORT_DOUT_COUNT; i++) {
        actual[i] = dout_inited ? PortDout_ActualFreqHz(i + 1) : dout_freq_hz[i];
    }
    /* All eight, not just the selected: the panel fills every channel from
     * this line, and a missing one would become freq=0, below the floor. */
    PortCmd_FormatPairs(actual, (1U << PORT_DOUT_COUNT) - 1U,
                        PORT_DOUT_COUNT, freq, sizeof(freq));
    snprintf(out, out_len, "ch=%s mode=%s res=%lu duty=%s freq=%s period=%lu cycles=%lu",
             sel,
             (dout_mode == DOUT_MODE_QUAD) ? "quad" :
             (dout_mode == DOUT_MODE_BLINK) ? "blink" : "hold",
             (unsigned long)dout_res, duty, freq,
             (unsigned long)dout_period_ms, (unsigned long)dout_cycles);
}

static void dout_limits(char *out, uint32_t out_len)
{
    /* Widest over every res; dout_apply checks the bounds at the current res. */
    snprintf(out, out_len,
             "ch:1..%u mode:hold|blink|quad res:%lu..%lu duty:0..%lu freq:%lu..%lu period:%lu.. cycles:1..%lu",
             (unsigned)PORT_DOUT_COUNT,
             (unsigned long)PORT_DOUT_RES_MIN, (unsigned long)PORT_DOUT_RES_MAX,
             (unsigned long)((1UL << PORT_DOUT_RES_MAX) - 1UL),
             (unsigned long)PORT_DOUT_FREQ_MIN_HZ,
             (unsigned long)PORT_DOUT_FREQ_MAX_HZ(PORT_DOUT_RES_MIN),
             (unsigned long)PORTTOOL_PERIOD_MIN_MS,
             (unsigned long)DOUT_QUAD_CYCLES_MAX);
}

porttool_port_t porttool_dout = {
    .name     = "dout",
    .board    = PORTTOOL_BOARD_LOWER,
    .blk      = "A",
    .term     = "A03-A10",
    .terms    = NULL,          /* a plain run: DO<n> is terminal A0<n+2> */
    .params   = "ch,mode,res,duty,freq,period,cycles",
    .loop     = PORTTOOL_LOOP_CTRL,
    .channels = PORT_DOUT_COUNT,
    .start    = dout_start,
    .set      = dout_set,
    .stop     = dout_stop,
    .tick     = dout_tick,
    .echo     = dout_echo_got,
    .caps     = dout_caps,
    .limits   = dout_limits,
};

#endif /* PORTTOOL_ENABLE */

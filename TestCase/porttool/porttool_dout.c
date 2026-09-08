// porttool_dout.c
//
// Digital Out session: eight 24 V outputs, each with its own duty cycle.
//
// Parameters:
//   ch=1,5        which of the eight to drive          (default: all)
//   mode=hold     hold each duty, or blink             (default hold)
//   duty=100      one duty for every selected channel  (default 0)
//   duty=1:20,5:75  or a duty per channel, in percent
//   freq=1000     PWM frequency in Hz                  (default 1000)
//   period=1000   blink half period, ms                (default 1000, floor 50)
//
// Frame:
//   !dout t=48213 seq=1 rx=0 miss=0 mode=hold freq=1000 ch1=20 ch5=75
//
// *** There is deliberately no on= parameter. duty=0 is off and duty=100 is
// *** on, so a separate switch would be a second way to say the same thing and
// *** a state where the two could disagree.
//
// *** mode=blink exists for one specific job: DO puts out 24 V and DI tolerates
// *** 24 V, so one eight-way cable from A03-A10 to D02-D09 covers sixteen
// *** channels at once. Blinking is what makes DI visibly follow; a held level
// *** proves far less.

#include "porttool.h"
#include "porttool_cmd.h"
#include "port_dout.h"

#include "main.h"
#include <stdio.h>
#include <string.h>

#if PORTTOOL_ENABLE

typedef enum { DOUT_MODE_HOLD = 0, DOUT_MODE_BLINK } dout_mode_t;

static uint32_t    dout_mask = (1U << PORT_DOUT_COUNT) - 1U;
static dout_mode_t dout_mode = DOUT_MODE_HOLD;
static uint32_t    dout_duty[PORT_DOUT_COUNT];
static uint32_t    dout_freq_hz = PORT_DOUT_FREQ_DEF_HZ;
static uint32_t    dout_period_ms = 1000U;
static uint32_t    dout_due_ms;
static int         dout_dark;         /* blink phase: driving 0 rather than duty */
static int         dout_inited;

/* Rides the RS232 control port, so it says the loop is alive and nothing about
 * the outputs. What the outputs did is decided by a meter on the terminal, or
 * by the DI session on the other end of that eight-way cable. */
static porttool_echo_t dout_echo;

static void dout_echo_got(uint32_t value)
{
    PortTool_EchoGot(&dout_echo, value);
}

/* Pushes the wanted duties at the hardware. In the dark half of a blink every
 * selected channel goes to zero; unselected channels are never touched, so a
 * second session on the other channels is left alone. */
static void dout_drive(void)
{
    for (int i = 0; i < PORT_DOUT_COUNT; i++) {
        if ((dout_mask & (1U << i)) == 0U) { continue; }
        uint32_t want = dout_dark ? 0U : dout_duty[i];
        PortDout_SetDuty(i + 1, want);
    }
}

/* Parsed into locals and committed at the end: a line refused on its third
 * parameter must not have applied its first two. */
static int dout_apply(const char *args, char *err, uint32_t err_len)
{
    char probe[96];
    uint32_t v;

    uint32_t    want_mask   = dout_mask;
    dout_mode_t want_mode   = dout_mode;
    uint32_t    want_freq   = dout_freq_hz;
    uint32_t    want_period = dout_period_ms;
    uint32_t    want_duty[PORT_DOUT_COUNT];

    for (int i = 0; i < PORT_DOUT_COUNT; i++) {
        want_duty[i] = dout_duty[i];
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
        } else {
            snprintf(err, err_len, "mode=\"%s\" is not hold or blink", probe);
            return 0;
        }
    }

    if (PortCmd_GetStr(args, "duty", probe, sizeof(probe))) {
        if (strchr(probe, ':') != NULL) {
            if (!PortCmd_GetPairs(args, "duty", PORT_DOUT_COUNT, 100U,
                                  want_duty, NULL)) {
                snprintf(err, err_len,
                         "duty=\"%s\" must be like 1:20,5:75 - outputs 1..%d, "
                         "percent 0..100, each output named at most once",
                         probe, PORT_DOUT_COUNT);
                return 0;
            }
        } else {
            if (!PortCmd_GetU32(args, "duty", &v) || v > 100U) {
                snprintf(err, err_len, "duty=\"%s\" is not a percentage 0..100", probe);
                return 0;
            }
            for (int i = 0; i < PORT_DOUT_COUNT; i++) {
                want_duty[i] = v;
            }
        }
    }

    if (PortCmd_GetStr(args, "freq", probe, sizeof(probe))) {
        if (!PortCmd_GetU32(args, "freq", &v) ||
            v < PORT_DOUT_FREQ_MIN_HZ || v > PORT_DOUT_FREQ_MAX_HZ) {
            snprintf(err, err_len, "freq=\"%s\" must be %lu..%lu Hz",
                     probe, (unsigned long)PORT_DOUT_FREQ_MIN_HZ,
                     (unsigned long)PORT_DOUT_FREQ_MAX_HZ);
            return 0;
        }
        want_freq = v;
    }

    if (PortCmd_GetStr(args, "period", probe, sizeof(probe))) {
        if (!PortCmd_GetU32(args, "period", &v)) {
            snprintf(err, err_len, "period=\"%s\" is not a number", probe);
            return 0;
        }
        want_period = PortTool_ClampPeriod(v);
    }

    dout_mask      = want_mask;
    dout_mode      = want_mode;
    dout_freq_hz   = want_freq;
    dout_period_ms = want_period;
    for (int i = 0; i < PORT_DOUT_COUNT; i++) {
        dout_duty[i] = want_duty[i];
    }
    return 1;
}

static int dout_start(const char *args, char *err, uint32_t err_len)
{
    if (!dout_apply(args, err, err_len)) {
        return 0;
    }
    if (!PortDout_Init(dout_freq_hz)) {
        snprintf(err, err_len, "the PWM timer would not start");
        return 0;
    }
    dout_inited = 1;
    dout_dark = 0;
    dout_drive();
    PortTool_EchoReset(&dout_echo);
    dout_due_ms = HAL_GetTick();
    return 1;
}

static int dout_set(const char *args, char *err, uint32_t err_len)
{
    uint32_t was_freq = dout_freq_hz;

    if (!dout_apply(args, err, err_len)) {
        return 0;
    }
    if (dout_freq_hz != was_freq && !PortDout_Init(dout_freq_hz)) {
        snprintf(err, err_len, "the PWM timer would not take %lu Hz",
                 (unsigned long)dout_freq_hz);
        return 0;
    }
    dout_drive();
    return 1;
}

static void dout_stop(void)
{
    /* These are the only 24 V outputs on the board, so leaving one energised
     * because a session ended is not an option. */
    PortDout_Stop();
    dout_inited = 0;
    dout_dark = 0;
}

static void dout_tick(uint32_t now_ms)
{
    char body[PORTTOOL_REPLY_MAX];
    uint32_t n;

    if ((int32_t)(now_ms - dout_due_ms) < 0) {
        return;
    }
    dout_due_ms = now_ms + dout_period_ms;

    if (dout_mode == DOUT_MODE_BLINK) {
        dout_dark = !dout_dark;
        dout_drive();
    }

    PortTool_EchoTick(&dout_echo);
    n = PortTool_EchoFields(&dout_echo, body, sizeof(body));
    n += (uint32_t)snprintf(body + n, sizeof(body) - n, " mode=%s freq=%lu",
                            (dout_mode == DOUT_MODE_BLINK) ? "blink" : "hold",
                            (unsigned long)PortDout_ActualFreqHz());

    for (int i = 0; i < PORT_DOUT_COUNT && n < sizeof(body); i++) {
        if ((dout_mask & (1U << i)) == 0U) { continue; }
        /* What is on the pin right now, not what was asked for: in the dark
         * half of a blink those differ, and the frame has to say which. */
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

    PortCmd_FormatMask(dout_mask, PORT_DOUT_COUNT, sel, sizeof(sel));
    PortCmd_FormatPairs(dout_duty, dout_mask, PORT_DOUT_COUNT, duty, sizeof(duty));
    snprintf(out, out_len, "ch=%s mode=%s duty=%s freq=%lu period=%lu",
             sel,
             (dout_mode == DOUT_MODE_BLINK) ? "blink" : "hold",
             duty,
             (unsigned long)dout_freq_hz,
             (unsigned long)dout_period_ms);
}

static void dout_limits(char *out, uint32_t out_len)
{
    snprintf(out, out_len, "ch:1..%u mode:hold|blink duty:0..100 freq:%lu..%lu period:%lu..",
             (unsigned)PORT_DOUT_COUNT,
             (unsigned long)PORT_DOUT_FREQ_MIN_HZ, (unsigned long)PORT_DOUT_FREQ_MAX_HZ,
             (unsigned long)PORTTOOL_PERIOD_MIN_MS);
}

porttool_port_t porttool_dout = {
    .name     = "dout",
    .board    = PORTTOOL_BOARD_LOWER,
    .blk      = "A",
    .term     = "A03-A10",
    .terms    = NULL,          /* a plain run: DO<n> is terminal A0<n+2> */
    .params   = "ch,mode,duty,freq,period",
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

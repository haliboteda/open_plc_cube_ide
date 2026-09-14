// porttool_aout.c
//
// Analog Out session: the two analog output terminals, each a DAC channel
// feeding an XTR111 voltage-to-current converter.
//
// Parameters:
//   ch=1,2        which of the two               (default: both)
//   mv=500        one setting for every selected channel  (default 0)
//   mv=1:500,2:1500  or a setting per channel, in millivolts at the DAC
//   period=1000   milliseconds between frames    (default 1000, floor 50)
//
// Frame:
//   !aout t=48213 seq=1 rx=0 miss=0 ch1=500/488/4883 ef1=0 ch2=1500/1465/14648 ef2=0
//
// *** ch<n> is asked/quantised/microamps: what was requested, what the DAC can
// *** actually produce with its step size, and the current that should appear
// *** at the terminal. Reporting the request alone would hide a rounding of
// *** several millivolts; reporting only the quantised value would make the
// *** panel look like it ignored what was typed.
//
// *** The microamp figure is what the terminal SHOULD read, computed from the
// *** XTR111 set resistor. It is not a measurement - nothing on this board can
// *** read that current back. A meter in the loop is the only judge, which is
// *** the whole reason this port exists.
//
// *** It also only holds while jumpers JP3/JP4 are open. Closed, they feed
// *** VREF into the XTR111 summing node and the output becomes a 4-20 mA
// *** live-zero, about 4.85 mA with the DAC at zero. That is a soldered choice
// *** this firmware cannot read back, so the number here assumes open.

#include "porttool.h"
#include "porttool_cmd.h"
#include "port_dac.h"
#include "port_vref.h"

#include "main.h"
#include <stdio.h>
#include <string.h>

#if PORTTOOL_ENABLE

/* The DAC is fed from VREF+, so a setting can never exceed it. */
#define AOUT_MV_MAX 3300U

static uint32_t aout_mask = (1U << PORT_AOUT_COUNT) - 1U;
static uint32_t aout_mv[PORT_AOUT_COUNT];
static uint32_t aout_period_ms = 1000U;
static uint32_t aout_due_ms;
static int      aout_inited;

static porttool_echo_t aout_echo;

static void aout_echo_got(uint32_t value)
{
    PortTool_EchoGot(&aout_echo, value);
}

static void aout_drive(void)
{
    for (int i = 1; i <= PORT_AOUT_COUNT; i++) {
        if ((aout_mask & (1U << (i - 1))) == 0U) { continue; }
        (void)PortDac_SetMv(i, aout_mv[i - 1]);
    }
}

static int aout_apply(const char *args, char *err, uint32_t err_len)
{
    char probe[64];
    uint32_t v;

    uint32_t want_mask   = aout_mask;
    uint32_t want_period = aout_period_ms;
    uint32_t want_mv[PORT_AOUT_COUNT];

    for (int i = 0; i < PORT_AOUT_COUNT; i++) {
        want_mv[i] = aout_mv[i];
    }

    if (PortCmd_GetStr(args, "ch", probe, sizeof(probe))) {
        if (!PortCmd_GetMask(args, "ch", PORT_AOUT_COUNT, &v) || v == 0U) {
            snprintf(err, err_len, "ch=\"%s\" must be channels 1..%d separated by commas",
                     probe, PORT_AOUT_COUNT);
            return 0;
        }
        want_mask = v;
    }

    if (PortCmd_GetStr(args, "mv", probe, sizeof(probe))) {
        if (strchr(probe, ':') != NULL) {
            if (!PortCmd_GetPairs(args, "mv", PORT_AOUT_COUNT, AOUT_MV_MAX,
                                  want_mv, NULL)) {
                snprintf(err, err_len,
                         "mv=\"%s\" must be like 1:500,2:1500 - outputs 1..%d, "
                         "0..%lu mV, each output named at most once",
                         probe, PORT_AOUT_COUNT, (unsigned long)AOUT_MV_MAX);
                return 0;
            }
        } else {
            if (!PortCmd_GetU32(args, "mv", &v) || v > AOUT_MV_MAX) {
                snprintf(err, err_len, "mv=\"%s\" must be 0..%lu millivolts",
                         probe, (unsigned long)AOUT_MV_MAX);
                return 0;
            }
            for (int i = 0; i < PORT_AOUT_COUNT; i++) {
                want_mv[i] = v;
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

    aout_mask      = want_mask;
    aout_period_ms = want_period;
    for (int i = 0; i < PORT_AOUT_COUNT; i++) {
        aout_mv[i] = want_mv[i];
    }
    return 1;
}

static int aout_start(const char *args, char *err, uint32_t err_len)
{
    if (!aout_apply(args, err, err_len)) {
        return 0;
    }
    if (!aout_inited) {
        /* The DAC's full scale is VREF+, which on this board comes from
         * VREFBUF. Without it the output voltage is whatever VREF+ happens to
         * float at, so the current at the terminal would be neither what was
         * asked for nor anything this session could report honestly. */
        if (!PortVref_Enable()) {
            snprintf(err, err_len,
                     "VREFBUF would not come up, and the DAC's full scale is "
                     "VREF+ - the output would not be the value asked for");
            return 0;
        }
        if (!PortDac_Init()) {
            snprintf(err, err_len, "the DAC would not start");
            return 0;
        }
        aout_inited = 1;
    }
    aout_drive();
    PortTool_EchoReset(&aout_echo);
    aout_due_ms = HAL_GetTick();
    return 1;
}

static int aout_set(const char *args, char *err, uint32_t err_len)
{
    if (!aout_apply(args, err, err_len)) {
        return 0;
    }
    aout_drive();
    return 1;
}

static void aout_stop(void)
{
    /* These drive current into whatever is wired to the terminal, so they go
     * back to zero rather than being left wherever the session ended. */
    for (int i = 1; i <= PORT_AOUT_COUNT; i++) {
        aout_mv[i - 1] = 0U;
        (void)PortDac_SetMv(i, 0U);
    }
}

static void aout_tick(uint32_t now_ms)
{
    char body[PORTTOOL_REPLY_MAX];
    uint32_t n;

    if ((int32_t)(now_ms - aout_due_ms) < 0) {
        return;
    }
    aout_due_ms = now_ms + aout_period_ms;

    PortTool_EchoTick(&aout_echo);
    n = PortTool_EchoFields(&aout_echo, body, sizeof(body));

    for (int i = 1; i <= PORT_AOUT_COUNT && n < sizeof(body); i++) {
        if ((aout_mask & (1U << (i - 1))) == 0U) { continue; }
        uint32_t asked = aout_mv[i - 1];
        /* ef<n> is the XTR111's fault flag as the pin reads, nothing more -
         * which level means fault is not settled anywhere in Hardware/, so
         * the plan judges it once somebody has (DECISIONS.md 22). It is here
         * because it is the ONLY output-fault signal on this board that
         * reaches the MCU at all: the high-side switches' fault bits do not,
         * so without this an ageing run has nothing to watch. */
        int w = snprintf(body + n, sizeof(body) - n, " ch%d=%lu/%lu/%lu ef%d=%d",
                         i, (unsigned long)asked,
                         (unsigned long)PortDac_QuantisedMv(asked),
                         (unsigned long)PortDac_ExpectedMicroamps(asked),
                         i, PortDac_FaultLevel(i));
        if (w < 0) { break; }
        n += (uint32_t)w;
    }

    PortTool_Frame("aout", "%s", body);
}

static void aout_caps(char *out, uint32_t out_len)
{
    char sel[16];
    char mv[48];

    PortCmd_FormatMask(aout_mask, PORT_AOUT_COUNT, sel, sizeof(sel));
    PortCmd_FormatPairs(aout_mv, aout_mask, PORT_AOUT_COUNT, mv, sizeof(mv));
    snprintf(out, out_len, "ch=%s mv=%s period=%lu",
             sel, mv, (unsigned long)aout_period_ms);
}

static void aout_limits(char *out, uint32_t out_len)
{
    snprintf(out, out_len, "ch:1..%u mv:0..%lu period:%lu..",
             (unsigned)PORT_AOUT_COUNT, (unsigned long)AOUT_MV_MAX,
             (unsigned long)PORTTOOL_PERIOD_MIN_MS);
}

porttool_port_t porttool_aout = {
    .name     = "aout",
    .board    = PORTTOOL_BOARD_UPPER,
    .blk      = "D",
    .term     = "D14,D15",
    .terms    = "D14,D15",
    .params   = "ch,mv,period",
    .loop     = PORTTOOL_LOOP_CTRL,
    .channels = PORT_AOUT_COUNT,
    .start    = aout_start,
    .set      = aout_set,
    .stop     = aout_stop,
    .tick     = aout_tick,
    .echo     = aout_echo_got,
    .caps     = aout_caps,
    .limits   = aout_limits,
};

#endif /* PORTTOOL_ENABLE */

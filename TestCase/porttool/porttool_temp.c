// porttool_temp.c
//
// Board temperature session: the two LM50 sensors, one on the short-circuit
// protection and one on the high-side FETs.
//
// Parameters:
//   ch=1,2       which of the two             (default: both)
//   period=1000  milliseconds between frames  (default 1000, floor 50)
//
// Frame:
//   !temp t=48213 seq=1 rx=0 miss=0 vdda=3287 ok=1 ch1=738/238 ch2=751/251
//
// *** ch<n> is millivolts/deci-degrees. Deci- rather than whole degrees so a
// *** slow drift is visible without a floating-point number crossing the
// *** serial line.
//
// *** These two are the only temperatures on the board and they are not
// *** terminals - nothing external is connected. What they are worth watching
// *** for is the Digital Out drivers heating up under load, which is why they
// *** are their own session rather than a field on dout: they matter most while
// *** something else is being driven hard.

#include "porttool.h"
#include "porttool_cmd.h"
#include "port_adc.h"
#include "port_vref.h"

#include "main.h"
#include <stdio.h>

#if PORTTOOL_ENABLE

static uint32_t temp_mask = (1U << PORT_TEMP_COUNT) - 1U;
static uint32_t temp_period_ms = 1000U;
static uint32_t temp_due_ms;
static int      temp_inited;

static porttool_echo_t temp_echo;

static void temp_echo_got(uint32_t value)
{
    PortTool_EchoGot(&temp_echo, value);
}

static int temp_apply(const char *args, char *err, uint32_t err_len)
{
    char probe[64];
    uint32_t v;

    uint32_t want_mask   = temp_mask;
    uint32_t want_period = temp_period_ms;

    if (PortCmd_GetStr(args, "ch", probe, sizeof(probe))) {
        if (!PortCmd_GetMask(args, "ch", PORT_TEMP_COUNT, &v) || v == 0U) {
            snprintf(err, err_len, "ch=\"%s\" must be channels 1..%d separated by commas",
                     probe, PORT_TEMP_COUNT);
            return 0;
        }
        want_mask = v;
    }

    if (PortCmd_GetStr(args, "period", probe, sizeof(probe))) {
        if (!PortCmd_GetU32(args, "period", &v)) {
            snprintf(err, err_len, "period=\"%s\" is not a number", probe);
            return 0;
        }
        want_period = PortTool_ClampPeriod(v);
    }

    temp_mask      = want_mask;
    temp_period_ms = want_period;
    return 1;
}

static int temp_start(const char *args, char *err, uint32_t err_len)
{
    if (!temp_apply(args, err, err_len)) {
        return 0;
    }
    if (!temp_inited) {
        /* Same gate as the analog inputs: no reference means no meaning. */
        if (!PortVref_Enable()) {
            snprintf(err, err_len,
                     "VREFBUF would not come up, so there is no reference to "
                     "measure against and every reading would be meaningless");
            return 0;
        }
        if (!PortAdc_Init()) {
            snprintf(err, err_len, "the ADC would not start");
            return 0;
        }
        temp_inited = 1;
    }
    PortTool_EchoReset(&temp_echo);
    temp_due_ms = HAL_GetTick();
    return 1;
}

static int temp_set(const char *args, char *err, uint32_t err_len)
{
    return temp_apply(args, err, err_len);
}

static void temp_stop(void)
{
    /* Nothing is driven. */
}

static void temp_tick(uint32_t now_ms)
{
    char body[PORTTOOL_REPLY_MAX];
    uint32_t n;

    if ((int32_t)(now_ms - temp_due_ms) < 0) {
        return;
    }
    temp_due_ms = now_ms + temp_period_ms;

    PortTool_EchoTick(&temp_echo);
    n = PortTool_EchoFields(&temp_echo, body, sizeof(body));
    n += (uint32_t)snprintf(body + n, sizeof(body) - n, " vdda=%lu ok=%d",
                            (unsigned long)PortAdc_VddaMv(),
                            PortAdc_VddaTrusted());

    for (int i = 1; i <= PORT_TEMP_COUNT && n < sizeof(body); i++) {
        if ((temp_mask & (1U << (i - 1))) == 0U) { continue; }
        uint32_t mv = 0;
        int32_t decic = 0;
        int w;
        if (PortAdc_ReadTemp(i, &mv, &decic)) {
            w = snprintf(body + n, sizeof(body) - n, " ch%d=%lu/%ld",
                         i, (unsigned long)mv, (long)decic);
        } else {
            w = snprintf(body + n, sizeof(body) - n, " ch%d=fail", i);
        }
        if (w < 0) { break; }
        n += (uint32_t)w;
    }

    PortTool_Frame("temp", "%s", body);
}

static void temp_caps(char *out, uint32_t out_len)
{
    char sel[16];

    PortCmd_FormatMask(temp_mask, PORT_TEMP_COUNT, sel, sizeof(sel));
    snprintf(out, out_len, "ch=%s period=%lu", sel, (unsigned long)temp_period_ms);
}

static void temp_limits(char *out, uint32_t out_len)
{
    snprintf(out, out_len, "ch:1..%u period:%lu..",
             (unsigned)PORT_TEMP_COUNT, (unsigned long)PORTTOOL_PERIOD_MIN_MS);
}

porttool_port_t porttool_temp = {
    .name     = "temp",
    .board    = PORTTOOL_BOARD_LOWER,
    .blk      = "-",
    .term     = "-",          /* on-board sensors, no terminal */
    /* Which sensor is which, since neither has a terminal to name it by.
     * English like every other string the firmware puts on the wire. */
    .terms    = "SC-protect,HS-switch",
    .params   = "ch,period",
    .loop     = PORTTOOL_LOOP_CTRL,
    .channels = PORT_TEMP_COUNT,
    .start    = temp_start,
    .set      = temp_set,
    .stop     = temp_stop,
    .tick     = temp_tick,
    .echo     = temp_echo_got,
    .caps     = temp_caps,
    .limits   = temp_limits,
};

#endif /* PORTTOOL_ENABLE */

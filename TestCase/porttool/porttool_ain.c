// porttool_ain.c
//
// Analog In session: the two analog input terminals, plus the reference the
// readings are taken against.
//
// Parameters:
//   ch=1,2       which of the two          (default: both)
//   period=500   milliseconds between frames  (default 500, floor 50)
//
// Frame:
//   !ain t=48213 seq=1 rx=0 miss=0 vdda=3287 ok=1 ch1=21850/2071 ch2=530/50
//
// ch<n> is raw/millivolts at the MCU pin; the PC scales it to the terminal,
// since the range is a jumper this firmware cannot read.
//
// ok=0 means VDDA was not plausible and every reading is meaningless; they are
// still reported, for diagnosis (PORTTOOL-FLOW.md A.5).

#include "porttool.h"
#include "porttool_cmd.h"
#include "port_adc.h"
#include "port_vref.h"

#include "main.h"
#include <stdio.h>

#if PORTTOOL_ENABLE

static uint32_t ain_mask = (1U << PORT_AIN_COUNT) - 1U;
static uint32_t ain_period_ms = 500U;
static uint32_t ain_due_ms;
static int      ain_inited;

static porttool_echo_t ain_echo;

static void ain_echo_got(uint32_t value)
{
    PortTool_EchoGot(&ain_echo, value);
}

static int ain_apply(const char *args, char *err, uint32_t err_len)
{
    char probe[64];
    uint32_t v;

    uint32_t want_mask   = ain_mask;
    uint32_t want_period = ain_period_ms;

    if (PortCmd_GetStr(args, "ch", probe, sizeof(probe))) {
        if (!PortCmd_GetMask(args, "ch", PORT_AIN_COUNT, &v) || v == 0U) {
            snprintf(err, err_len, "ch=\"%s\" must be channels 1..%d separated by commas",
                     probe, PORT_AIN_COUNT);
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

    ain_mask      = want_mask;
    ain_period_ms = want_period;
    return 1;
}

static int ain_start(const char *args, char *err, uint32_t err_len)
{
    if (!ain_apply(args, err, err_len)) {
        return 0;
    }

    if (!ain_inited) {
        /* VREFBUF first: the board has no reference chip
         * (HARDWARE-FACTS.md 模拟量). */
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
        ain_inited = 1;
    }

    PortTool_EchoReset(&ain_echo);
    ain_due_ms = HAL_GetTick();
    return 1;
}

static int ain_set(const char *args, char *err, uint32_t err_len)
{
    return ain_apply(args, err, err_len);
}

static void ain_stop(void)
{
    /* Nothing is driven, so nothing to put back. */
}

static void ain_tick(uint32_t now_ms)
{
    char body[PORTTOOL_REPLY_MAX];
    uint32_t n;

    if ((int32_t)(now_ms - ain_due_ms) < 0) {
        return;
    }
    ain_due_ms = now_ms + ain_period_ms;

    PortTool_EchoTick(&ain_echo);
    n = PortTool_EchoFields(&ain_echo, body, sizeof(body));
    n += (uint32_t)snprintf(body + n, sizeof(body) - n, " vdda=%lu ok=%d",
                            (unsigned long)PortAdc_VddaMv(),
                            PortAdc_VddaTrusted());

    for (int i = 1; i <= PORT_AIN_COUNT && n < sizeof(body); i++) {
        if ((ain_mask & (1U << (i - 1))) == 0U) { continue; }
        uint32_t raw = 0, mv = 0;
        int w;
        if (PortAdc_ReadAin(i, &raw, &mv)) {
            w = snprintf(body + n, sizeof(body) - n, " ch%d=%lu/%lu",
                         i, (unsigned long)raw, (unsigned long)mv);
        } else {
            /* Not left out: a missing channel would read as "not selected". */
            w = snprintf(body + n, sizeof(body) - n, " ch%d=fail", i);
        }
        if (w < 0) { break; }
        n += (uint32_t)w;
    }

    PortTool_Frame("ain", "%s", body);
}

static void ain_caps(char *out, uint32_t out_len)
{
    char sel[16];

    PortCmd_FormatMask(ain_mask, PORT_AIN_COUNT, sel, sizeof(sel));
    snprintf(out, out_len, "ch=%s period=%lu", sel, (unsigned long)ain_period_ms);
}

static void ain_limits(char *out, uint32_t out_len)
{
    snprintf(out, out_len, "ch:1..%u period:%lu..",
             (unsigned)PORT_AIN_COUNT, (unsigned long)PORTTOOL_PERIOD_MIN_MS);
}

porttool_port_t porttool_ain = {
    .name     = "ain",
    .board    = PORTTOOL_BOARD_UPPER,
    .blk      = "D",
    .term     = "D12,D13",
    /* Per channel: the two terminals are different ranges. */
    .terms    = "D12,D13",
    .params   = "ch,period",
    .loop     = PORTTOOL_LOOP_CTRL,
    .channels = PORT_AIN_COUNT,
    .start    = ain_start,
    .set      = ain_set,
    .stop     = ain_stop,
    .tick     = ain_tick,
    .echo     = ain_echo_got,
    .caps     = ain_caps,
    .limits   = ain_limits,
};

#endif /* PORTTOOL_ENABLE */

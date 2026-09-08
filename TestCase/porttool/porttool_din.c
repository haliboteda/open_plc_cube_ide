// porttool_din.c
//
// Digital In session: reports the selected channels every period.
//
// Parameters:
//   ch=1,3,5     which of the eight to report   (default: all)
//   period=200   milliseconds between frames    (default 200, floor 50)
//
// Frame:
//   !din t=48213 v=0x16 ch1=1 ch3=0 ch5=1
//
// *** v is the whole eight-bit field in terminal order, bit 0 = DI1, whatever
// *** was selected - so a reading is never ambiguous about which pin it came
// *** from. The ch<n>= pairs are only the selected ones.

#include "porttool.h"
#include "porttool_cmd.h"
#include "port_din.h"

#include "main.h"
#include <stdio.h>
#include <string.h>

#if PORTTOOL_ENABLE

static uint32_t din_mask = (1U << PORT_DIN_COUNT) - 1U;
static uint32_t din_period_ms = PORTTOOL_PERIOD_DEF_MS;
static uint32_t din_due_ms;
static int      din_inited;

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

    uint32_t want_mask   = din_mask;
    uint32_t want_period = din_period_ms;

    if (PortCmd_GetStr(args, "ch", probe, sizeof(probe))) {
        if (!PortCmd_GetMask(args, "ch", PORT_DIN_COUNT, &v) || v == 0U) {
            snprintf(err, err_len, "ch=\"%s\" must be channels 1..%d separated by commas",
                     probe, PORT_DIN_COUNT);
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

    din_mask      = want_mask;
    din_period_ms = want_period;
    return 1;
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

    if ((int32_t)(now_ms - din_due_ms) < 0) {
        return;
    }
    din_due_ms = now_ms + din_period_ms;

    uint8_t bits = PortDin_ReadBits();

    PortTool_EchoTick(&din_echo);
    n = PortTool_EchoFields(&din_echo, body, sizeof(body));
    n += (uint32_t)snprintf(body + n, sizeof(body) - n, " v=0x%02X", bits);
    for (uint32_t i = 0; i < PORT_DIN_COUNT && n < sizeof(body); i++) {
        if ((din_mask & (1U << i)) == 0U) { continue; }
        int w = snprintf(body + n, sizeof(body) - n, " ch%lu=%d",
                         (unsigned long)(i + 1U), (bits >> i) & 1U);
        if (w < 0) { break; }
        n += (uint32_t)w;
    }

    PortTool_Frame("din", "%s", body);
}

static void din_caps(char *out, uint32_t out_len)
{
    char sel[40];

    PortCmd_FormatMask(din_mask, PORT_DIN_COUNT, sel, sizeof(sel));
    snprintf(out, out_len, "ch=%s period=%lu",
             sel, (unsigned long)din_period_ms);
}

static void din_limits(char *out, uint32_t out_len)
{
    snprintf(out, out_len, "ch:1..%u period:%lu..",
             (unsigned)PORT_DIN_COUNT, (unsigned long)PORTTOOL_PERIOD_MIN_MS);
}

porttool_port_t porttool_din = {
    .name     = "din",
    .board    = PORTTOOL_BOARD_UPPER,
    .blk      = "D",
    .term     = "D02-D09",
    .terms    = NULL,          /* a plain run: DI<n> is terminal D0<n+1> */
    .params   = "ch,period",
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

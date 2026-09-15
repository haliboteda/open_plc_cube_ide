// porttool_rs232.c
//
// RS232 session: automatic send, automatic receive, on the terminal the tool
// itself is talking over.
//
// Parameters:
//   period=3000  milliseconds between frames  (default 3000, floor 50)
//
// Frame:
//   !rs232 t=48213 seq=1 rx=0 miss=0 rxlines=42
//
// *** loop=self, not loop=ctrl. The echo round trip runs over the control
// *** port, but for THIS port that is the point: the bytes physically cross
// *** terminals C05/C06, the MAX3221 transceiver and PC10/PC11 in both
// *** directions. For every other port the counter only says the control port
// *** is alive; here it is the verdict.
//
// *** This replaces an earlier belief - written down in PORTTOOL-FLOW.md B.3.3
// *** and wrong - that the tool occupying this channel meant the channel could
// *** not be tested. Occupying it is what tests it.

#include "porttool.h"
#include "porttool_cmd.h"

#include "main.h"
#include <stdio.h>

#if PORTTOOL_ENABLE

static uint32_t rs232_period_ms = 3000U;
static uint32_t rs232_due_ms;

static porttool_echo_t rs232_echo;

static void rs232_echo_got(uint32_t value)
{
    PortTool_EchoGot(&rs232_echo, value);
}

static int rs232_apply(const char *args, char *err, uint32_t err_len)
{
    char probe[32];
    uint32_t v;

    if (PortCmd_GetStr(args, "period", probe, sizeof(probe))) {
        if (!PortCmd_GetU32(args, "period", &v)) {
            snprintf(err, err_len, "period=\"%s\" is not a number", probe);
            return 0;
        }
        rs232_period_ms = PortTool_ClampPeriod(v);
    }
    return 1;
}

static int rs232_start(const char *args, char *err, uint32_t err_len)
{
    if (!rs232_apply(args, err, err_len)) {
        return 0;
    }
    /* Nothing to initialise: UART4 and the MAX3221 have been up since main.c,
     * which is the only reason any of this is readable at all. */
    PortTool_EchoReset(&rs232_echo);
    rs232_due_ms = HAL_GetTick();
    return 1;
}

static int rs232_set(const char *args, char *err, uint32_t err_len)
{
    return rs232_apply(args, err, err_len);
}

static void rs232_stop(void)
{
    /* The console stays up - stopping this session must not take the tool's
     * own channel down with it. */
}

static void rs232_tick(uint32_t now_ms)
{
    char body[PORTTOOL_REPLY_MAX];
    uint32_t n;

    if ((int32_t)(now_ms - rs232_due_ms) < 0) {
        return;
    }
    rs232_due_ms = now_ms + rs232_period_ms;

    PortTool_EchoTick(&rs232_echo);
    n = PortTool_EchoFields(&rs232_echo, body, sizeof(body));
    n += (uint32_t)snprintf(body + n, sizeof(body) - n, " rxlines=%lu",
                            (unsigned long)PortTool_LinesReceived());

    PortTool_Frame("rs232", "%s", body);
}

static void rs232_caps(char *out, uint32_t out_len)
{
    snprintf(out, out_len, "period=%lu", (unsigned long)rs232_period_ms);
}

static void rs232_limits(char *out, uint32_t out_len)
{
    snprintf(out, out_len, "period:%lu..", (unsigned long)PORTTOOL_PERIOD_MIN_MS);
}

porttool_port_t porttool_rs232 = {
    .name     = "rs232",
    .board    = PORTTOOL_BOARD_UPPER,
    .blk      = "C",
    .term     = "C05,C06",
    .terms    = "C05+C06",     /* TxD and RxD are one channel, not two */
    .params   = "period",
    .loop     = PORTTOOL_LOOP_SELF,
    .channels = 1,
    .start    = rs232_start,
    .set      = rs232_set,
    .stop     = rs232_stop,
    .tick     = rs232_tick,
    .echo     = rs232_echo_got,
    .caps     = rs232_caps,
    .limits   = rs232_limits,
};

#endif /* PORTTOOL_ENABLE */

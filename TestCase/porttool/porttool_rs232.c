// porttool_rs232.c
//
// RS232 session: automatic send, automatic receive, on the terminal the tool
// itself is talking over, at a chosen rate (decision 104).
//
// Parameters:
//   period=3000  milliseconds between frames  (default 3000, floor 50)
//   baud=115200  rate of the whole control line while the session runs;
//                switched after the reply, back on stop or after 3 s of
//                silence (PortTool_LineRequest, PORTTOOL-FLOW.md B.7)
//
// Frame:
//   !rs232 t=48213 seq=1 rx=0 miss=0 rxlines=42 baud=19200
//
// loop=self: the echo crosses C05/C06, the MAX3221 and PC10/PC11 both ways, so
// here the counter is the verdict (DECISIONS.md 16).

#include "porttool.h"
#include "porttool_cmd.h"

#include "main.h"
#include <stdio.h>

#if PORTTOOL_ENABLE

static uint32_t rs232_period_ms = 3000U;
static uint32_t rs232_due_ms;
static uint32_t rs232_baud = PORTTOOL_LINE_DEFAULT_BAUD;

/* 230400 is within the MAX3221's ~250 kbps; whether it works also depends on
 * the USB-232 adapter (decision 104). */
static int rs232_baud_ok(uint32_t b)
{
    return b == 9600U || b == 19200U || b == 38400U || b == 57600U ||
           b == 115200U || b == 230400U;
}

static porttool_echo_t rs232_echo;

static void rs232_echo_got(uint32_t value)
{
    PortTool_EchoGot(&rs232_echo, value);
}

static int rs232_apply(const char *args, char *err, uint32_t err_len)
{
    char probe[32];
    uint32_t v;

    if (PortCmd_GetStr(args, "baud", probe, sizeof(probe))) {
        if (!PortCmd_GetU32(args, "baud", &v) || !rs232_baud_ok(v)) {
            snprintf(err, err_len,
                     "baud=\"%s\" must be one of 9600 19200 38400 57600 115200 230400",
                     probe);
            return 0;
        }
        rs232_baud = v;
    }
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
    rs232_baud = PORTTOOL_LINE_DEFAULT_BAUD;   /* a start without baud= runs at the default */
    if (!rs232_apply(args, err, err_len)) {
        return 0;
    }
    /* Nothing to initialise: UART4 is the control port, up since main.c. */
    PortTool_EchoReset(&rs232_echo);
    rs232_due_ms = HAL_GetTick();
    PortTool_LineRequest(rs232_baud);
    return 1;
}

static int rs232_set(const char *args, char *err, uint32_t err_len)
{
    if (!rs232_apply(args, err, err_len)) {
        return 0;
    }
    PortTool_LineRequest(rs232_baud);
    return 1;
}

static void rs232_stop(void)
{
    /* The console stays up, back at its default rate. */
    rs232_baud = PORTTOOL_LINE_DEFAULT_BAUD;
    PortTool_LineRequest(rs232_baud);
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
    n += (uint32_t)snprintf(body + n, sizeof(body) - n, " rxlines=%lu baud=%lu",
                            (unsigned long)PortTool_LinesReceived(),
                            (unsigned long)PortTool_LineBaud());

    PortTool_Frame("rs232", "%s", body);
}

static void rs232_caps(char *out, uint32_t out_len)
{
    snprintf(out, out_len, "baud=%lu period=%lu", (unsigned long)rs232_baud,
             (unsigned long)rs232_period_ms);
}

static void rs232_limits(char *out, uint32_t out_len)
{
    snprintf(out, out_len, "baud:9600|19200|38400|57600|115200|230400 period:%lu..",
             (unsigned long)PORTTOOL_PERIOD_MIN_MS);
}

porttool_port_t porttool_rs232 = {
    .name     = "rs232",
    .board    = PORTTOOL_BOARD_UPPER,
    .blk      = "C",
    .term     = "C05,C06",
    .terms    = "C05+C06",     /* TxD and RxD are one channel, not two */
    .params   = "baud,period",
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

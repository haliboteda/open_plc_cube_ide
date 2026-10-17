// porttool_rs485.c
//
// RS485 session: the board sends a number on the A/B pair and takes the reply
// back off that same pair.
//
// Parameters:
//   baud=115200  line rate                      (default 115200)
//   period=3000  milliseconds between sends      (default 3000, floor 50)
//
// Frame (on the CONTROL port, not on the link):
//   !rs485 t=48213 seq=3 rx=2 miss=0 baud=115200 rxbytes=12 junk=0
//
// loop=link: the echo travels over the pair, so the counter is its verdict and
// pt.echo is refused (DECISIONS.md 9). Status still goes out on the control
// port.
//
// Half duplex on one net: PD4 drives /RE and DE together, so the board cannot
// hear itself. A second device must answer (e.g. the panel's link responder).

#include "porttool.h"
#include "porttool_cmd.h"
#include "port_rs485.h"

#include "main.h"
#include <stdio.h>

#if PORTTOOL_ENABLE

/* Long enough for "PT 4294967295\n". */
#define RS485_LINE_MAX 24U

/* How long the tick keeps reading after sending, waiting for the reply. */
#define RS485_REPLY_WINDOW_MS 20U

/* A second bound, in iterations, for the host harness whose clock does not
 * move; on a board the time bound ends the loop first. */
#define RS485_REPLY_MAX_SPINS 200000U

static uint32_t rs485_baud = PORT_RS485_DEFAULT_BAUD;
static uint32_t rs485_period_ms = 3000U;
static uint32_t rs485_due_ms;
static int      rs485_inited;

/* What has arrived on the pair since the last complete reply. */
static char     rs485_in[RS485_LINE_MAX];
static uint32_t rs485_in_len;
static uint32_t rs485_rxbytes;
static uint32_t rs485_junk;      /* replies that were not a plain number */

static porttool_echo_t rs485_echo;

static int rs485_apply(const char *args, char *err, uint32_t err_len)
{
    char probe[32];
    uint32_t v;

    uint32_t want_baud   = rs485_baud;
    uint32_t want_period = rs485_period_ms;

    if (PortCmd_GetStr(args, "baud", probe, sizeof(probe))) {
        /* The usual RS485 rates only. */
        if (!PortCmd_GetU32(args, "baud", &v) ||
            (v != 9600U && v != 19200U && v != 38400U &&
             v != 57600U && v != 115200U)) {
            snprintf(err, err_len,
                     "baud=\"%s\" must be one of 9600 19200 38400 57600 115200",
                     probe);
            return 0;
        }
        want_baud = v;
    }

    if (PortCmd_GetStr(args, "period", probe, sizeof(probe))) {
        if (!PortCmd_GetU32(args, "period", &v)) {
            snprintf(err, err_len, "period=\"%s\" is not a number", probe);
            return 0;
        }
        want_period = PortTool_ClampPeriod(v);
    }

    rs485_baud      = want_baud;
    rs485_period_ms = want_period;
    return 1;
}

static int rs485_start(const char *args, char *err, uint32_t err_len)
{
    if (!rs485_apply(args, err, err_len)) {
        return 0;
    }
    /* Every start, not only the first: pt.run rs485.pins re-muxes PD4/PD5 to
     * GPIO, and a stale mux looks exactly like a wiring fault. */
    if (!PortRs485_Init(rs485_baud)) {
        snprintf(err, err_len, "USART2 would not start at %lu baud",
                 (unsigned long)rs485_baud);
        return 0;
    }
    rs485_inited = 1;

    rs485_in_len  = 0U;
    rs485_rxbytes = 0U;
    rs485_junk    = 0U;
    PortRs485_ResetOverruns();
    PortTool_EchoReset(&rs485_echo);
    rs485_due_ms = HAL_GetTick();
    return 1;
}

static int rs485_set(const char *args, char *err, uint32_t err_len)
{
    uint32_t was_baud = rs485_baud;

    if (!rs485_apply(args, err, err_len)) {
        return 0;
    }
    if (rs485_baud != was_baud && !PortRs485_Init(rs485_baud)) {
        snprintf(err, err_len, "USART2 would not take %lu baud",
                 (unsigned long)rs485_baud);
        return 0;
    }
    return 1;
}

static void rs485_stop(void)
{
    /* Back to receive, so the driver does not hold the pair. */
    if (rs485_inited) {
        PortRs485_DriveEnable(0);
    }
}

/* Takes whatever is waiting on the pair; called every superloop pass. */
static void rs485_drain(void)
{
    uint8_t b;

    while (PortRs485_RecvByte(&b)) {
        rs485_rxbytes++;

        if (b == '\r' || b == '\n') {
            if (rs485_in_len == 0U) {
                continue;
            }
            rs485_in[rs485_in_len] = '\0';

            uint32_t v = 0;
            int digits = 0;
            for (const char *p = rs485_in; *p != '\0'; p++) {
                if (*p < '0' || *p > '9') {
                    digits = -1;
                    break;
                }
                v = v * 10U + (uint32_t)(*p - '0');
                digits++;
            }
            if (digits > 0) {
                PortTool_EchoGot(&rs485_echo, v);
            } else {
                /* Something is on the pair: a different fault from silence. */
                rs485_junk++;
            }
            rs485_in_len = 0U;
        } else if (rs485_in_len + 1U < sizeof(rs485_in)) {
            rs485_in[rs485_in_len++] = (char)b;
        } else {
            rs485_in_len = 0U;
            rs485_junk++;
        }
    }
}

static void rs485_tick(uint32_t now_ms)
{
    char body[PORTTOOL_REPLY_MAX];
    char out[RS485_LINE_MAX];
    uint32_t n;

    rs485_drain();

    if ((int32_t)(now_ms - rs485_due_ms) < 0) {
        return;
    }
    rs485_due_ms = now_ms + rs485_period_ms;

    PortTool_EchoTick(&rs485_echo);

    /* Sent on the pair under test; the other end sends it straight back. */
    n = (uint32_t)snprintf(out, sizeof(out), "%lu\n",
                           (unsigned long)rs485_echo.seq);
    (void)PortRs485_Send((const uint8_t *)out, (uint16_t)n);

    /* Take the reply here in a bounded spin, so a frame's printf (over 10 ms)
     * cannot sit inside the receive window and overrun the USART. */
    {
        uint32_t until = HAL_GetTick() + RS485_REPLY_WINDOW_MS;
        uint32_t spins = RS485_REPLY_MAX_SPINS;

        while ((spins-- > 0u) && ((int32_t)(HAL_GetTick() - until) < 0)) {
            rs485_drain();
            if (rs485_echo.have) {
                break;      /* the number came back - no need to spin on */
            }
        }
    }

    n = PortTool_EchoFields(&rs485_echo, body, sizeof(body));
    n += (uint32_t)snprintf(body + n, sizeof(body) - n,
                            " baud=%lu rxbytes=%lu junk=%lu overrun=%lu",
                            (unsigned long)rs485_baud,
                            (unsigned long)rs485_rxbytes,
                            (unsigned long)rs485_junk,
                            (unsigned long)PortRs485_Overruns());

    PortTool_Frame("rs485", "%s", body);
}

static void rs485_caps(char *out, uint32_t out_len)
{
    snprintf(out, out_len, "baud=%lu period=%lu",
             (unsigned long)rs485_baud, (unsigned long)rs485_period_ms);
}

static void rs485_limits(char *out, uint32_t out_len)
{
    snprintf(out, out_len, "baud:9600|19200|38400|57600|115200 period:%lu..",
             (unsigned long)PORTTOOL_PERIOD_MIN_MS);
}

porttool_port_t porttool_rs485 = {
    .name     = "rs485",
    .board    = PORTTOOL_BOARD_UPPER,
    .blk      = "C",
    .term     = "C10,C11",
    .terms    = "C10+C11",     /* A and B are one differential pair */
    .params   = "baud,period",
    .loop     = PORTTOOL_LOOP_LINK,
    .channels = 1,
    .start    = rs485_start,
    .set      = rs485_set,
    .stop     = rs485_stop,
    .tick     = rs485_tick,
    /* No echo hook: the reply comes off the pair (DECISIONS.md 9). */
    .echo     = NULL,
    .caps     = rs485_caps,
    .limits   = rs485_limits,
};

#endif /* PORTTOOL_ENABLE */

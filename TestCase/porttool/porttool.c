// porttool.c
//
// Port test tool entry and command loop - see porttool.h.

#include "porttool.h"
#include "porttool_cmd.h"
#include "porttool_calw.h"  /* pt.calw; also where PORTTOOL_CALIB_BASE comes from */
#include "porttool_aodiag.h" /* TEMPORARY: pt.aodiag, see that header */
#include "porttool_run.h"
#include "port_led.h"

#include "main.h"
#include "usart.h"          /* huart4: the RS232 log port is also the command port */
#include "calib_area.h"     /* pt.cal: where the calibration area is, how long */

#include <stdarg.h>
#include <stdio.h>
#include <string.h>

#if PORTTOOL_ENABLE

#define PORTTOOL_VERSION "0.18.1"

/* Each porttool_<name>.c defines one of these. */
extern porttool_port_t porttool_ain;
extern porttool_port_t porttool_can;
extern porttool_port_t porttool_knx;
extern porttool_port_t porttool_aout;
extern porttool_port_t porttool_din;
extern porttool_port_t porttool_dout;
extern porttool_port_t porttool_eth;
extern porttool_port_t porttool_relay;
extern porttool_port_t porttool_rs232;
extern porttool_port_t porttool_rs485;
extern porttool_port_t porttool_sd;
extern porttool_port_t porttool_sdram;
extern porttool_port_t porttool_temp;
extern porttool_port_t porttool_usb;

static porttool_port_t *const ports[] = {
    &porttool_din,
    &porttool_dout,
    &porttool_relay,
    &porttool_ain,
    &porttool_aout,
    &porttool_temp,
    &porttool_rs232,
    &porttool_rs485,
    &porttool_can,
    &porttool_knx,
    &porttool_eth,
    &porttool_usb,
    &porttool_sd,      /* detect switch only; deep checks are pt.run targets */
    &porttool_sdram,   /* retention; probe/sweep/crc are pt.run targets */
};
#define PORT_COUNT (sizeof(ports) / sizeof(ports[0]))

static char line_buf[PORTTOOL_LINE_MAX];
static uint32_t line_len;

/* The command port is received on interrupt into a ring: printf blocks in
 * HAL_UART_Transmit, and a polled receive loses the bytes that arrive meanwhile.
 * Single writer (ISR: head) and single reader (poll_command: tail), so no
 * critical section is needed. 512 holds more than one PORTTOOL_LINE_MAX line. */
#define RX_RING_SIZE 512U
static volatile uint8_t  rx_ring[RX_RING_SIZE];
static volatile uint16_t rx_head;   /* ISR writes */
static volatile uint16_t rx_tail;   /* poll_command reads */
static volatile uint32_t rx_dropped;/* ring was full - the reader fell behind */
static volatile uint8_t  rx_err_seen; /* an error arrived; the reader resets its line */

static uint32_t lines_in;    /* PortTool_LinesReceived */
static uint32_t rx_errors;   /* PortTool_RxErrors */

/* The deadman (DECISIONS.md 37). Only pt.hold renews it, so a PC merely echoing
 * frames does not keep outputs live. On lapse the board releases every output
 * and lights the indicator itself, since the PC is gone. */
#define PORTTOOL_HOLD_MAX_MS 600000U

static uint32_t hold_span_ms;   /* what the PC last asked for, echoed back */
static uint32_t hold_until_ms;
static int      hold_armed;

/* The control line's rate - see PortTool_LineRequest in porttool.h. */
static uint32_t line_baud = PORTTOOL_LINE_DEFAULT_BAUD;
static uint32_t line_next;          /* 0 = nothing to apply after this reply */
static uint32_t line_heard_ms;      /* last byte in */
static int      line_settled = 1;   /* a byte has arrived at the current rate */

void PortTool_LineRequest(uint32_t baud)
{
    line_next = (baud == line_baud) ? 0U : baud;
}

uint32_t PortTool_LineBaud(void)
{
    return line_baud;
}

/* " line=<baud>" when this command's reply announces a switch, else "". */
static const char *line_suffix(void)
{
    static char buf[24];

    if (line_next == 0U) {
        return "";
    }
    snprintf(buf, sizeof(buf), " line=%lu", (unsigned long)line_next);
    return buf;
}

/* No reply is in flight here: printf's HAL_UART_Transmit returns only after
 * TC, so the announcing reply's last byte is already on the wire. */
static void line_set(uint32_t baud)
{
    huart4.Init.BaudRate = baud;
    (void)HAL_UART_Init(&huart4);
    __HAL_UART_ENABLE_IT(&huart4, UART_IT_RXNE);
    line_baud = baud;
}

static void line_heard(uint32_t now_ms)
{
    line_heard_ms = now_ms;
    line_settled  = 1;
}

/* Applies a requested rate once the reply announcing it has gone out. Frames
 * wait for the PC's first byte at the new rate: sent earlier they are noise. */
static void line_apply(uint32_t now_ms)
{
    if (line_next == 0U) {
        return;
    }
    line_set(line_next);
    line_next     = 0U;
    line_settled  = (line_baud == PORTTOOL_LINE_DEFAULT_BAUD);
    line_heard_ms = now_ms;
}

/* A PC that died mid-test would leave the board on a rate nobody knows. */
static void line_check(uint32_t now_ms)
{
    if (line_baud == PORTTOOL_LINE_DEFAULT_BAUD || line_next != 0U) {
        return;
    }
    if ((uint32_t)(now_ms - line_heard_ms) < PORTTOOL_LINE_SILENCE_MS) {
        return;
    }
    line_set(PORTTOOL_LINE_DEFAULT_BAUD);
    line_settled = 1;
    printf("!line t=%lu baud=%lu reverted=1\r\n", (unsigned long)now_ms,
           (unsigned long)line_baud);
}

uint32_t PortTool_LinesReceived(void)
{
    return lines_in;
}

uint32_t PortTool_RxErrors(void)
{
    return rx_errors;
}

uint32_t PortTool_ClampPeriod(uint32_t requested_ms)
{
    return (requested_ms < PORTTOOL_PERIOD_MIN_MS) ? PORTTOOL_PERIOD_MIN_MS
                                                   : requested_ms;
}

void PortTool_Frame(const char *port, const char *fmt, ...)
{
    char body[PORTTOOL_REPLY_MAX];
    va_list ap;

    va_start(ap, fmt);
    (void)vsnprintf(body, sizeof(body), fmt, ap);
    va_end(ap);

    if (!line_settled) {
        return;
    }
    printf("!%s t=%lu %s\r\n", port, (unsigned long)HAL_GetTick(), body);
}

static porttool_port_t *find_port(const char *name)
{
    for (uint32_t i = 0; i < PORT_COUNT; i++) {
        if (strcmp(ports[i]->name, name) == 0) {
            return ports[i];
        }
    }
    return NULL;
}

int PortTool_IsSessionPort(const char *name)
{
    return find_port(name) != NULL;
}

int PortTool_PortRunning(const char *name)
{
    const porttool_port_t *p = find_port(name);

    return (p != NULL) && p->running;
}

static void stop_all(void)
{
    for (uint32_t i = 0; i < PORT_COUNT; i++) {
        if (ports[i]->running && ports[i]->stop != NULL) {
            ports[i]->stop();
        }
        ports[i]->running = 0;
    }
}

void PortTool_EchoReset(porttool_echo_t *e)
{
    e->seq  = 1U;   /* 0 would be indistinguishable from "nothing yet" */
    e->rx   = 0U;
    e->miss = 0U;
    e->got     = 0U;
    e->have    = 0;
    e->started = 0;
}

void PortTool_EchoGot(porttool_echo_t *e, uint32_t value)
{
    e->got  = value;
    e->have = 1;
}

void PortTool_EchoTick(porttool_echo_t *e)
{
    if (!e->started) {
        /* The first frame has not gone out yet, so there is nothing for the PC
         * to have answered. Counting this period as a miss would make every
         * healthy loop start at miss=1. */
        e->started = 1;
        return;
    }

    if (e->have && e->got == e->seq) {
        e->rx   = e->got;
        e->seq  = e->rx + 1U;
        e->miss = 0U;
    } else {
        /* Either nothing came back, or what did come back answers some earlier
         * frame. Both mean this period did not close, so seq stays put: a
         * counter that kept climbing would look alive with the loop broken. */
        e->miss++;
    }
    e->have = 0;
}

uint32_t PortTool_EchoFields(const porttool_echo_t *e, char *out, uint32_t out_len)
{
    int w = snprintf(out, out_len, "seq=%lu rx=%lu miss=%lu",
                     (unsigned long)e->seq, (unsigned long)e->rx,
                     (unsigned long)e->miss);
    return (w < 0) ? 0U : (uint32_t)w;
}

void PortTool_EchoScan(porttool_echo_t *e, porttool_numscan_t *s,
                       const uint8_t *data, uint32_t len)
{
    for (uint32_t i = 0; i < len; i++) {
        uint8_t c = data[i];
        if ((c >= '0') && (c <= '9')) {
            s->acc = (s->acc * 10U) + (uint32_t)(c - '0');
            s->in_number = 1;
        } else if (s->in_number) {
            PortTool_EchoGot(e, s->acc);
            s->acc = 0;
            s->in_number = 0;
        }
    }
}

uint32_t PortTool_Crc32Update(uint32_t crc, const uint8_t *p, uint32_t n)
{
    for (uint32_t i = 0; i < n; i++) {
        crc ^= p[i];
        for (int k = 0; k < 8; k++) {
            crc = (crc & 1U) ? (crc >> 1) ^ 0xEDB88320UL : (crc >> 1);
        }
    }
    return crc;
}

const char *PortTool_LoopName(porttool_loop_t loop)
{
    switch (loop) {
    case PORTTOOL_LOOP_CTRL: return "ctrl";
    case PORTTOOL_LOOP_LINK: return "link";
    case PORTTOOL_LOOP_SELF: return "self";
    default:                 return "none";
    }
}

/* Per session: "OK port=" (shape), "OK vals=" (current values), and optional
 * terms= / limits= / info= lines. Shape and values are separate lines so
 * neither overflows PORTTOOL_REPLY_MAX. With `emit` 0 it only counts. */
static uint32_t session_caps(int emit)
{
    char buf[PORTTOOL_REPLY_MAX];
    uint32_t lines = 0;

    for (uint32_t i = 0; i < PORT_COUNT; i++) {
        const porttool_port_t *p = ports[i];

        lines += 2;                       /* port= and vals= */
        if (p->terms != NULL) {
            lines++;
        }
        if (p->limits != NULL) {
            lines++;
        }
        if (p->info != NULL) {
            lines++;
        }
        if (!emit) {
            continue;
        }

        /* A port's pt.run targets go on its session row (DECISIONS.md 17). */
        char runs[96];
        uint32_t shots = PortTool_RunTargetsFor(p->name, runs, sizeof(runs));

        printf("OK port=%s board=%s kind=session blk=%s term=%s channels=%u "
               "loop=%s params=%s running=%d",
               p->name, p->board, p->blk, p->term, (unsigned)p->channels,
               PortTool_LoopName(p->loop), p->params, p->running);
        if (shots > 0U) {
            printf(" runs=%s", runs);
        }
        printf("\r\n");

        buf[0] = '\0';
        if (p->caps != NULL) {
            p->caps(buf, sizeof(buf));
        }
        printf("OK vals=%s %s\r\n", p->name, buf);

        if (p->terms != NULL) {
            printf("OK terms=%s %s\r\n", p->name, p->terms);
        }

        if (p->limits != NULL) {
            buf[0] = '\0';
            p->limits(buf, sizeof(buf));
            printf("OK limits=%s %s\r\n", p->name, buf);
        }

        if (p->info != NULL) {
            buf[0] = '\0';
            p->info(buf, sizeof(buf));
            printf("OK info=%s %s\r\n", p->name, buf);
        }
    }

    return lines;
}

/* The whole port list. `lines=` counts the OK lines that follow (not the same
 * as `ports=`), so the PC knows where the reply ends. hold= and led= say this
 * firmware understands pt.hold and pt.led. */
static void reply_caps(void)
{
    uint32_t run_ports = PortTool_RunCaps(0);   /* one line per run group */

    printf("OK porttool=%s ports=%lu lines=%lu hold=1 led=1\r\n",
           PORTTOOL_VERSION,
           (unsigned long)(PORT_COUNT + run_ports),
           (unsigned long)(session_caps(0) + run_ports));

    (void)session_caps(1);
    (void)PortTool_RunCaps(1);
}

static void reply_id(void)
{
    /* From the UID registers directly: no IAP code is linked in. */
    const volatile uint32_t *uid = (const volatile uint32_t *)UID_BASE;

    printf("OK uid=%08lX%08lX%08lX porttool=%s rx_errors=%lu rx_dropped=%lu\r\n",
           (unsigned long)uid[0], (unsigned long)uid[1], (unsigned long)uid[2],
           PORTTOOL_VERSION, (unsigned long)rx_errors,
           (unsigned long)rx_dropped);
}

_Static_assert(sizeof("OK cal=\r\n") - 1U + 2U * sizeof(calib_area_t)
               <= PORTTOOL_REPLY_MAX, "pt.cal must fit one line");

/* The calibration area's bytes as they sit in flash, read-only. The PC decodes
 * them, so calib_area.h stays the only parser of the format. See
 * $PROD/docs/modules/M4/PORTTOOL-FLOW.md A.4 "pt.cal". */
static void reply_cal(void)
{
    static const char digits[] = "0123456789ABCDEF";
    const volatile uint8_t *area = (const volatile uint8_t *)PORTTOOL_CALIB_BASE;
    char hex[2U * sizeof(calib_area_t) + 1U];

    for (uint32_t i = 0; i < sizeof(calib_area_t); i++) {
        uint8_t b = area[i];
        hex[2U * i]      = digits[b >> 4];
        hex[2U * i + 1U] = digits[b & 0x0FU];
    }
    hex[sizeof(hex) - 1U] = '\0';

    printf("OK cal=%s\r\n", hex);
}

static void reply_list(void)
{
    int any = 0;

    for (uint32_t i = 0; i < PORT_COUNT; i++) {
        if (ports[i]->running) {
            printf("OK running=%s\r\n", ports[i]->name);
            any = 1;
        }
    }
    if (!any) {
        printf("OK running=none\r\n");
    }
}

static void cmd_start(const char *rest)
{
    char name[24];
    char err[96];
    const char *args = NULL;

    PortCmd_Word(rest, name, sizeof(name), &args);
    porttool_port_t *p = find_port(name);
    if (p == NULL) {
        printf("ERR no such port \"%s\"\r\n", name);
        return;
    }

    err[0] = '\0';
    if (p->start == NULL || !p->start(args, err, sizeof(err))) {
        printf("ERR %s start refused: %s\r\n", p->name,
               err[0] ? err : "no reason given");
        return;
    }
    p->running = 1;
    printf("OK started %s%s\r\n", p->name, line_suffix());
}

static void cmd_set(const char *rest)
{
    char name[24];
    char err[96];
    const char *args = NULL;

    PortCmd_Word(rest, name, sizeof(name), &args);
    porttool_port_t *p = find_port(name);
    if (p == NULL) {
        printf("ERR no such port \"%s\"\r\n", name);
        return;
    }
    if (!p->running) {
        printf("ERR %s is not running\r\n", p->name);
        return;
    }

    err[0] = '\0';
    if (p->set == NULL || !p->set(args, err, sizeof(err))) {
        printf("ERR %s set refused: %s\r\n", p->name,
               err[0] ? err : "no reason given");
        return;
    }
    printf("OK set %s%s\r\n", p->name, line_suffix());
}

/* Reads a bare decimal argument. Returns 0 for anything else, including an
 * empty word, so a caller can tell "no argument" from "argument of zero". */
static int parse_u32(const char *word, uint32_t *out)
{
    uint32_t v = 0;
    uint32_t digits = 0;

    for (const char *c = word; *c != '\0'; c++) {
        if (*c < '0' || *c > '9') {
            return 0;
        }
        v = v * 10U + (uint32_t)(*c - '0');
        digits++;
    }
    if (digits == 0U) {
        return 0;
    }
    *out = v;
    return 1;
}

/* pt.echo <port> <value>. Refused for a loop=link port: its reply must come
 * off the link under test (porttool_port_t.echo). */
static void cmd_echo(const char *rest)
{
    char name[24];
    char value[24];
    const char *args = NULL;
    uint32_t v = 0;

    PortCmd_Word(rest, name, sizeof(name), &args);
    porttool_port_t *p = find_port(name);
    if (p == NULL) {
        printf("ERR no such port \"%s\"\r\n", name);
        return;
    }
    if (p->echo == NULL) {
        printf("ERR %s takes its echo on its own link, not on this one\r\n", p->name);
        return;
    }
    if (!p->running) {
        printf("ERR %s is not running\r\n", p->name);
        return;
    }

    PortCmd_Word(args, value, sizeof(value), NULL);
    if (value[0] == '\0') {
        printf("ERR %s echo needs a number\r\n", p->name);
        return;
    }
    if (!parse_u32(value, &v)) {
        printf("ERR %s echo value \"%s\" is not a number\r\n", p->name, value);
        return;
    }

    p->echo(v);
    printf("OK echo %s %lu\r\n", p->name, (unsigned long)v);
}

static void cmd_stop(const char *rest)
{
    char name[24];

    PortCmd_Word(rest, name, sizeof(name), NULL);

    if (strcmp(name, "all") == 0) {
        stop_all();
        printf("OK stopped all%s\r\n", line_suffix());
        return;
    }

    porttool_port_t *p = find_port(name);
    if (p == NULL) {
        printf("ERR no such port \"%s\"\r\n", name);
        return;
    }
    if (p->stop != NULL) {
        p->stop();
    }
    p->running = 0;
    printf("OK stopped %s%s\r\n", p->name, line_suffix());
}

static void cmd_hold(const char *rest)
{
    char word[24];
    uint32_t ms;

    PortCmd_Word(rest, word, sizeof(word), NULL);

    if (word[0] == '\0') {
        if (!hold_armed) {
            printf("OK hold=off\r\n");
            return;
        }
        int32_t left = (int32_t)(hold_until_ms - HAL_GetTick());
        printf("OK hold=%lu left=%ld\r\n", (unsigned long)hold_span_ms,
               (long)(left < 0 ? 0 : left));
        return;
    }

    if (!parse_u32(word, &ms)) {
        printf("ERR hold=\"%s\" is not a number of milliseconds\r\n", word);
        return;
    }

    if (ms == 0U) {
        hold_armed = 0;
        hold_span_ms = 0;
        printf("OK hold=off\r\n");
        return;
    }

    if (ms > PORTTOOL_HOLD_MAX_MS) {
        printf("ERR hold=%lu is over the %lu ms ceiling\r\n",
               (unsigned long)ms, (unsigned long)PORTTOOL_HOLD_MAX_MS);
        return;
    }

    hold_span_ms  = ms;
    hold_until_ms = HAL_GetTick() + ms;
    hold_armed    = 1;
    printf("OK hold=%lu\r\n", (unsigned long)ms);
}

/* The indicator, driven from the PC, which makes every verdict. pt.run
 * led.blink is the one-shot lamp check; this one stays up. */
static void cmd_led(const char *rest)
{
    char word[24];
    uint32_t on;

    PortCmd_Word(rest, word, sizeof(word), NULL);

    if (!PortCmd_GetU32(word, "fault", &on) || on > 1U) {
        printf("ERR pt.led takes fault=0 or fault=1\r\n");
        return;
    }

    PortLed_Init();
    PortLed_Set((int)on);
    printf("OK led fault=%lu\r\n", (unsigned long)on);
}

/* Leaves sessions alone; what follows the target name is the target's args. */
static void cmd_run(const char *rest)
{
    char name[32];
    const char *args = NULL;

    PortCmd_Word(rest, name, sizeof(name), &args);

    if (name[0] == '\0') {
        PortTool_RunList();
        return;
    }

    if (!PortTool_RunTarget(name, args)) {
        printf("ERR no such run target \"%s\" - run pt.run with no argument to "
               "list them\r\n", name);
    }
}

static void dispatch(const char *line)
{
    char verb[24];
    const char *rest = NULL;

    PortCmd_Word(line, verb, sizeof(verb), &rest);

    if (verb[0] == '\0') {
        return;
    } else if (strcmp(verb, "pt.run") == 0) {
        cmd_run(rest);
    } else if (strcmp(verb, "pt.caps") == 0) {
        reply_caps();
    } else if (strcmp(verb, "pt.id") == 0) {
        reply_id();
    } else if (strcmp(verb, "pt.cal") == 0) {
        reply_cal();
    } else if (strcmp(verb, "pt.calw") == 0) {
        PortTool_CalWrite(rest);
    } else if (strcmp(verb, "pt.list") == 0) {
        reply_list();
    } else if (strcmp(verb, "pt.start") == 0) {
        cmd_start(rest);
    } else if (strcmp(verb, "pt.set") == 0) {
        cmd_set(rest);
    } else if (strcmp(verb, "pt.stop") == 0) {
        cmd_stop(rest);
    } else if (strcmp(verb, "pt.echo") == 0) {
        cmd_echo(rest);
    } else if (strcmp(verb, "pt.hold") == 0) {
        cmd_hold(rest);
    } else if (strcmp(verb, "pt.led") == 0) {
        cmd_led(rest);
    } else if (strcmp(verb, "pt.aodiag") == 0) {   /* TEMPORARY, porttool_aodiag.h */
        PortTool_AoDiag(rest, stop_all);
    } else {
        printf("ERR unknown command \"%s\"\r\n", verb);
    }
}

/* Collects one line without ever blocking the sampling loop. */
static void poll_command(void)
{
    uint8_t b;

    /* A receive error costs the command it interrupted: drop the half-line
     * rather than splice two commands into one. */
    if (rx_err_seen) {
        rx_err_seen = 0;
        line_len = 0U;
    }

    while (rx_tail != rx_head) {
        b = rx_ring[rx_tail];
        rx_tail = (uint16_t)((rx_tail + 1U) % RX_RING_SIZE);
        line_heard(HAL_GetTick());
        if (b == '\r' || b == '\n') {
            if (line_len > 0U) {
                line_buf[line_len] = '\0';
                lines_in++;
                dispatch(line_buf);
                line_len = 0U;
            }
        } else if (line_len + 1U < sizeof(line_buf)) {
            line_buf[line_len++] = (char)b;
        } else {
            /* Dropped whole, not truncated into a different command. */
            line_len = 0U;
            printf("ERR line too long\r\n");
        }
    }
}

/* The command port's receive interrupt. Here, not in the CubeMX-generated
 * stm32h7xx_it.c; enabling UART4's IRQ in the .ioc would fail the link loudly.
 * Raw registers, not HAL_UART_IRQHandler: printf's HAL_UART_Transmit is usually
 * using huart4's handle state when this fires. */
void UART4_IRQHandler(void)
{
    uint32_t isr = UART4->ISR;

    if (isr & (USART_ISR_ORE | USART_ISR_FE | USART_ISR_NE)) {
        UART4->ICR = USART_ICR_ORECF | USART_ICR_FECF | USART_ICR_NECF;
        rx_errors++;
        rx_err_seen = 1;
    }

    if (isr & USART_ISR_RXNE_RXFNE) {
        uint8_t b = (uint8_t)(UART4->RDR & 0xFFu);
        uint16_t next = (uint16_t)((rx_head + 1U) % RX_RING_SIZE);

        if (next != rx_tail) {
            rx_ring[rx_head] = b;
            rx_head = next;
        } else {
            /* Drop the newest: overwriting would corrupt a half-read line. */
            rx_dropped++;
        }
    }
}

/* Releases everything when the PC stops renewing. Takes now_ms so the host
 * test can drive the clock. */
static void hold_check(uint32_t now_ms)
{
    if (!hold_armed) {
        return;
    }
    if ((int32_t)(now_ms - hold_until_ms) < 0) {
        return;
    }

    hold_armed = 0;
    stop_all();
    PortLed_Init();
    PortLed_Set(1);
    printf("!hold t=%lu expired=1 span=%lu\r\n", (unsigned long)now_ms,
           (unsigned long)hold_span_ms);
}

/* Gives every running session a chance to push a frame; each decides whether
 * its period has elapsed. Takes now_ms so the host test can drive the clock. */
static void tick_sessions(uint32_t now_ms)
{
    if (!line_settled) {
        return;   /* the PC has not followed a line-rate switch yet */
    }
    for (uint32_t i = 0; i < PORT_COUNT; i++) {
        if (ports[i]->running && ports[i]->tick != NULL) {
            ports[i]->tick(now_ms);
        }
    }
}

void PortTool_Run(void)
{
    /* Priority 6 preempts the HAL timebase (TIM6, TICK_INT_PRIORITY 15); the
     * handler is a few register accesses. Only RXNE: transmit stays polled so
     * printf and frames keep their order. */
    HAL_NVIC_SetPriority(UART4_IRQn, 6, 0);
    HAL_NVIC_EnableIRQ(UART4_IRQn);
    __HAL_UART_ENABLE_IT(&huart4, UART_IT_RXNE);

    printf("\r\n=== port tool %s ===\r\n"
           "Lines starting with ! are pushed samples; everything else is log.\r\n"
           "  pt.caps                          what this board exposes\r\n"
           "  pt.start din ch=1,3,5 period=200 start a session\r\n"
           "  pt.stop all                      stop every session\r\n"
           "  pt.hold 6000                     renew the deadman; 0 disarms\r\n"
           "  pt.led fault=1                   light the indicator; 0 clears\r\n\r\n",
           PORTTOOL_VERSION);

    for (;;) {
        uint32_t now = HAL_GetTick();

        hold_check(now);
        tick_sessions(now);
        PortEth_Poll();
        poll_command();
        line_apply(now);
        line_check(now);
    }
}

#endif /* PORTTOOL_ENABLE */

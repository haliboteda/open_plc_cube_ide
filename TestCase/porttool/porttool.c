// porttool.c
//
// Port test tool entry and command loop - see porttool.h.

#include "porttool.h"
#include "porttool_cmd.h"
#include "porttool_handover.h"
#include "porttool_run.h"
#include "port_led.h"

#include "main.h"
#include "usart.h"          /* huart4: the RS232 log port is also the command port */

#include <stdarg.h>
#include <stdio.h>
#include <string.h>

#if PORTTOOL_ENABLE

#define PORTTOOL_VERSION "0.10.0"

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
    /* A session for the detect switch only; the deep checks on this same port
     * stay as pt.run targets and appear on its row as runs=. */
    &porttool_sd,
    /* Retention, which only means anything over a long run - so it is a
     * session that never blocks rather than the one-way entry it replaces.
     * probe, sweep, retention and crc stay as pt.run targets on this row. */
    &porttool_sdram,
};
#define PORT_COUNT (sizeof(ports) / sizeof(ports[0]))

static char line_buf[PORTTOOL_LINE_MAX];
static uint32_t line_len;

/* The command port is received on interrupt into a ring, not polled.
 *
 * printf here goes one byte at a time through a blocking HAL_UART_Transmit
 * (Core/Src/main.c __io_putchar), so a single sample frame is a hundred
 * blocking calls. A polled receive is not looking at the wire during any of
 * them, and a command byte arriving then is simply lost.
 *
 * Measured on the board 2026-09-08 with sessions running: 8 commands sent from
 * the PC produced one reply lost outright and one command corrupted into
 * "ppt.id", with the board's own rx_errors counter at 9. With sessions stopped
 * the same 8 commands were clean. So this was not a host bug - the board was
 * dropping bytes it never looked for.
 *
 * The ring is filled from the ISR and drained by poll_command, which is the
 * only reader. head is written only by the ISR and tail only by the reader, so
 * one byte of slack in the full test is all the synchronisation needed - no
 * critical section, and the ISR never blocks.
 *
 * 512 is well over one line: PORTTOOL_LINE_MAX is 192, and the longest burst
 * anything sends is a command plus its terminator. */
#define RX_RING_SIZE 512U
static volatile uint8_t  rx_ring[RX_RING_SIZE];
static volatile uint16_t rx_head;   /* ISR writes */
static volatile uint16_t rx_tail;   /* poll_command reads */
static volatile uint32_t rx_dropped;/* ring was full - the reader fell behind */
static volatile uint8_t  rx_err_seen; /* an error arrived; the reader resets its line */

/* Complete lines the command port has taken in. The RS232 session reports it
 * as evidence that the receive direction is carrying traffic. */
static uint32_t lines_in;

/* Receive errors cleared on the command port - see poll_command(). Reported by
 * pt.id because a station that saw one lost a command: the count is the only
 * evidence, and without it a plan step that timed out looks like a fault in
 * whatever it was testing. */
static uint32_t rx_errors;

/* The deadman. The PC owns the clock for a timed run, so a PC that dies leaves
 * the board driving 24 V until somebody presses reset. pt.hold is what the PC
 * renews to say it is still there; when it lapses the board releases every
 * output and lights the indicator itself.
 *
 * *** This is not the board judging a test. DECISIONS.md 22 stands: the board
 * *** judges whether anyone is still listening, never whether the board is
 * *** good. Renewal is explicit - no other command refreshes it - so a PC that
 * *** is merely echoing frames on a dead panel does not keep the outputs live.
 *
 * *** The indicator is lit here rather than left to the PC because the PC is
 * *** by definition gone at this point, and a burn-in rack nobody is watching
 * *** from a screen is exactly where the lamp is the only thing readable. */
#define PORTTOOL_HOLD_MAX_MS 600000U

static uint32_t hold_span_ms;   /* what the PC last asked for, echoed back */
static uint32_t hold_until_ms;
static int      hold_armed;

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

const char *PortTool_LoopName(porttool_loop_t loop)
{
    switch (loop) {
    case PORTTOOL_LOOP_CTRL: return "ctrl";
    case PORTTOOL_LOOP_LINK: return "link";
    case PORTTOOL_LOOP_SELF: return "self";
    default:                 return "none";
    }
}

/* Per session: an "OK port=" line describing its shape, an "OK vals=" line
 * with its current parameter values, and an "OK terms=" line for the ports
 * whose channels do not map onto their terminal range one for one.
 *
 * Shape and values are separate lines because a port with a value per channel
 * cannot fit both inside PORTTOOL_LINE_MAX: dout alone spends 31 characters on
 * "duty=1:0,2:0,...,8:0", and the analog ports will do the same. One line
 * would have to be trimmed to fit, and a trimmed caps line is a parameter the
 * panel silently never renders.
 *
 * With `emit` 0 it only counts, so the header can say how many lines follow. */
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
        if (!emit) {
            continue;
        }

        /* A port with both a session and a deep bring-up entry - rs485 - is
         * one piece of hardware, so it gets one row with the entry listed
         * beside the session rather than a second row under the same name.
         * eth does the same with a one-shot target: its session is the TCP
         * server and eth.link is the PHY probe, and they share an RJ45. */
        char deep[64];
        char runs[64];
        uint32_t targets = PortTool_HandoverTargetsFor(p->name, deep, sizeof(deep));
        uint32_t shots   = PortTool_RunTargetsFor(p->name, runs, sizeof(runs));

        printf("OK port=%s board=%s kind=session blk=%s term=%s channels=%u "
               "loop=%s params=%s running=%d",
               p->name, p->board, p->blk, p->term, (unsigned)p->channels,
               PortTool_LoopName(p->loop), p->params, p->running);
        if (targets > 0U) {
            printf(" targets=%s", deep);
        }
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
    }

    return lines;
}

/* The whole port list - sessions, handover groups and one-shot hardware - so
 * the PC builds its panel from the firmware instead of a hard-coded copy of
 * it, and can check a plan file's targets without a board present.
 *
 * `lines=` counts the OK lines that follow. It is not the same as `ports=`:
 * a port may add a terms= line, and later ones may add more. Reading a fixed
 * count is what keeps the PC from having to guess where the reply ends. */
static void reply_caps(void)
{
    /* A handover group and a run group each print exactly one line and never a
     * terms= line, so for those the line count and the port count are the same
     * number. */
    uint32_t handover_ports = PortTool_HandoverCaps(0);
    uint32_t run_ports      = PortTool_RunCaps(0);

    /* hold= and led= are capability bits, not counts: the PC has no port list
     * of its own, so this is how it learns whether this firmware understands
     * pt.hold and pt.led rather than sending them and reading back an
     * "unknown command". */
    printf("OK porttool=%s ports=%lu lines=%lu hold=1 led=1\r\n",
           PORTTOOL_VERSION,
           (unsigned long)(PORT_COUNT + handover_ports + run_ports),
           (unsigned long)(session_caps(0) + handover_ports + run_ports));

    (void)session_caps(1);
    (void)PortTool_HandoverCaps(1);
    (void)PortTool_RunCaps(1);
}

static void reply_id(void)
{
    /* Straight from the MCU's unique ID registers - no IAP code is linked in,
     * so the tool stays independent of the bootloader's server. */
    const volatile uint32_t *uid = (const volatile uint32_t *)UID_BASE;

    printf("OK uid=%08lX%08lX%08lX porttool=%s rx_errors=%lu rx_dropped=%lu\r\n",
           (unsigned long)uid[0], (unsigned long)uid[1], (unsigned long)uid[2],
           PORTTOOL_VERSION, (unsigned long)rx_errors,
           (unsigned long)rx_dropped);
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
    printf("OK started %s\r\n", p->name);
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
    printf("OK set %s\r\n", p->name);
}

/* pt.echo <port> <value> - the PC sending back the number it just saw.
 *
 * Refused for a loop=link port on purpose. Those take their reply off the link
 * being tested; answering here instead would advance the counter while that
 * link is dead, which is exactly the false pass this tool is not allowed to
 * produce. */
static void cmd_echo(const char *rest)
{
    char name[24];
    char value[24];
    const char *args = NULL;
    uint32_t v = 0;
    int digits = 0;

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
    for (const char *c = value; *c != '\0'; c++) {
        if (*c < '0' || *c > '9') {
            printf("ERR %s echo value \"%s\" is not a number\r\n", p->name, value);
            return;
        }
        v = v * 10U + (uint32_t)(*c - '0');
        digits++;
    }
    if (digits == 0) {
        printf("ERR %s echo needs a number\r\n", p->name);
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
        printf("OK stopped all\r\n");
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
    printf("OK stopped %s\r\n", p->name);
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

/* The indicator, driven from the PC. Every verdict is made up there, so the
 * lamp is told what to show rather than deciding anything here.
 *
 * Distinct from pt.run led.blink, which blocks for a fixed six pulses: that is
 * a one-shot check of the lamp, not a warning that has to stay up. */
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

static void cmd_handover(const char *rest)
{
    char name[32];

    PortCmd_Word(rest, name, sizeof(name), NULL);

    if (name[0] == '\0') {
        PortTool_HandoverList();
        return;
    }

    /* Every session goes down first: the entry taking over reinitialises the
     * same peripherals, and a relay left energised by a session nobody is
     * watching any more is the one that matters. */
    stop_all();

    if (!PortTool_Handover(name)) {
        printf("ERR no such handover target \"%s\" - run pt.handover with no "
               "argument to list them\r\n", name);
    }
}

/* Unlike pt.handover, this comes back: sessions are left alone, and a target
 * that measures nothing still answers with a line the PC can read. */
static void cmd_run(const char *rest)
{
    char name[32];
    const char *args = NULL;

    /* What follows the target name is handed to the target, so a one-shot can
     * be parameterised from a plan the same way a session is. */
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
    } else if (strcmp(verb, "pt.handover") == 0) {
        cmd_handover(rest);
    } else if (strcmp(verb, "pt.run") == 0) {
        cmd_run(rest);
    } else if (strcmp(verb, "pt.caps") == 0) {
        reply_caps();
    } else if (strcmp(verb, "pt.id") == 0) {
        reply_id();
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
    } else {
        printf("ERR unknown command \"%s\"\r\n", verb);
    }
}

/* Collects one line without ever blocking the sampling loop. */
static void poll_command(void)
{
    uint8_t b;

    /* Receive errors are cleared in the ISR now - it is the only code that
     * touches the peripheral. What is left here is draining the ring.
     *
     * An error still costs the command it interrupted, so line_len is reset
     * when one has been seen: keeping a half-line would splice two commands
     * into one. That reset lives here rather than in the ISR because line_len
     * belongs to this loop. */
    if (rx_err_seen) {
        rx_err_seen = 0;
        line_len = 0U;
    }

    while (rx_tail != rx_head) {
        b = rx_ring[rx_tail];
        rx_tail = (uint16_t)((rx_tail + 1U) % RX_RING_SIZE);
        {
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
            /* Overlong input is dropped whole rather than truncated into a
             * command that means something else. */
            line_len = 0U;
            printf("ERR line too long\r\n");
        }
        }
    }
}

/* The command port's receive interrupt.
 *
 * Defined here rather than in Core/Src/stm32h7xx_it.c deliberately: that file
 * is CubeMX-generated and this handler belongs to the tool. If UART4's global
 * interrupt is ever switched on in the .ioc, CubeMX generates its own and the
 * link fails on a duplicate symbol - which is the loud failure, not a silent
 * one.
 *
 * Raw registers, and deliberately NOT HAL_UART_IRQHandler: printf is inside a
 * blocking HAL_UART_Transmit most of the time this fires, and the HAL's own
 * handler would touch the same handle state that call is using. Nothing here
 * reads or writes any of huart4's HAL fields. */
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
            /* The reader fell behind. Counted rather than overwritten:
             * dropping the oldest byte would corrupt a command already
             * half-read, which is worse than losing the newest one. */
            rx_dropped++;
        }
    }
}

/* Releases everything when the PC stops renewing - see the hold_armed block.
 *
 * now_ms is a parameter for the same reason tick_sessions takes one: the host
 * test's clock does not run on its own, and a deadman that could only be
 * reached through HAL_GetTick() would be untestable off a board. */
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

/* Gives every running session a chance to push a frame. Each one decides for
 * itself whether its period has elapsed. Factored out of the superloop so the
 * host test can advance time by hand: without it the frame path - the whole
 * data plane - could only be exercised on a board. */
static void tick_sessions(uint32_t now_ms)
{
    for (uint32_t i = 0; i < PORT_COUNT; i++) {
        if (ports[i]->running && ports[i]->tick != NULL) {
            ports[i]->tick(now_ms);
        }
    }
}

void PortTool_Run(void)
{
    /* Receive on interrupt, so command bytes are captured while printf is
     * blocking inside HAL_UART_Transmit - see the ring buffer's comment for
     * the measurement that made this necessary.
     *
     * Priority 6 is below the HAL timebase (TIM6 takes uwTickPrio, which
     * HAL_Init leaves at 0): the tick must not be delayed by this, and the
     * handler is a few register accesses either way. Only RXNE is enabled -
     * transmission stays polled, because printf's ordering with the sample
     * frames is what makes the log readable by a person. */
    HAL_NVIC_SetPriority(UART4_IRQn, 6, 0);
    HAL_NVIC_EnableIRQ(UART4_IRQn);
    __HAL_UART_ENABLE_IT(&huart4, UART_IT_RXNE);

    printf("\r\n=== port tool %s ===\r\n"
           "Lines starting with ! are pushed samples; everything else is log.\r\n"
           "  pt.caps                          what this board exposes\r\n"
           "  pt.start din ch=1,3,5 period=200 start a session\r\n"
           "  pt.stop all                      stop every session\r\n"
           "  pt.hold 6000                     renew the deadman; 0 disarms\r\n"
           "  pt.led fault=1                   light the indicator; 0 clears\r\n"
           "  pt.handover                      list the standalone bring-up entries\r\n\r\n",
           PORTTOOL_VERSION);

    for (;;) {
        uint32_t now = HAL_GetTick();

        hold_check(now);
        tick_sessions(now);
        PortEth_Poll();
        poll_command();
    }
}

#endif /* PORTTOOL_ENABLE */

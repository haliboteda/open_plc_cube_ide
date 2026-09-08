// porttool_relay.c
//
// Relay session: drives the selected relays and reports what it drove.
//
// Parameters:
//   ch=1,2       which of the six                    (default: all)
//   mode=hold    hold the levels in on=, or square    (default hold)
//   on=1         level for every selected relay       (default 0)
//   on=1:1,2:0   or a level per relay
//   period=2000  half period for mode=square, ms      (default 2000, floor 1000)
//
// *** The floor is 1000 ms, not the tool-wide 50. *** See RELAY_PERIOD_MIN_MS
// below: these contacts are rated 3e4 operations and nothing about testing them
// needs speed.
//
// Frame:
//   !relay t=48213 mode=square ch1=1 ch2=0
//
// *** Each relay carries its own level. There is deliberately no single
// *** level= field any more: with six independent channels one number could
// *** only be right when they all happen to agree, and wrong silently the
// *** rest of the time.
//
// *** The relays are the only actuators here. Stopping the session releases
// *** every one of them rather than leaving whatever the last edge set.

#include "porttool.h"
#include "porttool_cmd.h"

#include "main.h"
#include "relay.h"
#include <stdio.h>
#include <string.h>

#if PORTTOOL_ENABLE

typedef enum { RELAY_MODE_HOLD = 0, RELAY_MODE_SQUARE } relay_mode_t;

static uint32_t     relay_mask = (1U << RELAY_COUNT) - 1U;
static relay_mode_t relay_mode = RELAY_MODE_HOLD;
/* Half period in mode=square, so the default is 2 s closed, 2 s open.
 *
 * *** This port has a floor of its own, well above the tool-wide one. ***
 * These are HF41F mechanical relays rated 3x10^4 operations. At the tool-wide
 * 50 ms floor a square session eats that entire rating in about 25 minutes,
 * and nothing about a contact test needs to be fast - what is being looked for
 * is a contact that closes, which a person hears and an instrument reads at
 * any speed. So the parameter stays configurable, as the user asked on
 * 2026-09-08, but not down into the range that destroys the part.
 *
 * (Learned the wrong way the same day: a command-path stress test drove them
 * at 100 ms for about 30 s, roughly 150 operations of the rated 30,000.)
 *
 * The soak session has the same constraint for the same reason, with a much
 * longer interval because it runs for hours - see porttool_soak.c. */
#define RELAY_PERIOD_MIN_MS 1000U

static uint32_t     relay_period_ms = 2000U;
static uint32_t     relay_due_ms;
static uint32_t     relay_on[RELAY_COUNT];   /* commanded level, per relay */
static int          relay_inited;

/* Rides the RS232 control port, so it only says the loop is alive. Whether a
 * contact actually moved is not knowable from the board at all - there is no
 * read-back - so this number must never be read as a verdict on the relays. */
static porttool_echo_t relay_echo;

static void relay_echo_got(uint32_t value)
{
    PortTool_EchoGot(&relay_echo, value);
}

static void relay_drive(void)
{
    for (uint32_t i = 0; i < RELAY_COUNT; i++) {
        if ((relay_mask & (1U << i)) == 0U) { continue; }
        if (relay_on[i]) {
            Relay_On((RELAY_Name)i);
        } else {
            Relay_Off((RELAY_Name)i);
        }
    }
}

static void relay_release_all(void)
{
    for (uint32_t i = 0; i < RELAY_COUNT; i++) {
        Relay_Off((RELAY_Name)i);
    }
}

/* Everything is parsed into locals and committed only once the whole line is
 * good. Applying as we go would leave a refused command having changed some of
 * the settings, and then "refused" would no longer mean "nothing moved" - the
 * next pt.start with no arguments would quietly use half of what was rejected. */
static int relay_apply(const char *args, char *err, uint32_t err_len)
{
    char probe[64];
    uint32_t v;

    uint32_t     want_mask   = relay_mask;
    relay_mode_t want_mode   = relay_mode;
    uint32_t     want_period = relay_period_ms;
    uint32_t     want_on[RELAY_COUNT];

    for (uint32_t i = 0; i < RELAY_COUNT; i++) {
        want_on[i] = relay_on[i];
    }

    if (PortCmd_GetStr(args, "ch", probe, sizeof(probe))) {
        if (!PortCmd_GetMask(args, "ch", RELAY_COUNT, &v) || v == 0U) {
            snprintf(err, err_len, "ch=\"%s\" must be channels 1..%d separated by commas",
                     probe, RELAY_COUNT);
            return 0;
        }
        want_mask = v;
    }

    if (PortCmd_GetStr(args, "mode", probe, sizeof(probe))) {
        if (strcmp(probe, "hold") == 0) {
            want_mode = RELAY_MODE_HOLD;
        } else if (strcmp(probe, "square") == 0) {
            want_mode = RELAY_MODE_SQUARE;
        } else {
            snprintf(err, err_len, "mode=\"%s\" is not hold or square", probe);
            return 0;
        }
    }

    if (PortCmd_GetStr(args, "on", probe, sizeof(probe))) {
        /* Two spellings: one level for every selected relay, or a level per
         * relay. The plain form is what the command line and the existing
         * documentation use, so it keeps working. */
        if (strchr(probe, ':') != NULL) {
            if (!PortCmd_GetPairs(args, "on", RELAY_COUNT, 1U, want_on, NULL)) {
                snprintf(err, err_len,
                         "on=\"%s\" must be like 1:1,2:0 - relays 1..%d, level 0 or 1, "
                         "each relay named at most once", probe, RELAY_COUNT);
                return 0;
            }
        } else {
            if (!PortCmd_GetU32(args, "on", &v) || v > 1U) {
                snprintf(err, err_len, "on=\"%s\" is not 0 or 1", probe);
                return 0;
            }
            for (uint32_t i = 0; i < RELAY_COUNT; i++) {
                want_on[i] = v;
            }
        }
    }

    if (PortCmd_GetStr(args, "period", probe, sizeof(probe))) {
        if (!PortCmd_GetU32(args, "period", &v)) {
            snprintf(err, err_len, "period=\"%s\" is not a number", probe);
            return 0;
        }
        /* Refused rather than clamped: a station that asked for 100 ms and
         * silently got 1000 would report a cycle count that never happened. */
        if (v < RELAY_PERIOD_MIN_MS) {
            snprintf(err, err_len,
                     "period=%lu is below %lu ms - these are mechanical relays "
                     "rated 3e4 operations, and faster than that wears them out "
                     "without testing anything more",
                     (unsigned long)v, (unsigned long)RELAY_PERIOD_MIN_MS);
            return 0;
        }
        want_period = v;
    }

    relay_mask      = want_mask;
    relay_mode      = want_mode;
    relay_period_ms = want_period;
    for (uint32_t i = 0; i < RELAY_COUNT; i++) {
        relay_on[i] = want_on[i];
    }
    return 1;
}

static int relay_start(const char *args, char *err, uint32_t err_len)
{
    if (!relay_apply(args, err, err_len)) {
        return 0;
    }
    if (!relay_inited) {
        Relay_Init();
        relay_inited = 1;
    }
    relay_release_all();
    relay_drive();
    PortTool_EchoReset(&relay_echo);
    relay_due_ms = HAL_GetTick();
    return 1;
}

static int relay_set(const char *args, char *err, uint32_t err_len)
{
    if (!relay_apply(args, err, err_len)) {
        return 0;
    }
    if (relay_mode == RELAY_MODE_HOLD) {
        relay_drive();
    }
    return 1;
}

static void relay_stop(void)
{
    relay_release_all();
    for (uint32_t i = 0; i < RELAY_COUNT; i++) {
        relay_on[i] = 0U;
    }
}

static void relay_tick(uint32_t now_ms)
{
    char body[PORTTOOL_REPLY_MAX];
    uint32_t n;

    if ((int32_t)(now_ms - relay_due_ms) < 0) {
        return;
    }
    relay_due_ms = now_ms + relay_period_ms;

    if (relay_mode == RELAY_MODE_SQUARE) {
        /* Every selected relay flips its own level, so a set started with
         * mixed levels keeps its pattern and simply inverts it each half
         * period. */
        for (uint32_t i = 0; i < RELAY_COUNT; i++) {
            if ((relay_mask & (1U << i)) != 0U) {
                relay_on[i] = !relay_on[i];
            }
        }
        relay_drive();
    }

    PortTool_EchoTick(&relay_echo);
    n = PortTool_EchoFields(&relay_echo, body, sizeof(body));
    n += (uint32_t)snprintf(body + n, sizeof(body) - n, " mode=%s",
                            (relay_mode == RELAY_MODE_SQUARE) ? "square" : "hold");
    for (uint32_t i = 0; i < RELAY_COUNT && n < sizeof(body); i++) {
        if ((relay_mask & (1U << i)) == 0U) { continue; }
        int w = snprintf(body + n, sizeof(body) - n, " ch%lu=%lu",
                         (unsigned long)(i + 1U), (unsigned long)relay_on[i]);
        if (w < 0) { break; }
        n += (uint32_t)w;
    }

    PortTool_Frame("relay", "%s", body);
}

static void relay_caps(char *out, uint32_t out_len)
{
    char sel[32];
    char on[64];

    PortCmd_FormatMask(relay_mask, RELAY_COUNT, sel, sizeof(sel));
    PortCmd_FormatPairs(relay_on, relay_mask, RELAY_COUNT, on, sizeof(on));
    snprintf(out, out_len, "ch=%s mode=%s on=%s period=%lu",
             sel,
             (relay_mode == RELAY_MODE_SQUARE) ? "square" : "hold",
             on,
             (unsigned long)relay_period_ms);
}

static void relay_limits(char *out, uint32_t out_len)
{
    snprintf(out, out_len, "ch:1..%u mode:hold|square on:0..1 period:%lu..",
             (unsigned)RELAY_COUNT, (unsigned long)RELAY_PERIOD_MIN_MS);
}

porttool_port_t porttool_relay = {
    .name     = "relay",
    .board    = PORTTOOL_BOARD_LOWER,
    .blk      = "B",
    .term     = "B01-B12",
    /* Each relay is a contact pair, so the six channels do not line up with
     * the twelve terminals one for one. */
    .terms    = "B01+B02,B03+B04,B05+B06,B07+B08,B09+B10,B11+B12",
    .params   = "ch,mode,on,period",
    .loop     = PORTTOOL_LOOP_CTRL,
    .channels = RELAY_COUNT,
    .start    = relay_start,
    .set      = relay_set,
    .stop     = relay_stop,
    .tick     = relay_tick,
    .echo     = relay_echo_got,
    .caps     = relay_caps,
    .limits   = relay_limits,
};

#endif /* PORTTOOL_ENABLE */

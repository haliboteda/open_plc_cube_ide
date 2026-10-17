// porttool_relay.c
//
// Relay session: drives the selected relays and reports what it drove.
// Protocol: $PROD/docs/modules/M4/PORTTOOL-FLOW.md B.2 and "!relay".
//
// Parameters:
//   ch=1,2       which of the six                     (default: all)
//   mode=hold    hold the levels in on=, or cycle      (default hold)
//   on=1         level for every selected relay        (default 0)
//   on=1:1,2:0   or a level per relay
//   close=2000   mode=cycle: ms held closed            (default 2000, floor 1000)
//   open=2000    mode=cycle: ms held open              (default 2000, floor 1000)
//   count=10     mode=cycle: open/close cycles to run  (default 10, 1..1000000)
//   close, open and count take one value for every relay or ch:value pairs.
//
// mode=cycle: each selected relay starts released, holds open, closes, holds
// closed, releases (n+1), and stops released once it reaches its count.
//
// Frames:
//   !relay t=48213 seq=3 rx=2 miss=0 mode=cycle ch1=1 n1=12 ch2=0 n2=12 done=0
//   mode=cycle: one at session start and one per edge of any relay.
//   mode=hold:  one per second, without n<n> and done.
//
// Each relay carries its own level; there is no single level= field because
// six independent channels only agree by accident.
//
// Stopping the session releases every relay.

#include "porttool.h"
#include "porttool_cmd.h"

#include "main.h"
#include "relay.h"
#include <stdio.h>
#include <string.h>

#if PORTTOOL_ENABLE

typedef enum { RELAY_MODE_HOLD = 0, RELAY_MODE_CYCLE } relay_mode_t;

/* Datasheet gives no maximum operating frequency, see
 * $PROD/docs/hardware/HARDWARE-FACTS.md "继电器 RY1–RY6". */
#define RELAY_HOLD_MIN_MS   1000U
#define RELAY_COUNT_MAX     1000000U
#define RELAY_HOLD_FRAME_MS 1000U

static uint32_t     relay_mask = (1U << RELAY_COUNT) - 1U;
static relay_mode_t relay_mode = RELAY_MODE_HOLD;
static uint32_t     relay_on[RELAY_COUNT];       /* commanded level, per relay */
static uint32_t     relay_close_ms[RELAY_COUNT] = {2000U, 2000U, 2000U, 2000U, 2000U, 2000U};
static uint32_t     relay_open_ms[RELAY_COUNT]  = {2000U, 2000U, 2000U, 2000U, 2000U, 2000U};
static uint32_t     relay_count[RELAY_COUNT]    = {10U, 10U, 10U, 10U, 10U, 10U};
static uint32_t     relay_n[RELAY_COUNT];        /* cycles completed this session */
static uint32_t     relay_since[RELAY_COUNT];    /* when the current hold began */
static uint32_t     relay_finished;              /* relays that reached their count */
static int          relay_frame_now;             /* send a frame on the next tick */
static uint32_t     relay_due_ms;
static int          relay_inited;

/* Rides the RS232 control port, so it only says the loop is alive; there is
 * no contact read-back, so it is never a verdict on the relays. */
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

/* Every relay released, counts cleared, first frame on the next tick. */
static void relay_cycle_begin(uint32_t now_ms)
{
    relay_release_all();
    for (uint32_t i = 0; i < RELAY_COUNT; i++) {
        relay_on[i]    = 0U;
        relay_n[i]     = 0U;
        relay_since[i] = now_ms;
    }
    relay_finished  = 0U;
    relay_frame_now = 1;
}

/* close= / open= / count=: one value for all six, or ch:value pairs. Returns
 * 0 with err set when malformed or outside lo..hi; vals untouched then. */
static int relay_get_vals(const char *args, const char *key, uint32_t lo, uint32_t hi,
                          const char *unit, uint32_t *vals, char *err, uint32_t err_len)
{
    char probe[96];
    char range[40];
    uint32_t tmp[RELAY_COUNT];
    uint32_t touched = 0U;
    uint32_t v;

    if (!PortCmd_GetStr(args, key, probe, sizeof(probe))) {
        return 1;
    }
    if (hi == 0xFFFFFFFFU) {
        snprintf(range, sizeof(range), "at least %lu%s", (unsigned long)lo, unit);
    } else {
        snprintf(range, sizeof(range), "%lu..%lu%s", (unsigned long)lo, (unsigned long)hi, unit);
    }
    for (uint32_t i = 0; i < RELAY_COUNT; i++) {
        tmp[i] = vals[i];
    }
    if (strchr(probe, ':') != NULL) {
        int ok = PortCmd_GetPairs(args, key, RELAY_COUNT, hi, tmp, &touched);
        for (uint32_t i = 0; ok && i < RELAY_COUNT; i++) {
            if ((touched & (1U << i)) != 0U && tmp[i] < lo) { ok = 0; }
        }
        if (!ok) {
            /* The value is not echoed: six pairs would not fit the reply. */
            snprintf(err, err_len, "%s= must be like 1:%lu,2:%lu - relays 1..%d once each, %s",
                     key, (unsigned long)lo, (unsigned long)lo, RELAY_COUNT, range);
            return 0;
        }
    } else {
        if (!PortCmd_GetU32(args, key, &v) || v < lo || v > hi) {
            snprintf(err, err_len, "%s=\"%s\" must be %s", key, probe, range);
            return 0;
        }
        for (uint32_t i = 0; i < RELAY_COUNT; i++) {
            tmp[i] = v;
        }
    }
    for (uint32_t i = 0; i < RELAY_COUNT; i++) {
        vals[i] = tmp[i];
    }
    return 1;
}

/* Everything is parsed into locals and committed only once the whole line is
 * good, so a refused command changes nothing. */
static int relay_apply(const char *args, char *err, uint32_t err_len)
{
    char probe[64];
    uint32_t v;

    uint32_t     want_mask = relay_mask;
    relay_mode_t want_mode = relay_mode;
    uint32_t     want_on[RELAY_COUNT];
    uint32_t     want_close[RELAY_COUNT];
    uint32_t     want_open[RELAY_COUNT];
    uint32_t     want_count[RELAY_COUNT];

    for (uint32_t i = 0; i < RELAY_COUNT; i++) {
        want_on[i]    = relay_on[i];
        want_close[i] = relay_close_ms[i];
        want_open[i]  = relay_open_ms[i];
        want_count[i] = relay_count[i];
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
        } else if (strcmp(probe, "cycle") == 0) {
            want_mode = RELAY_MODE_CYCLE;
        } else {
            snprintf(err, err_len, "mode=\"%s\" is not hold or cycle", probe);
            return 0;
        }
    }

    if (PortCmd_GetStr(args, "on", probe, sizeof(probe))) {
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

    /* Refused rather than clamped: a cycle count run at a time nobody asked
     * for is not the test that was asked for. */
    if (!relay_get_vals(args, "close", RELAY_HOLD_MIN_MS, 0xFFFFFFFFU, " ms", want_close, err, err_len) ||
        !relay_get_vals(args, "open", RELAY_HOLD_MIN_MS, 0xFFFFFFFFU, " ms", want_open, err, err_len) ||
        !relay_get_vals(args, "count", 1U, RELAY_COUNT_MAX, "", want_count, err, err_len)) {
        return 0;
    }

    relay_mask = want_mask;
    relay_mode = want_mode;
    for (uint32_t i = 0; i < RELAY_COUNT; i++) {
        relay_on[i]       = want_on[i];
        relay_close_ms[i] = want_close[i];
        relay_open_ms[i]  = want_open[i];
        relay_count[i]    = want_count[i];
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
    PortTool_EchoReset(&relay_echo);
    relay_due_ms = HAL_GetTick();
    if (relay_mode == RELAY_MODE_CYCLE) {
        relay_cycle_begin(relay_due_ms);
    } else {
        relay_drive();
    }
    return 1;
}

static int relay_set(const char *args, char *err, uint32_t err_len)
{
    relay_mode_t was_mode = relay_mode;
    uint32_t     was_mask = relay_mask;

    if (!relay_apply(args, err, err_len)) {
        return 0;
    }
    if (relay_mode == RELAY_MODE_HOLD) {
        relay_drive();
    } else if (was_mode != RELAY_MODE_CYCLE || was_mask != relay_mask) {
        relay_cycle_begin(HAL_GetTick());
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

/* mode=cycle: moves every relay whose hold has run out; 1 when any moved. */
static int relay_cycle_step(uint32_t now_ms)
{
    int moved = 0;

    for (uint32_t i = 0; i < RELAY_COUNT; i++) {
        uint32_t bit = 1U << i;
        if ((relay_mask & bit) == 0U || (relay_finished & bit) != 0U) { continue; }
        uint32_t hold = relay_on[i] ? relay_close_ms[i] : relay_open_ms[i];
        if ((uint32_t)(now_ms - relay_since[i]) < hold) { continue; }
        relay_since[i] = now_ms;
        if (relay_on[i]) {
            relay_on[i] = 0U;
            relay_n[i]++;
            if (relay_n[i] >= relay_count[i]) {
                relay_finished |= bit;
            }
        } else {
            relay_on[i] = 1U;
        }
        moved = 1;
    }
    if (moved) {
        relay_drive();
    }
    return moved;
}

static void relay_tick(uint32_t now_ms)
{
    char body[PORTTOOL_REPLY_MAX];
    uint32_t n;
    int cycle = (relay_mode == RELAY_MODE_CYCLE);

    if (cycle) {
        int send = relay_frame_now;
        relay_frame_now = 0;
        if (!relay_cycle_step(now_ms) && !send) {
            return;
        }
    } else {
        if ((int32_t)(now_ms - relay_due_ms) < 0) {
            return;
        }
        relay_due_ms = now_ms + RELAY_HOLD_FRAME_MS;
    }

    /* Six relays at their widest fit PORTTOOL_REPLY_MAX: 164 bytes of body. */
    PortTool_EchoTick(&relay_echo);
    n = PortTool_EchoFields(&relay_echo, body, sizeof(body));
    n += (uint32_t)snprintf(body + n, sizeof(body) - n, " mode=%s", cycle ? "cycle" : "hold");
    for (uint32_t i = 0; i < RELAY_COUNT && n < sizeof(body); i++) {
        if ((relay_mask & (1U << i)) == 0U) { continue; }
        int w = cycle
            ? snprintf(body + n, sizeof(body) - n, " ch%lu=%lu n%lu=%lu",
                       (unsigned long)(i + 1U), (unsigned long)relay_on[i],
                       (unsigned long)(i + 1U), (unsigned long)relay_n[i])
            : snprintf(body + n, sizeof(body) - n, " ch%lu=%lu",
                       (unsigned long)(i + 1U), (unsigned long)relay_on[i]);
        if (w < 0) { break; }
        n += (uint32_t)w;
    }
    if (cycle && n < sizeof(body)) {
        (void)snprintf(body + n, sizeof(body) - n, " done=%d",
                       (relay_finished & relay_mask) == relay_mask ? 1 : 0);
    }

    PortTool_Frame("relay", "%s", body);
}

/* 1 with the shared value when every selected relay has the same one. */
static int relay_same(const uint32_t *vals, uint32_t *value)
{
    int have = 0;

    for (uint32_t i = 0; i < RELAY_COUNT; i++) {
        if ((relay_mask & (1U << i)) == 0U) { continue; }
        if (!have) { *value = vals[i]; have = 1; }
        else if (vals[i] != *value) { return 0; }
    }
    return have;
}

/* close / open / count only while every selected relay shares them: six
 * different values do not fit PORTTOOL_REPLY_MAX (PORTTOOL-FLOW.md B.2). */
static void relay_caps(char *out, uint32_t out_len)
{
    char sel[32];
    char on[64];
    uint32_t c, o, n;
    int w;

    PortCmd_FormatMask(relay_mask, RELAY_COUNT, sel, sizeof(sel));
    PortCmd_FormatPairs(relay_on, relay_mask, RELAY_COUNT, on, sizeof(on));
    w = snprintf(out, out_len, "ch=%s mode=%s on=%s",
                 sel, (relay_mode == RELAY_MODE_CYCLE) ? "cycle" : "hold", on);
    if (w > 0 && (uint32_t)w < out_len &&
        relay_same(relay_close_ms, &c) && relay_same(relay_open_ms, &o) && relay_same(relay_count, &n)) {
        snprintf(out + w, out_len - (uint32_t)w, " close=%lu open=%lu count=%lu",
                 (unsigned long)c, (unsigned long)o, (unsigned long)n);
    }
}

static void relay_limits(char *out, uint32_t out_len)
{
    snprintf(out, out_len, "ch:1..%u mode:hold|cycle on:0..1 close:%lu.. open:%lu.. count:1..%lu",
             (unsigned)RELAY_COUNT, (unsigned long)RELAY_HOLD_MIN_MS,
             (unsigned long)RELAY_HOLD_MIN_MS, (unsigned long)RELAY_COUNT_MAX);
}

porttool_port_t porttool_relay = {
    .name     = "relay",
    .board    = PORTTOOL_BOARD_LOWER,
    .blk      = "B",
    .term     = "B01-B12",
    /* Each relay is a contact pair, so the six channels do not line up with
     * the twelve terminals one for one. */
    .terms    = "B01+B02,B03+B04,B05+B06,B07+B08,B09+B10,B11+B12",
    .params   = "ch,mode,on,close,open,count",
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

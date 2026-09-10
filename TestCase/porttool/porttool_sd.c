// porttool_sd.c
//
// SD card session: watches the detect switch so an insertion or a removal is
// visible on the panel the moment it happens.
//
// Parameters:
//   period=500    milliseconds between frames   (default 500, floor 50)
//
// Frame:
//   !sd t=48213 seq=1 rx=0 miss=0 detected=1 changes=2 in=1 out=1
//
// *** This session polls ONE GPIO and nothing else. Bringing SDMMC1 up and
// *** reading the card's registers takes long enough to be felt in the
// *** superloop, and doing it on every insertion would stall every other
// *** running session for as long as the card takes to answer. What kind of
// *** card went in, how big it is and what filesystem it holds stay with the
// *** pt.run targets on this same port - the panel puts them on one card.
//
// *** changes counts every edge, in counts insertions and out counts
// *** removals. A count is what makes hot-plug decidable by a plan: "detected
// *** went 0 then 1" is a claim about two frames, and a step that only ever
// *** sees the end state cannot tell a card that was always there from one
// *** that was just put in.
//
// *** The detect pin is active-low with a pull-up, on the assumption that the
// *** switch closes to ground on insertion (sd_test.h). If a board turns out
// *** to be wired the other way the counts still work - an edge is an edge -
// *** but detected= would read inverted.

#include "porttool.h"
#include "porttool_cmd.h"
#include "SD/sd_test.h"

#include "main.h"
#include <stdio.h>

#if PORTTOOL_ENABLE

static uint32_t sd_period_ms = 500U;
static uint32_t sd_due_ms;
static int      sd_last = -1;      /* -1 until the first sample, so it is not an edge */
static uint32_t sd_changes;
static uint32_t sd_in;
static uint32_t sd_out;

/* Rides the RS232 control port: it says the loop is alive and nothing about
 * the card. Whether a card is there is detected= below. */
static porttool_echo_t sd_echo;

static void sd_echo_got(uint32_t value)
{
    PortTool_EchoGot(&sd_echo, value);
}

static int sd_apply(const char *args, char *err, uint32_t err_len)
{
    char probe[32];
    uint32_t v;

    if (PortCmd_GetStr(args, "period", probe, sizeof(probe))) {
        if (!PortCmd_GetU32(args, "period", &v)) {
            snprintf(err, err_len, "period=\"%s\" is not a number", probe);
            return 0;
        }
        sd_period_ms = PortTool_ClampPeriod(v);
    }
    return 1;
}

static int sd_start(const char *args, char *err, uint32_t err_len)
{
    if (!sd_apply(args, err, err_len)) {
        return 0;
    }
    /* Counters zeroed by starting, so a plan's "a card was inserted during
     * this step" cannot be satisfied by something that happened before it. */
    sd_changes = 0;
    sd_in = 0;
    sd_out = 0;
    sd_last = -1;
    PortTool_EchoReset(&sd_echo);
    sd_due_ms = HAL_GetTick();
    return 1;
}

static int sd_set(const char *args, char *err, uint32_t err_len)
{
    return sd_apply(args, err, err_len);
}

static void sd_stop(void)
{
    /* The detect pin is an input and the card was never powered by this
     * session; there is nothing to put back. */
}

static void sd_tick(uint32_t now_ms)
{
    char body[PORTTOOL_REPLY_MAX];
    uint32_t n;

    /* Before the due check, so a card that goes in and straight back out
     * between two frames is still counted rather than missed. */
    int now = SD_Test_Detected() ? 1 : 0;
    if (sd_last < 0) {
        sd_last = now;
    } else if (now != sd_last) {
        sd_changes++;
        if (now) { sd_in++; } else { sd_out++; }
        sd_last = now;
    }

    if ((int32_t)(now_ms - sd_due_ms) < 0) {
        return;
    }
    sd_due_ms = now_ms + sd_period_ms;

    PortTool_EchoTick(&sd_echo);
    n = PortTool_EchoFields(&sd_echo, body, sizeof(body));
    (void)snprintf(body + n, sizeof(body) - n,
                   " detected=%d changes=%lu in=%lu out=%lu",
                   now, (unsigned long)sd_changes,
                   (unsigned long)sd_in, (unsigned long)sd_out);

    PortTool_Frame("sd", "%s", body);
}

static void sd_caps(char *out, uint32_t out_len)
{
    snprintf(out, out_len, "period=%lu", (unsigned long)sd_period_ms);
}

static void sd_limits(char *out, uint32_t out_len)
{
    snprintf(out, out_len, "period:%lu..", (unsigned long)PORTTOOL_PERIOD_MIN_MS);
}

porttool_port_t porttool_sd = {
    .name     = "sd",
    .board    = PORTTOOL_BOARD_BRIDGE,
    .blk      = "-",
    .term     = "J6",
    .terms    = NULL,
    .params   = "period",
    .loop     = PORTTOOL_LOOP_CTRL,
    .channels = 1,
    .start    = sd_start,
    .set      = sd_set,
    .stop     = sd_stop,
    .tick     = sd_tick,
    .echo     = sd_echo_got,
    .caps     = sd_caps,
    .limits   = sd_limits,
};

#endif /* PORTTOOL_ENABLE */

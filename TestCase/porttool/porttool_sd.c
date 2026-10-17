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
// Polls the detect GPIO only: bringing SDMMC1 up would stall the superloop.
// Card identity, size and filesystem are the pt.run targets on this port.
//
// changes counts every edge, in insertions, out removals, so a plan can tell
// a card just put in from one that was always there.
//
// The detect pin is taken as active-low with a pull-up (sd_test.h); wired the
// other way, detected= reads inverted but the counts still hold.

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

/* loop=ctrl: nothing about the card (DECISIONS.md 9). */
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
    /* Zeroed on start, so only this step's edges count. */
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
    /* Nothing to put back. */
}

static void sd_tick(uint32_t now_ms)
{
    char body[PORTTOOL_REPLY_MAX];
    uint32_t n;

    /* Before the due check, so an edge between two frames is counted. */
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

// porttool_sdram.c
//
// SDRAM retention session: write random addresses, let them sit, read them
// back, forever - without ever blocking the superloop.
//
// Parameters:
//   wait=5000    how long the data is left sitting, ms (default 5000, floor 1000)
//   period=1000  milliseconds between frames        (default 1000, floor 50)
//
// Frame:
//   !sdram t=48213 seq=1 rx=0 miss=0 ready=1 phase=wait cycles=3 checked=64
//          failed=0 first_bad=0x00000000 wait_ms=5000 seed=0x12345678
//
// Proves auto-refresh runs: cells lose charge in tens of ms without it, hence
// the wait= floor. A session so the PC can judge a long run while it runs
// (DECISIONS.md 38): write and read-back happen inside a tick, the wait is a
// deadline.
//
// loop=ctrl (DECISIONS.md 9): the verdict is failed= against checked=.
// sdram.probe / sweep / crc are pt.run targets on this row (DECISIONS.md 17).

#include "porttool.h"
#include "porttool_cmd.h"

#include "SDRAM/sdram_test.h"

#include "main.h"
#include <stdio.h>
#include <string.h>

#if PORTTOOL_ENABLE

/* Below a second a board with no refresh would still pass. */
#define SDRAM_WAIT_MIN_MS 1000U
#define SDRAM_WAIT_DEF_MS 5000U

typedef enum { SDRAM_PHASE_WRITE = 0, SDRAM_PHASE_WAIT, SDRAM_PHASE_VERIFY } sdram_phase_t;

static uint32_t sdram_wait_ms = SDRAM_WAIT_DEF_MS;
static uint32_t sdram_period_ms = 1000u;
static uint32_t sdram_due_ms;        /* when the next frame goes out */
static uint32_t sdram_sit_until_ms;  /* when the data has sat long enough */

static sdram_phase_t sdram_phase;
static uint32_t sdram_rng_state;
static uint32_t sdram_cycles;        /* completed write/wait/verify rounds */
static uint32_t sdram_failed_total;  /* summed over every cycle */
static uint32_t sdram_first_bad;     /* the first address that ever lost data */
static uint8_t  sdram_ready;

static sdram_retention_t sdram_last;
static porttool_echo_t sdram_echo;

static const char *sdram_phase_name(void)
{
    switch (sdram_phase) {
    case SDRAM_PHASE_WAIT:   return "wait";
    case SDRAM_PHASE_VERIFY: return "verify";
    default:                 return "write";
    }
}

static int sdram_apply(const char *args, char *err, uint32_t err_len)
{
    char probe[32];
    uint32_t v;

    uint32_t want_wait   = sdram_wait_ms;
    uint32_t want_period = sdram_period_ms;

    if (PortCmd_GetStr(args, "wait", probe, sizeof(probe))) {
        if (!PortCmd_GetU32(args, "wait", &v)) {
            snprintf(err, err_len, "wait=\"%s\" is not a number", probe);
            return 0;
        }
        /* Refused rather than clamped: the plan should learn its wait is wrong. */
        if (v < SDRAM_WAIT_MIN_MS) {
            snprintf(err, err_len,
                     "wait=%lu is below %lu ms - cells hold for tens of "
                     "milliseconds unrefreshed, so a shorter wait passes on a "
                     "board whose refresh never started",
                     (unsigned long)v, (unsigned long)SDRAM_WAIT_MIN_MS);
            return 0;
        }
        want_wait = v;
    }

    if (PortCmd_GetStr(args, "period", probe, sizeof(probe))) {
        if (!PortCmd_GetU32(args, "period", &v)) {
            snprintf(err, err_len, "period=\"%s\" is not a number", probe);
            return 0;
        }
        want_period = PortTool_ClampPeriod(v);
    }

    sdram_wait_ms   = want_wait;
    sdram_period_ms = want_period;
    return 1;
}

static int sdram_start(const char *args, char *err, uint32_t err_len)
{
    sdram_probe_t p;

    if (!sdram_apply(args, err, err_len)) {
        return 0;
    }

    /* Refused without the FMC up, or its failures would look like the array's. */
    SDRAM_Test_Probe(&p);
    if (!p.ready) {
        snprintf(err, err_len,
                 "the FMC controller is not brought up, so nothing written "
                 "would mean anything");
        return 0;
    }

    if (sdram_rng_state == 0U) {
        sdram_rng_state = HAL_GetTick() | 1U;  /* xorshift needs a non-zero seed */
    }

    sdram_ready        = 1;
    sdram_cycles       = 0;
    sdram_failed_total = 0;
    sdram_first_bad    = 0;
    memset(&sdram_last, 0, sizeof(sdram_last));
    PortTool_EchoReset(&sdram_echo);

    sdram_phase   = SDRAM_PHASE_WRITE;
    sdram_due_ms  = HAL_GetTick();
    return 1;
}

static int sdram_set(const char *args, char *err, uint32_t err_len)
{
    return sdram_apply(args, err, err_len);
}

static void sdram_stop(void)
{
    sdram_ready = 0;
}

static void sdram_echo_got(uint32_t value)
{
    PortTool_EchoGot(&sdram_echo, value);
}

static void sdram_tick(uint32_t now_ms)
{
    char body[PORTTOOL_REPLY_MAX];
    uint32_t n;

    /* Runs every pass, so the cycle's timing does not follow the frame rate. */
    switch (sdram_phase) {
    case SDRAM_PHASE_WRITE:
        SDRAM_Test_RetentionWrite(&sdram_rng_state, &sdram_last);
        sdram_last.wait_ms = sdram_wait_ms;
        sdram_sit_until_ms = now_ms + sdram_wait_ms;
        sdram_phase = SDRAM_PHASE_WAIT;
        break;

    case SDRAM_PHASE_WAIT:
        if ((int32_t)(now_ms - sdram_sit_until_ms) >= 0) {
            sdram_phase = SDRAM_PHASE_VERIFY;
        }
        break;

    case SDRAM_PHASE_VERIFY:
    default:
        SDRAM_Test_RetentionVerify(&sdram_last);
        sdram_last.wait_ms = sdram_wait_ms;
        sdram_failed_total += sdram_last.failed;
        if ((sdram_first_bad == 0U) && (sdram_last.first_bad_addr != 0U)) {
            sdram_first_bad = sdram_last.first_bad_addr;
        }
        sdram_cycles++;
        sdram_phase = SDRAM_PHASE_WRITE;
        break;
    }

    if ((int32_t)(now_ms - sdram_due_ms) < 0) {
        return;
    }
    sdram_due_ms = now_ms + sdram_period_ms;

    PortTool_EchoTick(&sdram_echo);
    n = PortTool_EchoFields(&sdram_echo, body, sizeof(body));

    /* failed= is summed over the whole run, so one bad cycle stays visible. */
    (void) snprintf(body + n, sizeof(body) - n,
                    " ready=%u phase=%s cycles=%lu checked=%lu failed=%lu"
                    " first_bad=0x%08lX wait_ms=%lu seed=0x%08lX",
                    (unsigned)sdram_ready, sdram_phase_name(),
                    (unsigned long)sdram_cycles,
                    (unsigned long)sdram_last.checked,
                    (unsigned long)sdram_failed_total,
                    (unsigned long)sdram_first_bad,
                    (unsigned long)sdram_wait_ms,
                    (unsigned long)sdram_last.seed);

    PortTool_Frame("sdram", "%s", body);
}

static void sdram_caps(char *out, uint32_t out_len)
{
    snprintf(out, out_len, "wait=%lu period=%lu",
             (unsigned long)sdram_wait_ms, (unsigned long)sdram_period_ms);
}

static void sdram_limits(char *out, uint32_t out_len)
{
    snprintf(out, out_len, "wait:%lu.. period:%lu..",
             (unsigned long)SDRAM_WAIT_MIN_MS,
             (unsigned long)PORTTOOL_PERIOD_MIN_MS);
}

porttool_port_t porttool_sdram = {
    .name     = "sdram",
    .board    = PORTTOOL_BOARD_BRIDGE,
    .blk      = "-",
    .term     = "U6",        /* the part itself; there is no terminal */
    .params   = "wait,period",
    .loop     = PORTTOOL_LOOP_CTRL,
    .channels = 1,
    .start    = sdram_start,
    .set      = sdram_set,
    .stop     = sdram_stop,
    .tick     = sdram_tick,
    .echo     = sdram_echo_got,
    .caps     = sdram_caps,
    .limits   = sdram_limits,
};

#endif /* PORTTOOL_ENABLE */

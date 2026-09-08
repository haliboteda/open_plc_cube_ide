// porttool_soak.c
//
// Burn-in: drive everything at once for hours, watch it, and stop by itself.
//
// Parameters:
//   minutes=120   how long to run       (default 120, 1..600)
//   period=5000   milliseconds a frame  (default 5000, floor 50)
//
// Frame:
//   !soak t=48213 seq=1 rx=0 miss=0 elapsed_s=61 left_s=7139 cycles=12
//        din=0xFF tmin=231 tmax=248 vdda=3287 faults=0 relay_ops=2 done=0
//
// *** No verdict. faults= counts what it saw; whether that many is acceptable
// *** is the PC's decision (DECISIONS.md 22). done=1 says the timer expired,
// *** which is not the same as passing.
//
// *** The indicator is driven here: steady off while healthy, blinking once a
// *** frame once faults > 0. That is the production guide's "abnormal
// *** indicator gives warning", and it is the only thing a person walking past
// *** a burn-in rack can read.
//
// ⚠️ Relay cycling is deliberately slow. The HF41F is rated 3x10^4 operations;
// toggling once a second for two hours would spend 7200 of them - a quarter of
// the contact life - in a single run. At RELAY_INTERVAL_MS the same run costs
// a couple of hundred. A soak that wears out the part it is soaking is not a
// soak, so this interval is a hard design constraint, not a tuning knob.

#include "porttool.h"
#include "porttool_cmd.h"
#include "port_adc.h"
#include "port_dout.h"
#include "port_din.h"
#include "port_led.h"
#include "port_vref.h"
#include "relay.h"

#include "main.h"
#include <stdio.h>

#if PORTTOOL_ENABLE

#define SOAK_MIN_MINUTES     1U
#define SOAK_MAX_MINUTES   600U
#define SOAK_DEF_MINUTES   120U
#define SOAK_DEF_PERIOD   5000U

/* See the header note: this is contact life, not a preference. */
#define RELAY_INTERVAL_MS 30000U

/* Rotating the high-side outputs rather than holding them all on keeps the
 * total 24 V draw bounded: there is no global current limit on this board, and
 * eight channels into unknown loads is how a bench supply trips. */
#define DOUT_STEP_MS       2000U
#define DOUT_DUTY_PCT        50U

static uint32_t soak_minutes = SOAK_DEF_MINUTES;
static uint32_t soak_period_ms = SOAK_DEF_PERIOD;

static uint32_t soak_started_ms;
static uint32_t soak_due_ms;
static uint32_t soak_relay_due_ms;
static uint32_t soak_dout_due_ms;

static uint32_t soak_cycles;
static uint32_t soak_relay_ops;
static uint32_t soak_faults;
static uint32_t soak_dout_ch;      /* which high-side output is on right now */
static int      soak_relay_on;
static int      soak_done;
static int      soak_inited;

static int32_t  soak_tmin;
static int32_t  soak_tmax;
static int      soak_have_temp;

static porttool_echo_t soak_echo;

static void soak_echo_got(uint32_t value)
{
    PortTool_EchoGot(&soak_echo, value);
}

static int soak_apply(const char *args, char *err, uint32_t err_len)
{
    char probe[64];
    uint32_t v;

    uint32_t want_minutes = soak_minutes;
    uint32_t want_period  = soak_period_ms;

    if (PortCmd_GetStr(args, "minutes", probe, sizeof(probe))) {
        if (!PortCmd_GetU32(args, "minutes", &v) ||
            v < SOAK_MIN_MINUTES || v > SOAK_MAX_MINUTES) {
            snprintf(err, err_len, "minutes=\"%s\" must be %lu..%lu",
                     probe, (unsigned long)SOAK_MIN_MINUTES,
                     (unsigned long)SOAK_MAX_MINUTES);
            return 0;
        }
        want_minutes = v;
    }

    if (PortCmd_GetStr(args, "period", probe, sizeof(probe))) {
        if (!PortCmd_GetU32(args, "period", &v)) {
            snprintf(err, err_len, "period=\"%s\" is not a number", probe);
            return 0;
        }
        want_period = PortTool_ClampPeriod(v);
    }

    soak_minutes   = want_minutes;
    soak_period_ms = want_period;
    return 1;
}

/* Everything is released before anything is driven, so a restart never leaves
 * a channel from the previous run energised. */
static void soak_all_off(void)
{
    Relay_Init();
    for (int i = 0; i < RELAY_COUNT; i++) {
        Relay_Off((RELAY_Name)i);
    }
    for (int ch = 1; ch <= PORT_DOUT_COUNT; ch++) {
        PortDout_SetDuty((uint32_t)ch, 0U);
    }
    PortLed_Set(0);
    soak_relay_on = 0;
    soak_dout_ch  = 0;
}

static int soak_start(const char *args, char *err, uint32_t err_len)
{
    uint32_t now;

    if (!soak_apply(args, err, err_len)) {
        return 0;
    }

    if (!soak_inited) {
        /* The analog side gates the whole soak: without a reference the
         * temperature readings are meaningless, and temperature is the main
         * thing a burn-in is watching. */
        if (!PortVref_Enable()) {
            snprintf(err, err_len,
                     "VREFBUF would not come up, so the temperatures a soak "
                     "exists to watch would be meaningless");
            return 0;
        }
        if (!PortAdc_Init()) {
            snprintf(err, err_len, "the ADC would not start");
            return 0;
        }
        if (!PortDout_Init(PORT_DOUT_FREQ_DEF_HZ)) {
            snprintf(err, err_len, "the high-side outputs would not start");
            return 0;
        }
        PortDin_Init();
        PortLed_Init();
        soak_inited = 1;
    }

    soak_all_off();

    now = HAL_GetTick();
    soak_started_ms   = now;
    soak_due_ms       = now;
    soak_relay_due_ms = now + RELAY_INTERVAL_MS;
    soak_dout_due_ms  = now;
    soak_cycles    = 0;
    soak_relay_ops = 0;
    soak_faults    = 0;
    soak_done      = 0;
    soak_have_temp = 0;

    PortTool_EchoReset(&soak_echo);
    return 1;
}

static int soak_set(const char *args, char *err, uint32_t err_len)
{
    return soak_apply(args, err, err_len);
}

static void soak_stop(void)
{
    soak_all_off();
}

/* One high-side channel at a time, walking. */
static void soak_step_dout(uint32_t now_ms)
{
    if ((int32_t)(now_ms - soak_dout_due_ms) < 0) {
        return;
    }
    soak_dout_due_ms = now_ms + DOUT_STEP_MS;

    if (soak_dout_ch >= 1U && soak_dout_ch <= PORT_DOUT_COUNT) {
        PortDout_SetDuty(soak_dout_ch, 0U);
    }
    soak_dout_ch = (soak_dout_ch % PORT_DOUT_COUNT) + 1U;
    PortDout_SetDuty(soak_dout_ch, DOUT_DUTY_PCT);
    soak_cycles++;
}

static void soak_step_relay(uint32_t now_ms)
{
    if ((int32_t)(now_ms - soak_relay_due_ms) < 0) {
        return;
    }
    soak_relay_due_ms = now_ms + RELAY_INTERVAL_MS;

    soak_relay_on = !soak_relay_on;
    for (int i = 0; i < RELAY_COUNT; i++) {
        if (soak_relay_on) {
            Relay_On((RELAY_Name)i);
        } else {
            Relay_Off((RELAY_Name)i);
        }
    }
    soak_relay_ops++;
}

static void soak_read_temps(void)
{
    for (int i = 1; i <= PORT_TEMP_COUNT; i++) {
        uint32_t mv = 0;
        int32_t decic = 0;
        if (!PortAdc_ReadTemp(i, &mv, &decic)) {
            soak_faults++;
            continue;
        }
        if (!soak_have_temp) {
            soak_tmin = decic;
            soak_tmax = decic;
            soak_have_temp = 1;
        } else {
            if (decic < soak_tmin) { soak_tmin = decic; }
            if (decic > soak_tmax) { soak_tmax = decic; }
        }
    }

    /* A reference that stopped being trustworthy mid-run is exactly the kind
     * of drift a soak is for, so it counts as a fault rather than being
     * silently reported as ok=0 once a frame. */
    if (!PortAdc_VddaTrusted()) {
        soak_faults++;
    }
}

static void soak_tick(uint32_t now_ms)
{
    char body[PORTTOOL_REPLY_MAX];
    uint32_t n;
    uint32_t elapsed_s;
    uint32_t total_s;

    if (!soak_done) {
        soak_step_dout(now_ms);
        soak_step_relay(now_ms);
    }

    if ((int32_t)(now_ms - soak_due_ms) < 0) {
        return;
    }
    soak_due_ms = now_ms + soak_period_ms;

    elapsed_s = (now_ms - soak_started_ms) / 1000U;
    total_s   = soak_minutes * 60U;

    if (!soak_done) {
        soak_read_temps();
        if (elapsed_s >= total_s) {
            /* Times out into a safe state and keeps reporting, so a panel that
             * was left open shows how it ended rather than going quiet. */
            soak_all_off();
            soak_done = 1;
        }
    }

    /* Steady off while healthy, one blink a frame once something has gone
     * wrong - the production guide's "abnormal indicator gives warning". */
    if (soak_faults > 0U) {
        PortLed_Set(1);
        HAL_Delay(60);
        PortLed_Set(0);
    }

    PortTool_EchoTick(&soak_echo);
    n = PortTool_EchoFields(&soak_echo, body, sizeof(body));
    n += (uint32_t)snprintf(body + n, sizeof(body) - n,
                            " elapsed_s=%lu left_s=%lu cycles=%lu din=0x%02X"
                            " relay_ops=%lu vdda=%lu faults=%lu done=%d",
                            (unsigned long)elapsed_s,
                            (unsigned long)((elapsed_s >= total_s) ? 0U : (total_s - elapsed_s)),
                            (unsigned long)soak_cycles,
                            PortDin_ReadBits(),
                            (unsigned long)soak_relay_ops,
                            (unsigned long)PortAdc_VddaMv(),
                            (unsigned long)soak_faults,
                            soak_done);

    if (soak_have_temp && n < sizeof(body)) {
        (void)snprintf(body + n, sizeof(body) - n, " tmin=%ld tmax=%ld",
                       (long)soak_tmin, (long)soak_tmax);
    }

    PortTool_Frame("soak", "%s", body);
}

static void soak_caps(char *out, uint32_t out_len)
{
    snprintf(out, out_len, "minutes=%lu period=%lu",
             (unsigned long)soak_minutes, (unsigned long)soak_period_ms);
}

static void soak_limits(char *out, uint32_t out_len)
{
    snprintf(out, out_len, "minutes:%lu..%lu period:%lu..",
             (unsigned long)SOAK_MIN_MINUTES, (unsigned long)SOAK_MAX_MINUTES,
             (unsigned long)PORTTOOL_PERIOD_MIN_MS);
}

porttool_port_t porttool_soak = {
    .name     = "soak",
    .board    = PORTTOOL_BOARD_WHOLE,
    .blk      = "-",
    .term     = "-",          /* drives several blocks at once, names none */
    .terms    = NULL,
    .params   = "minutes,period",
    .loop     = PORTTOOL_LOOP_CTRL,
    .channels = 1,
    .start    = soak_start,
    .set      = soak_set,
    .stop     = soak_stop,
    .tick     = soak_tick,
    .echo     = soak_echo_got,
    .caps     = soak_caps,
    .limits   = soak_limits,
};

#endif /* PORTTOOL_ENABLE */

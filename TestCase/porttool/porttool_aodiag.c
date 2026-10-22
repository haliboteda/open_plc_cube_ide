// porttool_aodiag.c
//
// *** TEMPORARY diagnostic: remove once the AO noise source is found. ***
// pt.aodiag - see porttool_aodiag.h. Experiments, probes and how to read the
// results: $PROD/maps/fixture-test-stages/issues/STG-41-where-does-the-ao-noise-come-from.md
//
//   E0  DAC static, firmware otherwise idle (interrupts live, CPU spinning)
//   E1  quiet window: every interrupt masked, tick stopped, ETH stopped, WFI
//   E2  E1 with the DAC output buffer off
//   E3  E1 with VREFBUF at scale=0..3
//   E4  AI1 converted every 1 ms
//   E5  a fixed line on the control UART every 10 ms
//   E6  DAC channel disabled, pin left to the board's 1k pull-down
//
// Afterwards: VREFBUF back at scale 0, buffer on, the channel back at 0 mV.

#include "porttool.h"
#include "porttool_aodiag.h"
#include "porttool_cmd.h"
#include "port_adc.h"
#include "port_aodiag.h"
#include "port_dac.h"
#include "port_vref.h"

#include <stdio.h>
#include <string.h>

#if PORTTOOL_ENABLE

#define AODIAG_SECS_MAX   600U
#define AODIAG_MV_DEF     1000U
#define AODIAG_SECS_DEF   10U
#define AODIAG_ADC_MS     1U
#define AODIAG_UART_MS    10U

static const char *const aodiag_keys[] = { "exp", "ch", "mv", "secs", "scale" };

/* Every word must be one of aodiag_keys, given once. */
static int aodiag_keys_ok(const char *args, char *bad, uint32_t bad_len)
{
    char word[48];
    const char *rest = args;
    uint32_t seen = 0U;

    for (;;) {
        PortCmd_Word(rest != NULL ? rest : "", word, sizeof(word), &rest);
        if (word[0] == '\0') {
            return 1;
        }
        char *eq = strchr(word, '=');
        uint32_t k = 0U;
        if (eq != NULL) {
            *eq = '\0';
            for (; k < sizeof(aodiag_keys) / sizeof(aodiag_keys[0]); k++) {
                if (strcmp(word, aodiag_keys[k]) == 0) { break; }
            }
        }
        if (eq == NULL || k == sizeof(aodiag_keys) / sizeof(aodiag_keys[0])
            || (seen & (1U << k)) != 0U) {
            snprintf(bad, bad_len, "%s", word);
            return 0;
        }
        seen |= 1U << k;
    }
}

static void aodiag_adc_step(void)
{
    uint32_t raw, mv;
    (void)PortAdc_ReadAin(1, &raw, &mv);
}

static void aodiag_uart_step(void)
{
    printf("aodiag burst 0123456789ABCDEFGHIJKLMNOPQRSTUVWXYZ\r\n");
}

void PortTool_AoDiag(const char *args, void (*stop_sessions)(void))
{
    char s[16];
    char bad[48];
    uint32_t exp, ch = 1U, mv = AODIAG_MV_DEF, secs = AODIAG_SECS_DEF, scale = 0U;
    int have_scale;

    if (args == NULL) { args = ""; }
    if (!aodiag_keys_ok(args, bad, sizeof(bad))) {
        printf("ERR aodiag \"%s\" - takes exp=E0..E6 ch=1|2 mv=<mV> secs=1..%lu "
               "scale=0..3, each once\r\n", bad, (unsigned long)AODIAG_SECS_MAX);
        return;
    }
    if (!PortCmd_GetStr(args, "exp", s, sizeof(s)) || (s[0] != 'E' && s[0] != 'e')
        || s[1] < '0' || s[1] > '6' || s[2] != '\0') {
        printf("ERR aodiag exp= must be E0..E6\r\n");
        return;
    }
    exp = (uint32_t)(s[1] - '0');
    if (PortCmd_GetStr(args, "ch", s, sizeof(s))
        && (!PortCmd_GetU32(args, "ch", &ch) || ch < 1U || ch > PORT_AOUT_COUNT)) {
        printf("ERR aodiag ch= must be 1 or 2\r\n");
        return;
    }
    if (PortCmd_GetStr(args, "secs", s, sizeof(s))
        && (!PortCmd_GetU32(args, "secs", &secs) || secs < 1U || secs > AODIAG_SECS_MAX)) {
        printf("ERR aodiag secs= must be 1..%lu\r\n", (unsigned long)AODIAG_SECS_MAX);
        return;
    }
    have_scale = PortCmd_GetStr(args, "scale", s, sizeof(s));
    if (have_scale != (exp == 3U)) {
        printf("ERR aodiag scale= is required for E3 and refused otherwise\r\n");
        return;
    }
    if (have_scale && (!PortCmd_GetU32(args, "scale", &scale) || scale > 3U)) {
        printf("ERR aodiag scale= must be 0..3\r\n");
        return;
    }
    if (PortCmd_GetStr(args, "mv", s, sizeof(s)) && !PortCmd_GetU32(args, "mv", &mv)) {
        printf("ERR aodiag mv= is not a number\r\n");
        return;
    }

    if (!PortVref_Enable() || !PortAdc_Init() || !PortDac_Init()) {
        printf("ERR aodiag VREFBUF, ADC or DAC would not start\r\n");
        return;
    }
    if (mv > PortDac_VrefMv()) {
        printf("ERR aodiag mv= must be 0..%lu\r\n", (unsigned long)PortDac_VrefMv());
        return;
    }

    if (stop_sessions != NULL) {
        stop_sessions();
    }

    int ok = PortDac_SetMv((int)ch, mv);
    if (ok && exp == 2U) { ok = PortAodiag_DacBuffer((int)ch, 0); }
    if (ok && exp == 6U) { ok = PortAodiag_DacOff((int)ch); }
    if (ok && exp == 3U) { ok = PortAodiag_VrefScale(scale); }
    if (!ok) {
        (void)PortAodiag_VrefScale(0U);
        (void)PortDac_SetMv((int)ch, 0U);
        printf("ERR aodiag could not set up E%lu - restored\r\n", (unsigned long)exp);
        return;
    }

    printf("OK aodiag exp=E%lu ch=%lu mv=%lu secs=%lu", (unsigned long)exp,
           (unsigned long)ch, (unsigned long)mv, (unsigned long)secs);
    if (exp == 3U) {
        printf(" scale=%lu", (unsigned long)scale);
    }
    printf(" starting\r\n");
    fflush(stdout);   /* the quiet window masks the UART next; the line must be out */

    uint32_t steps = 0U;
    switch (exp) {
    case 1U: case 2U: case 3U:
        (void)PortAodiag_QuietSleep(secs);
        break;
    case 4U:
        steps = PortAodiag_Spin(secs * 1000U, AODIAG_ADC_MS, aodiag_adc_step);
        break;
    case 5U:
        steps = PortAodiag_Spin(secs * 1000U, AODIAG_UART_MS, aodiag_uart_step);
        break;
    default:
        (void)PortAodiag_Spin(secs * 1000U, 0U, NULL);
        break;
    }

    int vref_ok = (exp == 3U) ? PortAodiag_VrefScale(0U) : 1;
    (void)PortDac_SetMv((int)ch, 0U);   /* enabled, 0 mV, buffer off */

    if (!vref_ok) {
        printf("ERR aodiag done exp=E3 but VREFBUF did not settle back at scale 0 "
               "- reset the board\r\n");
        return;
    }
    printf("OK aodiag done exp=E%lu steps=%lu\r\n", (unsigned long)exp,
           (unsigned long)steps);
}

#endif /* PORTTOOL_ENABLE */

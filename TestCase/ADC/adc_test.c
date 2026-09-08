// adc_test.c
//
// Board bring-up cases 3 and 11 - see adc_test.h.
//
// The converter itself is driven through TestCase/common/port_adc.*, which the
// port tool shares. What stays here is this case's own cadence, its pass band
// for the temperatures, and the wording it prints.

#include "adc_test.h"
#include "port_adc.h"
#include "main.h"
#include <stdio.h>

#define ADC_TEST_AIN_PERIOD_MS    1000U
#define ADC_TEST_TEMP_PERIOD_MS   3000U

#define TEMP_VALID_MIN_DECIC      (-250)   /* -25.0 degC, sensor spec floor */
#define TEMP_VALID_MAX_DECIC      (1000)   /* +100.0 degC, sensor spec ceiling */

static uint32_t adc_ain_due_ms;
static uint32_t adc_temp_due_ms;
static int      adc_ready;

uint32_t ADC_Test_GetVrefMv(void)
{
    return PortAdc_VddaMeasured() ? PortAdc_VddaMv() : 0U;
}

int ADC_Test_Init(void)
{
    if (!PortAdc_Init()) {
        return 0;
    }

    printf("[ADC] VREF+ measured %lu mV (%s, VREFINT raw=%lu)\r\n",
           (unsigned long)PortAdc_VddaMv(),
           PortAdc_VddaMeasured() ? "via VREFINT" : "VREFINT read failed, assumed",
           (unsigned long)PortAdc_VrefintRaw());

    if (PortAdc_VddaMeasured() && !PortAdc_VddaTrusted()) {
        printf("[ADC] !! VREF+ IS OUT OF RANGE - every reading below is meaningless.\r\n"
               "[ADC] !! This board carries no external reference, so VREF+ exists\r\n"
               "[ADC] !! only while the MCU's own VREFBUF drives that pin. A\r\n"
               "[ADC] !! full-scale VREFINT reading means it is below 1.216 V.\r\n");
    }

    printf("[ADC] Analog In 1 = voltage range (JP9 2-3 + JP5 1-2 bridged),\r\n"
           "[ADC] Analog In 2 = current range (JP8 1-2 + JP6 2-3 bridged).\r\n"
           "[ADC] Readings below are the raw conversion and the pin voltage only -\r\n"
           "[ADC] no front-end scaling is applied to them.\r\n");

    adc_ready       = 1;
    adc_ain_due_ms  = HAL_GetTick();
    adc_temp_due_ms = HAL_GetTick();
    return 1;
}

void ADC_Test_TickAnalogIn(uint32_t now_ms)
{
    if (!adc_ready || (int32_t)(now_ms - adc_ain_due_ms) < 0) {
        return;
    }
    adc_ain_due_ms = now_ms + ADC_TEST_AIN_PERIOD_MS;

    uint32_t raw1_closed = 0, raw1_open = 0, mv1 = 0, raw2 = 0, mv2 = 0;
    int ok1 = PortAdc_ReadAin1SwitchClosed(&raw1_closed) &&
              PortAdc_ReadAin(1, &raw1_open, &mv1);
    int ok2 = PortAdc_ReadAin(2, &raw2, &mv2);

    if (!ok1 || !ok2) {
        printf("[T3 ] Analog In: the ADC did not return a reading (%s / %s)\r\n",
               ok1 ? "input 1 ok" : "input 1 failed",
               ok2 ? "input 2 ok" : "input 2 failed");
        return;
    }

    /* Straight readout, no front-end maths - just the converter result and the
     * pin voltage it corresponds to. */
    printf("[T3 ] Analog In 1 (PC3_C, voltage range): raw %lu, %lu mV at the pin"
           "  [switch closed: raw %lu]\r\n",
           (unsigned long)raw1_open, (unsigned long)mv1,
           (unsigned long)raw1_closed);
    printf("[T3 ] Analog In 2 (PA6,   current range): raw %lu, %lu mV at the pin\r\n",
           (unsigned long)raw2, (unsigned long)mv2);
}

/* Sign carried separately: integer division loses it for -0.9..-0.1 degC. */
static void ADC_Test_FormatTemp(char *out, size_t len, int32_t decic)
{
    const char *sign = (decic < 0) ? "-" : "";
    int32_t mag = (decic < 0) ? -decic : decic;
    snprintf(out, len, "%s%ld.%01ld", sign, (long)(mag / 10), (long)(mag % 10));
}

void ADC_Test_TickTemperature(uint32_t now_ms)
{
    if (!adc_ready || (int32_t)(now_ms - adc_temp_due_ms) < 0) {
        return;
    }
    adc_temp_due_ms = now_ms + ADC_TEST_TEMP_PERIOD_MS;

    uint32_t mv_ps = 0, mv_hs = 0;
    int32_t  decic_ps = 0, decic_hs = 0;
    int ok_ps = PortAdc_ReadTemp(1, &mv_ps, &decic_ps);
    int ok_hs = PortAdc_ReadTemp(2, &mv_hs, &decic_hs);

    if (!ok_ps || !ok_hs) {
        printf("[T11] Temperature: the ADC did not return a reading\r\n");
        return;
    }

    /* 16, not 12: the widest an int32_t deci-degree can format to is
     * "-214748364.8" plus the terminator. Real readings are nowhere near that,
     * but the compiler only knows the type, and CHK-A4 wants a build with no
     * warnings at all. */
    char ps[16], hs[16];
    ADC_Test_FormatTemp(ps, sizeof(ps), decic_ps);
    ADC_Test_FormatTemp(hs, sizeof(hs), decic_hs);

    /* A temperature computed from a collapsed VREF+ can still land inside the
     * plausible band by luck. Saying "sensible" then would be a false pass. */
    const char *verdict;
    if (!PortAdc_VddaTrusted()) {
        verdict = "CANNOT BE TRUSTED, VREF+ is wrong";
    } else if (decic_ps >= TEMP_VALID_MIN_DECIC && decic_ps <= TEMP_VALID_MAX_DECIC &&
               decic_hs >= TEMP_VALID_MIN_DECIC && decic_hs <= TEMP_VALID_MAX_DECIC) {
        verdict = "both look sensible";
    } else {
        verdict = "OUT OF RANGE, so the sensor or the reference is wrong";
    }

    printf("[T11] Board temperature: PA0 input side %lu mV = %s C, "
           "PA3 output side %lu mV = %s C - %s\r\n",
           (unsigned long)mv_ps, ps, (unsigned long)mv_hs, hs, verdict);
}

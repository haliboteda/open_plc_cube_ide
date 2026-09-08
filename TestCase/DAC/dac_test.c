// dac_test.c
//
// Board bring-up case 4 - see dac_test.h.
//
// The converter itself is driven through TestCase/common/port_dac.*, which the
// port tool shares. What stays here is this case's two fixed setpoints, its
// cadence, and the wording it prints.

#include "dac_test.h"
#include "port_dac.h"
#include "port_adc.h"
#include "main.h"
#include <stdio.h>

#define DAC_TEST_PERIOD_MS     3000U
#define DAC_TEST_AOUT1_MV      500U
#define DAC_TEST_AOUT2_MV      1500U

static uint32_t dac_due_ms;
static int      dac_ready;

int DAC_Test_Init(void)
{
    if (!PortDac_Init()) {
        return 0;
    }

    printf("[DAC] full scale = VREF+ = %lu mV (%s)\r\n",
           (unsigned long)PortDac_VrefMv(),
           PortAdc_VddaMeasured() ? "measured" : "fallback, VREF+ was not measurable");

    if (!PortDac_SetMv(1, DAC_TEST_AOUT1_MV)) {
        return 0;
    }
    if (!PortDac_SetMv(2, DAC_TEST_AOUT2_MV)) {
        return 0;
    }

    dac_ready  = 1;
    dac_due_ms = HAL_GetTick();
    return 1;
}

void DAC_Test_Tick(uint32_t now_ms)
{
    if (!dac_ready || (int32_t)(now_ms - dac_due_ms) < 0) {
        return;
    }
    dac_due_ms = now_ms + DAC_TEST_PERIOD_MS;

    uint32_t act1 = PortDac_QuantisedMv(DAC_TEST_AOUT1_MV);
    uint32_t act2 = PortDac_QuantisedMv(DAC_TEST_AOUT2_MV);
    uint32_t ua1  = PortDac_ExpectedMicroamps(act1);
    uint32_t ua2  = PortDac_ExpectedMicroamps(act2);

    printf("[T4 ] Analog Out 1 is driving %lu mV - an ammeter in that loop should read %lu.%03lu mA\r\n",
           (unsigned long)act1,
           (unsigned long)(ua1 / 1000U), (unsigned long)(ua1 % 1000U));
    printf("[T4 ] Analog Out 2 is driving %lu mV - should read %lu.%03lu mA\r\n",
           (unsigned long)act2,
           (unsigned long)(ua2 / 1000U), (unsigned long)(ua2 % 1000U));
    printf("[T4 ] those two currents are only correct while jumpers JP3 and JP4 are open\r\n");
}

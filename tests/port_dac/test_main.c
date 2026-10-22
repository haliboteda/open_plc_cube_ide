/* T4-06: the fixture's AO turns the DAC buffer off at 0 and on otherwise
 * (decision 152), against a DAC that ignores buffer writes while enabled. */
#include <stdio.h>

#include "main.h"
#include "port_adc.h"
#include "port_dac.h"

extern unsigned test_dac_stops[2];

uint32_t PortAdc_VddaMv(void) { return 2500U; }
int PortAdc_VddaMeasured(void) { return 0; }

static int failures;

static void expect(int ch, int buffer_off, uint32_t code, unsigned stops,
                   const char *step)
{
    uint32_t shift = (ch == 1) ? DAC_MCR_MODE1_Pos : DAC_MCR_MODE2_Pos;
    uint32_t en = DAC_CR_EN1 << ((ch == 1) ? DAC_CHANNEL_1 : DAC_CHANNEL_2);
    int off = ((test_dac1.MCR >> shift) & DAC_MCR_MODE1_1) != 0U;
    int on = (test_dac1.CR & en) != 0U;
    uint32_t got = test_dac1.DHR12R[ch - 1];
    unsigned s = test_dac_stops[ch - 1];

    if (off != buffer_off || !on || got != code || s != stops) {
        printf("FAIL %s: AO%d buffer %s (want %s), enabled %d, code %u (want %u), "
               "stops %u (want %u)\n", step, ch, off ? "off" : "on",
               buffer_off ? "off" : "on", on, (unsigned)got, (unsigned)code, s, stops);
        failures++;
    }
}

int main(void)
{
    if (!PortDac_Init()) {
        printf("FAIL PortDac_Init\n");
        return 1;
    }

    /* AO1: first write configures; then only a 0 <-> non-0 change stops it. */
    PortDac_SetMv(1, 1000U); expect(1, 0, 1638U, 1U, "AO1 1000 mV first");
    PortDac_SetMv(1, 0U);    expect(1, 1, 0U,    2U, "AO1 to 0");
    PortDac_SetMv(1, 0U);    expect(1, 1, 0U,    2U, "AO1 0 again");
    PortDac_SetMv(1, 500U);  expect(1, 0, 819U,  3U, "AO1 back to 500 mV");
    PortDac_SetMv(1, 800U);  expect(1, 0, 1310U, 3U, "AO1 500 -> 800 mV, no stop");
    PortDac_SetMv(1, 0U);    expect(1, 1, 0U,    4U, "AO1 to 0 again");

    /* AO2 on its own bits; AO1 untouched. */
    PortDac_SetMv(2, 0U);    expect(2, 1, 0U,    1U, "AO2 0 first");
    PortDac_SetMv(2, 1000U); expect(2, 0, 1638U, 2U, "AO2 to 1000 mV");
    expect(1, 1, 0U, 4U, "AO1 after AO2 writes");

    if (failures) {
        printf("T4-06: %d failure(s)\n", failures);
        return 1;
    }
    printf("T4-06: PASS\n");
    return 0;
}

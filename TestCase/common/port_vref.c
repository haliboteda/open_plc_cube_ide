// port_vref.c
//
// Internal voltage reference - see port_vref.h.

#include "port_vref.h"
#include "main.h"
#include <stdio.h>

static int vref_on;

int PortVref_Enable(void)
{
    if (vref_on) {
        return 1;
    }

    /* VREFBUF sits on APB4 and has its own clock gate. Without it the CSR
     * writes below are silently dropped and the block stays at 0x00000000. */
    __HAL_RCC_VREF_CLK_ENABLE();
    __HAL_RCC_SYSCFG_CLK_ENABLE();

    HAL_SYSCFG_VREFBUF_VoltageScalingConfig(SYSCFG_VREFBUF_VOLTAGE_SCALE0);
    HAL_SYSCFG_VREFBUF_HighImpedanceConfig(SYSCFG_VREFBUF_HIGH_IMPEDANCE_DISABLE);
    SET_BIT(VREFBUF->CSR, VREFBUF_CSR_ENVR);

    /* Start-up is dominated by whatever decoupling sits on VREF+, which is not
     * documented for this board, so allow far more than the datasheet typical. */
    uint32_t start = HAL_GetTick();
    while ((VREFBUF->CSR & VREFBUF_CSR_VRR) == 0U) {
        if ((HAL_GetTick() - start) > 100U) {
            printf("[VREF] VREFBUF not ready after 100 ms - CSR=0x%08lX\r\n",
                   (unsigned long)VREFBUF->CSR);
            return 0;
        }
    }

    /* VRR can assert well before the output has actually settled - measuring
     * right after it gave 2493 mV on one boot and 2763 mV on the next, and that
     * error would multiply into every ADC reading, every temperature and every
     * DAC code. Give it a fixed settling window before anyone reads it. */
    uint32_t ready_ms = HAL_GetTick() - start;
    HAL_Delay(20);

    printf("[VREF] VREFBUF on, scale 0 (nominal 2.5 V), VRR after %lu ms,"
           " settled for 20 ms\r\n", (unsigned long)ready_ms);

    vref_on = 1;
    return 1;
}

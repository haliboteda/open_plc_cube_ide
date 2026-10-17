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

    /* VREFBUF has its own APB4 clock gate; without it CSR writes are dropped. */
    __HAL_RCC_VREF_CLK_ENABLE();
    __HAL_RCC_SYSCFG_CLK_ENABLE();

    HAL_SYSCFG_VREFBUF_VoltageScalingConfig(SYSCFG_VREFBUF_VOLTAGE_SCALE0);
    HAL_SYSCFG_VREFBUF_HighImpedanceConfig(SYSCFG_VREFBUF_HIGH_IMPEDANCE_DISABLE);
    SET_BIT(VREFBUF->CSR, VREFBUF_CSR_ENVR);

    /* Generous timeout: start-up depends on the VREF+ decoupling. */
    uint32_t start = HAL_GetTick();
    while ((VREFBUF->CSR & VREFBUF_CSR_VRR) == 0U) {
        if ((HAL_GetTick() - start) > 100U) {
            printf("[VREF] VREFBUF not ready after 100 ms - CSR=0x%08lX\r\n",
                   (unsigned long)VREFBUF->CSR);
            return 0;
        }
    }

    /* VRR asserts before the output settles (2493 vs 2763 mV measured right
     * after it), so wait a fixed 20 ms. */
    uint32_t ready_ms = HAL_GetTick() - start;
    HAL_Delay(20);

    printf("[VREF] VREFBUF on, scale 0 (nominal 2.5 V), VRR after %lu ms,"
           " settled for 20 ms\r\n", (unsigned long)ready_ms);

    vref_on = 1;
    return 1;
}

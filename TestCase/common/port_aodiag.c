// port_aodiag.c
//
// *** TEMPORARY diagnostic: remove once the AO noise source is found. ***
// See port_aodiag.h; documented in
// $PROD/maps/fixture-test-stages/issues/STG-41-where-does-the-ao-noise-come-from.md

#include "port_aodiag.h"
#include "porttool/porttool.h"   /* PORTTOOL_ENABLE */
#include "main.h"

#if PORTTOOL_ENABLE

#define WAKE_TIM      TIM2      /* 32-bit and unused anywhere else in this image */
#define WAKE_TIM_IRQn TIM2_IRQn
#define WAKE_TIM_HZ   10000U

extern ETH_HandleTypeDef heth;   /* LWIP/Target/ethernetif.c */

/* APB1 timer kernel clock, same rule as port_dout.c. */
static uint32_t apb1_timer_hz(void)
{
    uint32_t hz = HAL_RCC_GetPCLK1Freq();
    if ((RCC->D2CFGR & RCC_D2CFGR_D2PPRE1) != 0U) {
        hz *= 2U;
    }
    return hz;
}

uint32_t PortAodiag_QuietSleep(uint32_t secs)
{
    uint32_t iser[8];
    uint32_t systick_ctrl;
    uint32_t tim6_cr1;
    int eth_was_on = (heth.gState == HAL_ETH_STATE_STARTED);

    if (eth_was_on) {
        (void)HAL_ETH_Stop(&heth);
    }

    __HAL_RCC_TIM2_CLK_ENABLE();
    WAKE_TIM->CR1  = TIM_CR1_URS;          /* UG below must not raise UIF */
    WAKE_TIM->DIER = 0U;
    WAKE_TIM->PSC  = apb1_timer_hz() / WAKE_TIM_HZ - 1U;
    WAKE_TIM->ARR  = secs * WAKE_TIM_HZ - 1U;
    WAKE_TIM->CNT  = 0U;
    WAKE_TIM->EGR  = TIM_EGR_UG;           /* load PSC */
    WAKE_TIM->SR   = 0U;

    /* PRIMASK stays set for the whole window: WFI still wakes on a pending
     * enabled interrupt, so TIM2 needs no handler. */
    __disable_irq();
    for (uint32_t i = 0; i < 8U; i++) {
        iser[i] = NVIC->ISER[i];
        NVIC->ICER[i] = 0xFFFFFFFFUL;
    }
    systick_ctrl = SysTick->CTRL;
    SysTick->CTRL = 0U;
    tim6_cr1 = TIM6->CR1;
    TIM6->CR1 = tim6_cr1 & ~TIM_CR1_CEN;   /* HAL time base */
    __DSB();
    __ISB();

    NVIC_ClearPendingIRQ(WAKE_TIM_IRQn);
    NVIC_EnableIRQ(WAKE_TIM_IRQn);
    WAKE_TIM->DIER = TIM_DIER_UIE;
    WAKE_TIM->CR1  = TIM_CR1_URS | TIM_CR1_OPM | TIM_CR1_CEN;

    while ((WAKE_TIM->SR & TIM_SR_UIF) == 0U) {
        __DSB();
        __WFI();
    }

    WAKE_TIM->CR1  = 0U;
    WAKE_TIM->DIER = 0U;
    WAKE_TIM->SR   = 0U;
    NVIC_DisableIRQ(WAKE_TIM_IRQn);
    NVIC_ClearPendingIRQ(WAKE_TIM_IRQn);
    __HAL_RCC_TIM2_CLK_DISABLE();

    uwTick += secs * 1000U;                /* HAL_GetTick() stays wall time */
    TIM6->CR1 = tim6_cr1;
    SysTick->CTRL = systick_ctrl;
    for (uint32_t i = 0; i < 8U; i++) {
        NVIC->ISER[i] = iser[i];
    }
    __DSB();
    __ISB();
    __enable_irq();

    if (eth_was_on) {
        (void)HAL_ETH_Start(&heth);
    }
    return secs * 1000U;
}

uint32_t PortAodiag_Spin(uint32_t ms, uint32_t step_ms, void (*step)(void))
{
    uint32_t start = HAL_GetTick();
    uint32_t next = start;
    uint32_t steps = 0U;

    while ((HAL_GetTick() - start) < ms) {
        if (step != NULL && (int32_t)(HAL_GetTick() - next) >= 0) {
            next += step_ms;
            step();
            steps++;
        }
    }
    return steps;
}

/* MODEx = 000: external pin, buffer on; 010: external pin, buffer off.
 * Writable only while ENx = 0 (RM0433 DAC_MCR). */
int PortAodiag_DacBuffer(int ch, int on)
{
    uint32_t en, shift;

    if (ch == 1)      { en = DAC_CR_EN1; shift = DAC_MCR_MODE1_Pos; }
    else if (ch == 2) { en = DAC_CR_EN2; shift = DAC_MCR_MODE2_Pos; }
    else              { return 0; }

    DAC1->CR &= ~en;
    DAC1->MCR = (DAC1->MCR & ~(7UL << shift)) | ((on ? 0UL : 2UL) << shift);
    DAC1->CR |= en;
    return 1;
}

int PortAodiag_DacOff(int ch)
{
    if (ch == 1)      { DAC1->CR &= ~DAC_CR_EN1; }
    else if (ch == 2) { DAC1->CR &= ~DAC_CR_EN2; }
    else              { return 0; }
    return 1;
}

int PortAodiag_VrefScale(uint32_t scale)
{
    static const uint32_t vrs[4] = {
        SYSCFG_VREFBUF_VOLTAGE_SCALE0, SYSCFG_VREFBUF_VOLTAGE_SCALE1,
        SYSCFG_VREFBUF_VOLTAGE_SCALE2, SYSCFG_VREFBUF_VOLTAGE_SCALE3,
    };

    if (scale > 3U) {
        return 0;
    }
    /* Off, new scale, on: the same sequence PortVref_Enable() uses cold. */
    CLEAR_BIT(VREFBUF->CSR, VREFBUF_CSR_ENVR);
    HAL_SYSCFG_VREFBUF_VoltageScalingConfig(vrs[scale]);
    SET_BIT(VREFBUF->CSR, VREFBUF_CSR_ENVR);

    uint32_t start = HAL_GetTick();
    while ((VREFBUF->CSR & VREFBUF_CSR_VRR) == 0U) {
        if ((HAL_GetTick() - start) > 100U) {
            return 0;
        }
    }
    HAL_Delay(20);
    return 1;
}

#endif /* PORTTOOL_ENABLE */

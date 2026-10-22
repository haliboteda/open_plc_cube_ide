/* A DAC1 with the one rule port_dac.c has to respect: MODEx (buffer) ignores
 * writes while that channel is enabled (RM0433 DAC_MCR). */
#include "main.h"

DAC_TypeDef test_dac1;
GPIO_TypeDef test_gpio;
unsigned test_dac_stops[2];

static int idx(uint32_t ch) { return ch == DAC_CHANNEL_1 ? 0 : 1; }

HAL_StatusTypeDef HAL_DAC_Init(DAC_HandleTypeDef *h)
{
    (void)h;
    return HAL_OK;
}

/* Like the HAL: rewrites MODEx without disabling the channel first. */
HAL_StatusTypeDef HAL_DAC_ConfigChannel(DAC_HandleTypeDef *h,
                                        const DAC_ChannelConfTypeDef *c,
                                        uint32_t ch)
{
    if ((h->Instance->CR & (DAC_CR_EN1 << ch)) == 0U) {
        h->Instance->MCR = (h->Instance->MCR & ~(DAC_MCR_MODE1 << ch))
                         | (c->DAC_OutputBuffer << ch);
    }
    return HAL_OK;
}

HAL_StatusTypeDef HAL_DAC_SetValue(DAC_HandleTypeDef *h, uint32_t ch,
                                   uint32_t align, uint32_t v)
{
    (void)align;
    h->Instance->DHR12R[idx(ch)] = v;
    return HAL_OK;
}

HAL_StatusTypeDef HAL_DAC_Start(DAC_HandleTypeDef *h, uint32_t ch)
{
    h->Instance->CR |= DAC_CR_EN1 << ch;
    return HAL_OK;
}

HAL_StatusTypeDef HAL_DAC_Stop(DAC_HandleTypeDef *h, uint32_t ch)
{
    h->Instance->CR &= ~(DAC_CR_EN1 << ch);
    test_dac_stops[idx(ch)]++;
    return HAL_OK;
}

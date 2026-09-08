// port_din.c
//
// Shared Digital In access - see port_din.h.

#include "port_din.h"

const port_din_t port_din_pins[PORT_DIN_COUNT] = {
    { GPIOC, GPIO_PIN_6,  "PC6"  },   /* DI1, terminal D02, TIM3_CH1  */
    { GPIOB, GPIO_PIN_5,  "PB5"  },   /* DI2, terminal D03, TIM3_CH2  */
    { GPIOB, GPIO_PIN_6,  "PB6"  },   /* DI3, terminal D04, TIM4_CH1  */
    { GPIOB, GPIO_PIN_7,  "PB7"  },   /* DI4, terminal D05, TIM4_CH2  */
    { GPIOH, GPIO_PIN_10, "PH10" },   /* DI5, terminal D06, TIM5_CH1  */
    { GPIOH, GPIO_PIN_11, "PH11" },   /* DI6, terminal D07, TIM5_CH2  */
    { GPIOI, GPIO_PIN_5,  "PI5"  },   /* DI7, terminal D08, TIM8_CH1  */
    { GPIOI, GPIO_PIN_6,  "PI6"  },   /* DI8, terminal D09, TIM8_CH2  */
};

void PortDin_Init(void)
{
    __HAL_RCC_GPIOB_CLK_ENABLE();
    __HAL_RCC_GPIOC_CLK_ENABLE();
    __HAL_RCC_GPIOH_CLK_ENABLE();
    __HAL_RCC_GPIOI_CLK_ENABLE();

    GPIO_InitTypeDef gpio = {0};
    gpio.Mode = GPIO_MODE_INPUT;
    gpio.Pull = GPIO_NOPULL;
    for (int i = 0; i < PORT_DIN_COUNT; i++) {
        gpio.Pin = port_din_pins[i].pin;
        HAL_GPIO_Init(port_din_pins[i].port, &gpio);
    }
}

uint8_t PortDin_ReadBits(void)
{
    uint8_t bits = 0;

    for (int i = 0; i < PORT_DIN_COUNT; i++) {
        if (HAL_GPIO_ReadPin(port_din_pins[i].port, port_din_pins[i].pin) == GPIO_PIN_SET) {
            bits |= (uint8_t)(1U << i);
        }
    }
    return bits;
}

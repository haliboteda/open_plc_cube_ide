// port_led.c
//
// System indicator on PE2 - see port_led.h.

#include "port_led.h"
#include "main.h"

#define LED_PORT GPIOE
#define LED_PIN  GPIO_PIN_2

static int led_ready;

void PortLed_Init(void)
{
    GPIO_InitTypeDef g = {0};

    if (led_ready) {
        return;
    }

    __HAL_RCC_GPIOE_CLK_ENABLE();
    g.Pin   = LED_PIN;
    g.Mode  = GPIO_MODE_OUTPUT_PP;
    g.Pull  = GPIO_NOPULL;
    g.Speed = GPIO_SPEED_FREQ_LOW;
    HAL_GPIO_Init(LED_PORT, &g);

    HAL_GPIO_WritePin(LED_PORT, LED_PIN, GPIO_PIN_RESET);
    led_ready = 1;
}

void PortLed_Set(int on)
{
    PortLed_Init();
    HAL_GPIO_WritePin(LED_PORT, LED_PIN, on ? GPIO_PIN_SET : GPIO_PIN_RESET);
}

void PortLed_Blink(uint32_t pulses, uint32_t half_ms)
{
    PortLed_Init();

    for (uint32_t i = 0; i < pulses; i++) {
        PortLed_Set(1);
        HAL_Delay(half_ms);
        PortLed_Set(0);
        HAL_Delay(half_ms);
    }
}

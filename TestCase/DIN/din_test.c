// din_test.c
//
// Board bring-up case 1 - see din_test.h.

#include "din_test.h"
#include "main.h"
#include <stdio.h>

#define DIN_TEST_CYCLE_MS 3000U

typedef struct {
    GPIO_TypeDef *port;
    uint16_t      pin;
    const char   *name;
} din_pin_t;

static const din_pin_t din_pins[DIN_TEST_PINS] = {
    { GPIOB, GPIO_PIN_5,  "PB5"  },
    { GPIOC, GPIO_PIN_6,  "PC6"  },
    { GPIOB, GPIO_PIN_6,  "PB6"  },
    { GPIOB, GPIO_PIN_7,  "PB7"  },
    { GPIOH, GPIO_PIN_10, "PH10" },
    { GPIOH, GPIO_PIN_11, "PH11" },
    { GPIOI, GPIO_PIN_5,  "PI5"  },
    { GPIOI, GPIO_PIN_6,  "PI6"  },
};

static uint32_t din_due_ms;
static int      din_ready;

void DIN_Test_Init(void)
{
    __HAL_RCC_GPIOB_CLK_ENABLE();
    __HAL_RCC_GPIOC_CLK_ENABLE();
    __HAL_RCC_GPIOH_CLK_ENABLE();
    __HAL_RCC_GPIOI_CLK_ENABLE();

    GPIO_InitTypeDef gpio = {0};
    gpio.Mode = GPIO_MODE_INPUT;
    gpio.Pull = GPIO_NOPULL;
    for (int i = 0; i < DIN_TEST_PINS; i++) {
        gpio.Pin = din_pins[i].pin;
        HAL_GPIO_Init(din_pins[i].port, &gpio);
    }

    printf("[T1 ] all 8 pins are high-impedance inputs, no internal pull\r\n"
           "[T1 ] reading whatever the board puts on them, once a second\r\n\r\n");

    din_ready  = 1;
    din_due_ms = HAL_GetTick();
}

void DIN_Test_Tick(uint32_t now_ms)
{
    if (!din_ready || (int32_t)(now_ms - din_due_ms) < 0) {
        return;
    }
    din_due_ms = now_ms + DIN_TEST_CYCLE_MS;

    printf("[T1 ] inputs:");
    for (int i = 0; i < DIN_TEST_PINS; i++) {
        printf("  %s=%d", din_pins[i].name,
               HAL_GPIO_ReadPin(din_pins[i].port, din_pins[i].pin) == GPIO_PIN_SET);
    }
    printf("\r\n");
}

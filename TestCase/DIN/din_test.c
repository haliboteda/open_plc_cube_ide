// din_test.c
//
// Board bring-up case 1 - see din_test.h.

#include "din_test.h"
#include "main.h"
#include <stdio.h>

#define DIN_TEST_CYCLE_MS 3000U

/* Column order of the printed line, kept as it was when this file owned the
 * table: PB5 first, then PC6. The shared table is in terminal order. */
static const uint8_t din_print_order[DIN_TEST_PINS] = { 1, 0, 2, 3, 4, 5, 6, 7 };

static uint32_t din_due_ms;
static int      din_ready;

void DIN_Test_Init(void)
{
    PortDin_Init();

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
        const port_din_t *p = &port_din_pins[din_print_order[i]];
        printf("  %s=%d", p->name,
               HAL_GPIO_ReadPin(p->port, p->pin) == GPIO_PIN_SET);
    }
    printf("\r\n");
}

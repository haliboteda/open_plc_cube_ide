// relay_test.c
//
// Relay bring-up - see relay_test.h.

#include "relay_test.h"
#include "main.h"
#include "relay.h"
#include <stdio.h>

#define RELAY_TEST_HALF_PERIOD_MS 2000U

static uint32_t relay_due_ms;
static int      relay_state;

void Relay_Test_AllOff(void)
{
    for (int i = 0; i < RELAY_COUNT; i++) {
        Relay_Off((RELAY_Name)i);
    }
    relay_state = 0;
}

void Relay_Test_Init(void)
{
    Relay_Init();
    Relay_Test_AllOff();
    relay_due_ms = HAL_GetTick();
}

void Relay_Test_Tick(uint32_t now_ms)
{
    if ((int32_t)(now_ms - relay_due_ms) < 0) {
        return;
    }
    relay_due_ms = now_ms + RELAY_TEST_HALF_PERIOD_MS;
    relay_state = !relay_state;

    for (int i = 0; i < RELAY_COUNT; i++) {
        if (relay_state) {
            Relay_On((RELAY_Name)i);
        } else {
            Relay_Off((RELAY_Name)i);
        }
    }
    printf("[T2 ] Relays 1-6 just switched %s (they flip every 2 seconds)\r\n",
           relay_state ? "ON" : "OFF");
}

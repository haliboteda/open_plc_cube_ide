// bringup_test.c
//
// Combined bring-up runner - see bringup_test.h.

#include "bringup_test.h"

#include "main.h"
#include "usart.h"
#include "RELAY/relay_test.h"

#include "DIN/din_test.h"
#include "ADC/adc_test.h"
#include "DAC/dac_test.h"
#include "port_vref.h"

#include <stdio.h>


enum {
    CASE_DIN = 0,    /* 1  */
    CASE_RELAY,      /* 2  */
    CASE_AIN,        /* 3  */
    CASE_AOUT,       /* 4  */
    CASE_TEMP,       /* 11 */
    CASE_COUNT
};

static const char *const case_names[CASE_COUNT] = {
    "T1  Digital In read",
    "T2  Relay 2s square wave",
    "T3  Analog In",
    "T4  Analog Out",
    "T11 Temperature"
};

static int      case_enabled[CASE_COUNT];

static void BringUp_PrintHelp(void)
{
    printf("\r\n--- board bring-up, cases 1 2 3 4 11 running together ---\r\n");
    for (int i = 0; i < CASE_COUNT; i++) {
        printf("  %s  %s\r\n", case_enabled[i] ? "[on ]" : "[off]", case_names[i]);
    }
    printf("  keys: 1 2 3 4 b = toggle (b = temperature), a = all on, ? = this help\r\n\r\n");
}

static void BringUp_Toggle(int idx)
{
    case_enabled[idx] = !case_enabled[idx];
    printf("[MENU] %s -> %s\r\n", case_names[idx], case_enabled[idx] ? "on" : "off");

    /* Leave the relays where the operator can see them rather than frozen
     * mid-cycle in whatever state the last toggle happened to land on. */
    if (idx == CASE_RELAY && !case_enabled[idx]) {
        Relay_Test_AllOff();
    }
}

static void BringUp_PollKeys(void)
{
    uint8_t key;
    if (HAL_UART_Receive(&huart4, &key, 1, 0) != HAL_OK) {
        return;
    }

    switch (key) {
    case '1': BringUp_Toggle(CASE_DIN);   break;
    case '2': BringUp_Toggle(CASE_RELAY); break;
    case '3': BringUp_Toggle(CASE_AIN);   break;
    case '4': BringUp_Toggle(CASE_AOUT);  break;
    case 'b':
    case 'B': BringUp_Toggle(CASE_TEMP);  break;
    case 'a':
    case 'A':
        for (int i = 0; i < CASE_COUNT; i++) {
            case_enabled[i] = 1;
        }
        printf("[MENU] all cases on\r\n");
        break;
    case '?':
    case 'h':
    case 'H': BringUp_PrintHelp(); break;
    default:  break;
    }
}

void BringUp_Test_Run(void)
{
    /* Cases 1, 4 and 11. Cases 2 and 3 are still initialised so a keypress can
     * bring them in, but they stay quiet so they do not bury the others. */
    for (int i = 0; i < CASE_COUNT; i++) {
        case_enabled[i] = (i == CASE_DIN || i == CASE_AOUT || i == CASE_TEMP);
    }

    printf("\r\n[BRINGUP] cases 1, 4 and 11; cases 2 and 3 are paused\r\n");

    DIN_Test_Init();
    Relay_Test_Init();

    /* Before both analog inits: they measure and use the reference it sets up. */
    PortVref_Enable();

    if (!ADC_Test_Init()) {
        printf("[BRINGUP] ADC init FAILED - cases 3 and 11 disabled\r\n");
        case_enabled[CASE_AIN]  = 0;
        case_enabled[CASE_TEMP] = 0;
    }
    if (!DAC_Test_Init()) {
        printf("[BRINGUP] DAC init FAILED - case 4 disabled\r\n");
        case_enabled[CASE_AOUT] = 0;
    }

    BringUp_PrintHelp();

    for (;;) {
        uint32_t now_ms = HAL_GetTick();

        if (case_enabled[CASE_DIN])   { DIN_Test_Tick(now_ms); }
        if (case_enabled[CASE_RELAY]) { Relay_Test_Tick(now_ms); }
        if (case_enabled[CASE_AIN])   { ADC_Test_TickAnalogIn(now_ms); }
        if (case_enabled[CASE_AOUT])  { DAC_Test_Tick(now_ms); }
        if (case_enabled[CASE_TEMP])  { ADC_Test_TickTemperature(now_ms); }

        BringUp_PollKeys();
    }
}

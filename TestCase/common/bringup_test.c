// bringup_test.c
//
// Combined bring-up runner - see bringup_test.h.

#include "bringup_test.h"

#include "main.h"
#include "usart.h"
#include "relay.h"

#include "DIN/din_test.h"
#include "ADC/adc_test.h"
#include "DAC/dac_test.h"

#include <stdio.h>

#define RELAY_TEST_HALF_PERIOD_MS 2000U

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
static uint32_t relay_due_ms;
static int      relay_state;

/* This board has no external voltage reference - the Bridge BOM contains no
 * reference IC at all - so VREF+ carries nothing but its decoupling and the
 * ADC and DAC have no reference until the MCU's own VREFBUF drives that pin.
 * Nothing in this project ever enabled it, which is why every analog reading
 * was garbage and the DAC outputs sat near zero.
 *
 * Scale 0 (about 2.5 V) is the pick: VDDA is 3.3 V so it is allowed, and the
 * Analog In front end divides by 0.2494, which puts a 0-10 V terminal swing at
 * 0-2.494 V - full scale on a 2.5 V reference almost exactly.
 *
 * Driving VREF+ would be wrong if the pin were tied to a supply rail, but it
 * is not: VREFINT reads full scale, which can only happen with VREF+ below
 * 1.216 V, so the pin is floating rather than held at 3V3. */
static void BringUp_EnableVrefBuf(void)
{
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
            return;
        }
    }
    /* VRR can assert well before the output has actually settled - measuring
     * right after it gave 2493 mV on one boot and 2763 mV on the next, and
     * that error would multiply into every ADC reading, every temperature and
     * every DAC code. Give the buffer and whatever decoupling is on VREF+ a
     * fixed settling window before anyone reads it. */
    uint32_t ready_ms = HAL_GetTick() - start;
    HAL_Delay(20);

    printf("[VREF] VREFBUF on, scale 0 (nominal 2.5 V), VRR after %lu ms,"
           " settled for 20 ms\r\n", (unsigned long)ready_ms);
}

static void BringUp_Relay_Init(void)
{
    /* The six relay pins are already GPIO outputs out of MX_GPIO_Init. */
    Relay_Init();
    for (int i = 0; i < RELAY_COUNT; i++) {
        Relay_Off((RELAY_Name)i);
    }
    relay_state  = 0;
    relay_due_ms = HAL_GetTick();
}

static void BringUp_Relay_Tick(uint32_t now_ms)
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
        for (int i = 0; i < RELAY_COUNT; i++) {
            Relay_Off((RELAY_Name)i);
        }
        relay_state = 0;
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
    BringUp_Relay_Init();

    /* Before both analog inits: they measure and use the reference it sets up. */
    BringUp_EnableVrefBuf();

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
        if (case_enabled[CASE_RELAY]) { BringUp_Relay_Tick(now_ms); }
        if (case_enabled[CASE_AIN])   { ADC_Test_TickAnalogIn(now_ms); }
        if (case_enabled[CASE_AOUT])  { DAC_Test_Tick(now_ms); }
        if (case_enabled[CASE_TEMP])  { ADC_Test_TickTemperature(now_ms); }

        BringUp_PollKeys();
    }
}

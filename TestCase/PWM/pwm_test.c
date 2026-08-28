// pwm_test.c
//
// Standalone PWM bring-up test - see pwm_test.h for wiring/pin notes.
//
// Drives TIM1_CH2 / PA9 (Digital Out 6 / HIGHSIDE_FET6 terminal) with a
// 1 kHz PWM that ramps perceived brightness 0% -> 100% -> 0% forever
// ("breathing"), 3s per direction by default. The duty cycle itself is NOT
// a linear ramp - see PWM_Test_LightnessToDuty() for why.
// Wire an LED (+ series resistor, sized for the Digital Out supply rail)
// across the Digital Out 6 terminal and its return/GND terminal: it should
// fade smoothly up and down. No jumper wire or instrument needed to see
// that the timer channel is alive and the duty cycle is actually changing.
//
// What this does NOT prove (needs a scope/multimeter):
//   - edge quality (rise/fall time, ringing, actual voltage levels)
//   - exact frequency/duty accuracy, or behaviour under a real load
//     (motor/lamp/fan) on the VNQ5160K-E high-side switch

#include "pwm_test.h"
#include "main.h"
#include <stdio.h>

/* ---- Output under test: TIM1_CH2 / PA9 --------------------------------- */
#define PWM_TIM                  TIM1
#define PWM_CHANNEL              TIM_CHANNEL_2
#define PWM_GPIO_PORT            GPIOA
#define PWM_GPIO_PIN             GPIO_PIN_9
#define PWM_GPIO_AF              GPIO_AF1_TIM1

#define PWM_TARGET_FREQ_HZ       1000U   /* 1 kHz - well above visible flicker */
#define PWM_ARR_STEPS            1000U   /* duty resolution: 0.1% */

/* ---- Breathing ramp ----------------------------------------------------- */
#define BREATHE_STEP_PCT         1U      /* brightness change per step */
#define BREATHE_STEP_DELAY_MS    30U     /* 100 steps * 30ms = 3s per direction */

static TIM_HandleTypeDef htim_pwm;

static void PWM_Test_GPIO_Init(void)
{
    __HAL_RCC_GPIOA_CLK_ENABLE();

    GPIO_InitTypeDef gpio = {0};
    gpio.Pin = PWM_GPIO_PIN;
    gpio.Mode = GPIO_MODE_AF_PP;
    gpio.Pull = GPIO_NOPULL;
    gpio.Speed = GPIO_SPEED_FREQ_VERY_HIGH;
    gpio.Alternate = PWM_GPIO_AF;
    HAL_GPIO_Init(PWM_GPIO_PORT, &gpio);
}

/* Returns the requested duty (0-100) as a CCR value for PWM_ARR_STEPS steps */
static uint32_t PWM_Test_DutyToCCR(uint32_t duty_pct)
{
    return (PWM_ARR_STEPS * duty_pct) / 100U;
}

static int PWM_Test_InitOutput(uint32_t duty_pct)
{
    __HAL_RCC_TIM1_CLK_ENABLE();

    uint32_t tim1clk = HAL_RCC_GetPCLK2Freq();
    if ((RCC->D2CFGR & RCC_D2CFGR_D2PPRE2) != 0U) {
        tim1clk *= 2U; /* timer kernel clock is 2x pclk when the APB prescaler != 1 */
    }

    uint32_t psc = (tim1clk / (PWM_TARGET_FREQ_HZ * PWM_ARR_STEPS)) - 1U;

    htim_pwm.Instance = PWM_TIM;
    htim_pwm.Init.Prescaler = psc;
    htim_pwm.Init.CounterMode = TIM_COUNTERMODE_UP;
    htim_pwm.Init.Period = PWM_ARR_STEPS - 1U;
    htim_pwm.Init.ClockDivision = TIM_CLOCKDIVISION_DIV1;
    htim_pwm.Init.RepetitionCounter = 0;
    htim_pwm.Init.AutoReloadPreload = TIM_AUTORELOAD_PRELOAD_DISABLE;
    if (HAL_TIM_PWM_Init(&htim_pwm) != HAL_OK) {
        return 0;
    }

    TIM_OC_InitTypeDef oc = {0};
    oc.OCMode = TIM_OCMODE_PWM1;
    oc.Pulse = PWM_Test_DutyToCCR(duty_pct);
    oc.OCPolarity = TIM_OCPOLARITY_HIGH;
    oc.OCNPolarity = TIM_OCNPOLARITY_HIGH;
    oc.OCFastMode = TIM_OCFAST_DISABLE;
    oc.OCIdleState = TIM_OCIDLESTATE_RESET;
    oc.OCNIdleState = TIM_OCNIDLESTATE_RESET;
    if (HAL_TIM_PWM_ConfigChannel(&htim_pwm, &oc, PWM_CHANNEL) != HAL_OK) {
        return 0;
    }

    TIM_BreakDeadTimeConfigTypeDef bd = {0};
    bd.OffStateRunMode = TIM_OSSR_DISABLE;
    bd.OffStateIDLEMode = TIM_OSSI_DISABLE;
    bd.LockLevel = TIM_LOCKLEVEL_OFF;
    bd.DeadTime = 0;
    bd.BreakState = TIM_BREAK_DISABLE;
    bd.BreakPolarity = TIM_BREAKPOLARITY_HIGH;
    bd.AutomaticOutput = TIM_AUTOMATICOUTPUT_DISABLE;
    HAL_TIMEx_ConfigBreakDeadTime(&htim_pwm, &bd);

    uint32_t actual_freq = tim1clk / (psc + 1U) / PWM_ARR_STEPS;
    printf("PWM_TEST: output cfg TIM1_CH2/PA9 tim1clk=%luHz psc=%lu arr=%lu -> freq=%luHz\r\n",
           (unsigned long)tim1clk, (unsigned long)psc, (unsigned long)(PWM_ARR_STEPS - 1U),
           (unsigned long)actual_freq);

    return HAL_TIM_PWM_Start(&htim_pwm, PWM_CHANNEL) == HAL_OK;
}

static void PWM_Test_SetDuty(uint32_t duty_pct)
{
    __HAL_TIM_SET_COMPARE(&htim_pwm, PWM_CHANNEL, PWM_Test_DutyToCCR(duty_pct));
}

/* Human eyes don't perceive duty cycle linearly (far more sensitive at low
   duty, almost flat near 100%) - a linear duty ramp looks like it snaps to
   "bright" in the first fraction of the ramp and just sits there. This maps
   a linearly-increasing perceived lightness (0-100, "eyeball %") to the duty
   cycle (0-100) that actually produces it, using the CIE 1931 lightness
   formula. Feed it a linear time ramp and the LED looks evenly brighter. */
static uint32_t PWM_Test_LightnessToDuty(uint32_t lightness_pct)
{
    float l = (float)lightness_pct;
    float y;
    if (l <= 8.0f) {
        y = l / 903.3f;
    } else {
        float t = (l + 16.0f) / 116.0f;
        y = t * t * t;
    }
    return (uint32_t)(y * 100.0f + 0.5f);
}

void PWM_Test_Run(void)
{
    printf("PWM_TEST: LED breathing on Digital Out 6 (PA9/TIM1_CH2/HIGHSIDE_FET6)\r\n");

    PWM_Test_GPIO_Init();

    if (!PWM_Test_InitOutput(0U)) {
        printf("PWM_TEST: FAIL reason=output_init\r\n");
        return;
    }

    for (;;) {
        for (uint32_t lightness = 0U; lightness <= 100U; lightness += BREATHE_STEP_PCT) {
            PWM_Test_SetDuty(PWM_Test_LightnessToDuty(lightness));
            HAL_Delay(BREATHE_STEP_DELAY_MS);
        }
        for (uint32_t lightness = 100U; lightness > 0U; lightness -= BREATHE_STEP_PCT) {
            PWM_Test_SetDuty(PWM_Test_LightnessToDuty(lightness - BREATHE_STEP_PCT));
            HAL_Delay(BREATHE_STEP_DELAY_MS);
        }
    }
}

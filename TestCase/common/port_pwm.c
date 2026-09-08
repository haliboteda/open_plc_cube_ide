// port_pwm.c
//
// Shared TIM1_CH2 PWM output - see port_pwm.h.

#include "port_pwm.h"
#include "main.h"

#define PWM_TIM        TIM1
#define PWM_CHANNEL    TIM_CHANNEL_2
#define PWM_GPIO_PORT  GPIOA
#define PWM_GPIO_PIN   GPIO_PIN_9
#define PWM_GPIO_AF    GPIO_AF1_TIM1

static TIM_HandleTypeDef htim_pwm;
static uint32_t pwm_kernel_hz;
static uint32_t pwm_psc;
static uint32_t pwm_actual_hz;
static int      pwm_ready;

static uint32_t PortPwm_DutyToCCR(uint32_t duty_pct)
{
    return (PORT_PWM_STEPS * duty_pct) / 100U;
}

static void PortPwm_GpioInit(void)
{
    __HAL_RCC_GPIOA_CLK_ENABLE();

    GPIO_InitTypeDef gpio = {0};
    gpio.Pin       = PWM_GPIO_PIN;
    gpio.Mode      = GPIO_MODE_AF_PP;
    gpio.Pull      = GPIO_NOPULL;
    gpio.Speed     = GPIO_SPEED_FREQ_VERY_HIGH;
    gpio.Alternate = PWM_GPIO_AF;
    HAL_GPIO_Init(PWM_GPIO_PORT, &gpio);
}

int PortPwm_Init(uint32_t freq_hz, uint32_t duty_pct)
{
    if (pwm_ready) {
        PortPwm_SetDuty(duty_pct);
        return 1;
    }
    if (freq_hz == 0U) {
        return 0;
    }

    PortPwm_GpioInit();
    __HAL_RCC_TIM1_CLK_ENABLE();

    pwm_kernel_hz = HAL_RCC_GetPCLK2Freq();
    if ((RCC->D2CFGR & RCC_D2CFGR_D2PPRE2) != 0U) {
        pwm_kernel_hz *= 2U;   /* kernel clock is 2x pclk when the APB prescaler != 1 */
    }
    pwm_psc = (pwm_kernel_hz / (freq_hz * PORT_PWM_STEPS)) - 1U;

    htim_pwm.Instance               = PWM_TIM;
    htim_pwm.Init.Prescaler         = pwm_psc;
    htim_pwm.Init.CounterMode       = TIM_COUNTERMODE_UP;
    htim_pwm.Init.Period            = PORT_PWM_STEPS - 1U;
    htim_pwm.Init.ClockDivision     = TIM_CLOCKDIVISION_DIV1;
    htim_pwm.Init.RepetitionCounter = 0;
    htim_pwm.Init.AutoReloadPreload = TIM_AUTORELOAD_PRELOAD_DISABLE;
    if (HAL_TIM_PWM_Init(&htim_pwm) != HAL_OK) {
        return 0;
    }

    TIM_OC_InitTypeDef oc = {0};
    oc.OCMode       = TIM_OCMODE_PWM1;
    oc.Pulse        = PortPwm_DutyToCCR(duty_pct);
    oc.OCPolarity   = TIM_OCPOLARITY_HIGH;
    oc.OCNPolarity  = TIM_OCNPOLARITY_HIGH;
    oc.OCFastMode   = TIM_OCFAST_DISABLE;
    oc.OCIdleState  = TIM_OCIDLESTATE_RESET;
    oc.OCNIdleState = TIM_OCNIDLESTATE_RESET;
    if (HAL_TIM_PWM_ConfigChannel(&htim_pwm, &oc, PWM_CHANNEL) != HAL_OK) {
        return 0;
    }

    TIM_BreakDeadTimeConfigTypeDef bd = {0};
    bd.OffStateRunMode  = TIM_OSSR_DISABLE;
    bd.OffStateIDLEMode = TIM_OSSI_DISABLE;
    bd.LockLevel        = TIM_LOCKLEVEL_OFF;
    bd.DeadTime         = 0;
    bd.BreakState       = TIM_BREAK_DISABLE;
    bd.BreakPolarity    = TIM_BREAKPOLARITY_HIGH;
    bd.AutomaticOutput  = TIM_AUTOMATICOUTPUT_DISABLE;
    HAL_TIMEx_ConfigBreakDeadTime(&htim_pwm, &bd);

    pwm_actual_hz = pwm_kernel_hz / (pwm_psc + 1U) / PORT_PWM_STEPS;

    if (HAL_TIM_PWM_Start(&htim_pwm, PWM_CHANNEL) != HAL_OK) {
        return 0;
    }
    pwm_ready = 1;
    return 1;
}

void PortPwm_SetDuty(uint32_t duty_pct)
{
    __HAL_TIM_SET_COMPARE(&htim_pwm, PWM_CHANNEL, PortPwm_DutyToCCR(duty_pct));
}

uint32_t PortPwm_KernelClockHz(void) { return pwm_kernel_hz; }
uint32_t PortPwm_Prescaler(void)     { return pwm_psc; }
uint32_t PortPwm_ActualFreqHz(void)  { return pwm_actual_hz; }

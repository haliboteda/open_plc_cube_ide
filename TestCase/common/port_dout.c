// port_dout.c
//
// Eight independent software PWM outputs - see port_dout.h.

#include "port_dout.h"

#include <string.h>

/* TIM7 is a basic timer with no pins of its own, and nothing else in this
 * project uses it - TIM6 is the HAL time base, TIM1/8/15 own the pins these
 * outputs sit on. Its interrupt vector is a weak alias in the startup file, so
 * defining the handler here overrides it without touching generated code. */
#define DOUT_TIM TIM7

const port_dout_t port_dout_pins[PORT_DOUT_COUNT] = {
    { GPIOB, GPIO_PIN_13, "A03" },   /* Digital Out 1 - HSFET_1 */
    { GPIOB, GPIO_PIN_0,  "A04" },   /* Digital Out 2 */
    { GPIOH, GPIO_PIN_15, "A05" },   /* Digital Out 3 */
    { GPIOE, GPIO_PIN_4,  "A06" },   /* Digital Out 4 */
    { GPIOA, GPIO_PIN_8,  "A07" },   /* Digital Out 5 */
    { GPIOA, GPIO_PIN_9,  "A08" },   /* Digital Out 6 - also the PWM bring-up pin */
    { GPIOI, GPIO_PIN_7,  "A09" },   /* Digital Out 7 */
    { GPIOE, GPIO_PIN_5,  "A10" },   /* Digital Out 8 */
};

static TIM_HandleTypeDef htim_dout;
static uint32_t dout_actual_hz;
static int      dout_ready;

/* Read by the interrupt, written by the main loop. One byte per channel and a
 * single store per write, so a duty change lands whole - a torn value would be
 * one wrong period, but there is no reason to allow even that. */
static volatile uint8_t dout_duty[PORT_DOUT_COUNT];

/* Counts 0..PORT_DOUT_STEPS-1. Only the interrupt touches it. */
static volatile uint8_t dout_phase;

static void dout_gpio_init(void)
{
    __HAL_RCC_GPIOA_CLK_ENABLE();
    __HAL_RCC_GPIOB_CLK_ENABLE();
    __HAL_RCC_GPIOE_CLK_ENABLE();
    __HAL_RCC_GPIOH_CLK_ENABLE();
    __HAL_RCC_GPIOI_CLK_ENABLE();

    for (int i = 0; i < PORT_DOUT_COUNT; i++) {
        GPIO_InitTypeDef gpio = {0};
        gpio.Pin   = port_dout_pins[i].pin;
        gpio.Mode  = GPIO_MODE_OUTPUT_PP;
        gpio.Pull  = GPIO_NOPULL;
        gpio.Speed = GPIO_SPEED_FREQ_LOW;   /* the switch is far slower than the pin */
        HAL_GPIO_Init(port_dout_pins[i].port, &gpio);
        HAL_GPIO_WritePin(port_dout_pins[i].port, port_dout_pins[i].pin,
                          GPIO_PIN_RESET);
    }
}

/* The kernel clock feeding TIM7 is on APB1, doubled when that bus runs at a
 * prescaler other than 1 - the same rule port_pwm.c follows for TIM1. */
static uint32_t dout_kernel_hz(void)
{
    uint32_t hz = HAL_RCC_GetPCLK1Freq();
    if ((RCC->D2CFGR & RCC_D2CFGR_D2PPRE1) != 0U) {
        hz *= 2U;
    }
    return hz;
}

int PortDout_Init(uint32_t freq_hz)
{
    if (freq_hz < PORT_DOUT_FREQ_MIN_HZ) { freq_hz = PORT_DOUT_FREQ_MIN_HZ; }
    if (freq_hz > PORT_DOUT_FREQ_MAX_HZ) { freq_hz = PORT_DOUT_FREQ_MAX_HZ; }

    uint32_t kernel = dout_kernel_hz();
    uint32_t ticks  = freq_hz * PORT_DOUT_STEPS;
    if (ticks == 0U || kernel / ticks == 0U) {
        return 0;
    }
    uint32_t psc = (kernel / ticks) - 1U;

    if (!dout_ready) {
        dout_gpio_init();
        __HAL_RCC_TIM7_CLK_ENABLE();
    }

    htim_dout.Instance           = DOUT_TIM;
    htim_dout.Init.Prescaler     = psc;
    htim_dout.Init.CounterMode   = TIM_COUNTERMODE_UP;
    htim_dout.Init.Period        = 0U;   /* one interrupt per prescaled tick */
    htim_dout.Init.AutoReloadPreload = TIM_AUTORELOAD_PRELOAD_DISABLE;
    if (HAL_TIM_Base_Init(&htim_dout) != HAL_OK) {
        return 0;
    }

    dout_actual_hz = kernel / (psc + 1U) / PORT_DOUT_STEPS;

    if (!dout_ready) {
        /* Below the HAL time base (TIM6, priority 15) so a burst of these can
         * never starve HAL_GetTick(), which every session's period depends on. */
        HAL_NVIC_SetPriority(TIM7_IRQn, 14, 0);
        HAL_NVIC_EnableIRQ(TIM7_IRQn);
        if (HAL_TIM_Base_Start_IT(&htim_dout) != HAL_OK) {
            return 0;
        }
        dout_ready = 1;
    }
    return 1;
}

void PortDout_SetDuty(int ch, uint32_t duty_pct)
{
    if (ch < 1 || ch > PORT_DOUT_COUNT) {
        return;
    }
    if (duty_pct > 100U) {
        duty_pct = 100U;
    }
    dout_duty[ch - 1] = (uint8_t)duty_pct;

    /* A channel that is fully off or fully on is settled here rather than left
     * to the interrupt: with duty 0 or 100 the comparison below never changes
     * the pin again, so without this the pin would keep whatever level the
     * last switched period happened to end on. */
    if (duty_pct == 0U) {
        HAL_GPIO_WritePin(port_dout_pins[ch - 1].port,
                          port_dout_pins[ch - 1].pin, GPIO_PIN_RESET);
    } else if (duty_pct >= 100U) {
        HAL_GPIO_WritePin(port_dout_pins[ch - 1].port,
                          port_dout_pins[ch - 1].pin, GPIO_PIN_SET);
    }
}

void PortDout_AllOff(void)
{
    for (int i = 0; i < PORT_DOUT_COUNT; i++) {
        dout_duty[i] = 0U;
        HAL_GPIO_WritePin(port_dout_pins[i].port, port_dout_pins[i].pin,
                          GPIO_PIN_RESET);
    }
}

void PortDout_Stop(void)
{
    if (dout_ready) {
        (void)HAL_TIM_Base_Stop_IT(&htim_dout);
        HAL_NVIC_DisableIRQ(TIM7_IRQn);
        dout_ready = 0;
    }
    PortDout_AllOff();
}

uint32_t PortDout_ActualFreqHz(void) { return dout_actual_hz; }

/* One pass per PWM step. Deliberately not HAL_TIM_IRQHandler and a callback:
 * at up to 200 kHz the flag is cleared and the pins written directly, because
 * the HAL path walks every interrupt source on the timer each time. */
void TIM7_IRQHandler(void)
{
    DOUT_TIM->SR = 0U;

    uint8_t phase = dout_phase;
    if (++phase >= PORT_DOUT_STEPS) {
        phase = 0U;
    }
    dout_phase = phase;

    for (int i = 0; i < PORT_DOUT_COUNT; i++) {
        uint8_t duty = dout_duty[i];
        /* 0 and 100 are held by PortDout_SetDuty, so leave them alone here and
         * let a steady output stay steady. */
        if (duty == 0U || duty >= PORT_DOUT_STEPS) {
            continue;
        }
        if (phase < duty) {
            port_dout_pins[i].port->BSRR = port_dout_pins[i].pin;
        } else {
            port_dout_pins[i].port->BSRR = (uint32_t)port_dout_pins[i].pin << 16;
        }
    }
}

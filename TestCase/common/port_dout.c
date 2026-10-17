// port_dout.c
//
// Eight independent software PWM outputs - see port_dout.h.

#include "port_dout.h"

#include <string.h>

/* TIM7: a basic timer nothing else uses (TIM6 is the HAL time base). Its
 * vector is weak in the startup file, so the handler here overrides it. */
#define DOUT_TIM TIM7

const port_dout_t port_dout_pins[PORT_DOUT_COUNT] = {
    { GPIOB, GPIO_PIN_13 },   /* Digital Out 1, terminal A03 */
    { GPIOB, GPIO_PIN_0  },   /* Digital Out 2, terminal A04 */
    { GPIOH, GPIO_PIN_15 },   /* Digital Out 3, terminal A05 */
    { GPIOE, GPIO_PIN_4  },   /* Digital Out 4, terminal A06 */
    { GPIOA, GPIO_PIN_8  },   /* Digital Out 5, terminal A07 */
    { GPIOA, GPIO_PIN_9  },   /* Digital Out 6, terminal A08 */
    { GPIOI, GPIO_PIN_7  },   /* Digital Out 7, terminal A09 */
    { GPIOE, GPIO_PIN_5  },   /* Digital Out 8, terminal A10 */
};

static TIM_HandleTypeDef htim_dout;
static uint32_t dout_tick_hz;                        /* interrupt rate */
static uint32_t dout_want_hz[PORT_DOUT_COUNT];       /* what was asked for */
static uint32_t dout_actual_hz[PORT_DOUT_COUNT];     /* what the maths lands on */
static uint32_t dout_res = PORT_DOUT_RES_DEF;
static int      dout_ready;

/* Read by the interrupt, written by the main loop. One aligned word per
 * channel and a single store per write, so a duty change lands whole. */
static volatile uint32_t dout_duty[PORT_DOUT_COUNT];
static volatile uint32_t dout_full = (1UL << PORT_DOUT_RES_DEF) - 1UL;

/* Per-channel phase accumulators: each adds its own increment per interrupt
 * and wraps on its own, so each channel has its own frequency; the output is
 * high while the accumulator is below the threshold. 32 bits so a slow channel
 * beside a fast one does not round to an increment of zero. */
static volatile uint32_t dout_acc[PORT_DOUT_COUNT];
static volatile uint32_t dout_inc[PORT_DOUT_COUNT];
static volatile uint32_t dout_thresh[PORT_DOUT_COUNT];

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
 * prescaler other than 1. */
static uint32_t dout_kernel_hz(void)
{
    uint32_t hz = HAL_RCC_GetPCLK1Freq();
    if ((RCC->D2CFGR & RCC_D2CFGR_D2PPRE1) != 0U) {
        hz *= 2U;
    }
    return hz;
}

/* Recomputes every channel's increment for the interrupt rate now in force.
 * Called after anything that moves either the rate or a wanted frequency. */
static void dout_recompute(void)
{
    for (int i = 0; i < PORT_DOUT_COUNT; i++) {
        /* Rounded both ways, so 1 Hz is not reported as 0. */
        uint64_t num = ((uint64_t)dout_want_hz[i] << 32) + (dout_tick_hz / 2U);
        uint32_t inc = (uint32_t)(num / dout_tick_hz);
        if (inc == 0U) { inc = 1U; }
        dout_inc[i] = inc;
        /* What that increment really produces, which is what gets reported. */
        dout_actual_hz[i] = (uint32_t)
            ((((uint64_t)inc * dout_tick_hz) + 0x80000000ULL) >> 32);
    }
}

/* The fastest channel sets the interrupt rate: 2^res interrupts per period. */
static uint32_t dout_wanted_tick_hz(void)
{
    uint32_t top = PORT_DOUT_FREQ_MIN_HZ;
    for (int i = 0; i < PORT_DOUT_COUNT; i++) {
        if (dout_want_hz[i] > top) { top = dout_want_hz[i]; }
    }
    return top << dout_res;
}

/* Programs the timer for the rate the current frequencies need. */
static int dout_program(void)
{
    uint32_t kernel = dout_kernel_hz();
    uint32_t ticks  = dout_wanted_tick_hz();
    if (ticks == 0U || kernel / ticks == 0U) {
        return 0;
    }
    uint32_t psc = (kernel / ticks) - 1U;

    htim_dout.Instance           = DOUT_TIM;
    htim_dout.Init.Prescaler     = psc;
    htim_dout.Init.CounterMode   = TIM_COUNTERMODE_UP;
    htim_dout.Init.Period        = 0U;   /* one interrupt per prescaled tick */
    htim_dout.Init.AutoReloadPreload = TIM_AUTORELOAD_PRELOAD_DISABLE;
    if (HAL_TIM_Base_Init(&htim_dout) != HAL_OK) {
        return 0;
    }
    dout_tick_hz = kernel / (psc + 1U);
    dout_recompute();
    return 1;
}

int PortDout_Init(void)
{
    if (!dout_ready) {
        dout_gpio_init();
        __HAL_RCC_TIM7_CLK_ENABLE();
        for (int i = 0; i < PORT_DOUT_COUNT; i++) {
            dout_want_hz[i] = PORT_DOUT_FREQ_DEF_HZ;
        }
    }

    if (!dout_program()) {
        return 0;
    }

    if (!dout_ready) {
        /* Priority 14 preempts the HAL time base (TIM6, TICK_INT_PRIORITY 15). */
        HAL_NVIC_SetPriority(TIM7_IRQn, 14, 0);
        HAL_NVIC_EnableIRQ(TIM7_IRQn);
        if (HAL_TIM_Base_Start_IT(&htim_dout) != HAL_OK) {
            return 0;
        }
        dout_ready = 1;
    }
    return 1;
}

int PortDout_SetRes(uint32_t res)
{
    if (res < PORT_DOUT_RES_MIN || res > PORT_DOUT_RES_MAX) {
        return 0;
    }
    if (res == dout_res) {
        return 1;               /* no glitch on a pt.set that repeats res */
    }
    for (int i = 0; i < PORT_DOUT_COUNT; i++) {
        if (dout_want_hz[i] > PORT_DOUT_FREQ_MAX_HZ(res)) {
            return 0;
        }
    }

    uint32_t was = dout_res;
    dout_res = res;
    if (dout_ready && !dout_program()) {
        dout_res = was;
        (void)dout_program();
        return 0;
    }
    /* Duties are in the old unit until set again: park every channel low. */
    PortDout_AllOff();
    dout_full = (1UL << res) - 1UL;
    return 1;
}

uint32_t PortDout_Res(void) { return dout_res; }

int PortDout_SetFreq(int ch, uint32_t freq_hz)
{
    if (ch < 1 || ch > PORT_DOUT_COUNT) {
        return 0;
    }
    if (freq_hz < PORT_DOUT_FREQ_MIN_HZ) { freq_hz = PORT_DOUT_FREQ_MIN_HZ; }
    if (freq_hz > PORT_DOUT_FREQ_MAX_HZ(dout_res)) {
        freq_hz = PORT_DOUT_FREQ_MAX_HZ(dout_res);
    }

    uint32_t was = dout_want_hz[ch - 1];
    dout_want_hz[ch - 1] = freq_hz;

    if (!dout_ready) {
        return 1;               /* takes effect when the timer starts */
    }
    if (!dout_program()) {
        dout_want_hz[ch - 1] = was;
        (void)dout_program();
        return 0;
    }
    return 1;
}

void PortDout_SetDuty(int ch, uint32_t duty)
{
    if (ch < 1 || ch > PORT_DOUT_COUNT) {
        return;
    }
    uint32_t full = dout_full;
    if (duty > full) {
        duty = full;
    }
    /* Threshold before duty: the interrupt only compares once duty says the
     * channel is switching, so it never sees a new duty with an old threshold. */
    dout_thresh[ch - 1] = (uint32_t)((uint64_t)duty << (32U - dout_res));
    dout_duty[ch - 1] = duty;

    /* 0 and full are settled here: the interrupt leaves those pins alone. */
    if (duty == 0U) {
        HAL_GPIO_WritePin(port_dout_pins[ch - 1].port,
                          port_dout_pins[ch - 1].pin, GPIO_PIN_RESET);
    } else if (duty >= full) {
        HAL_GPIO_WritePin(port_dout_pins[ch - 1].port,
                          port_dout_pins[ch - 1].pin, GPIO_PIN_SET);
    }
}

void PortDout_AllOff(void)
{
    for (int i = 0; i < PORT_DOUT_COUNT; i++) {
        dout_duty[i] = 0U;
        dout_thresh[i] = 0U;
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

uint32_t PortDout_ActualFreqHz(int ch)
{
    if (ch < 1 || ch > PORT_DOUT_COUNT) {
        return 0;
    }
    return dout_actual_hz[ch - 1];
}

uint32_t PortDout_TickHz(void) { return dout_tick_hz; }

/* One pass per resolution step, up to 256 kHz: registers directly, not
 * HAL_TIM_IRQHandler, which walks every interrupt source each time. */
void TIM7_IRQHandler(void)
{
    DOUT_TIM->SR = 0U;

    for (int i = 0; i < PORT_DOUT_COUNT; i++) {
        uint32_t acc = dout_acc[i] + dout_inc[i];   /* wraps: that is the period */
        dout_acc[i] = acc;

        uint32_t duty = dout_duty[i];
        /* 0 and full are held by PortDout_SetDuty. */
        if (duty == 0U || duty >= dout_full) {
            continue;
        }
        if (acc < dout_thresh[i]) {
            port_dout_pins[i].port->BSRR = port_dout_pins[i].pin;
        } else {
            port_dout_pins[i].port->BSRR = (uint32_t)port_dout_pins[i].pin << 16;
        }
    }
}

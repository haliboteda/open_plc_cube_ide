// port_adc.c
//
// Shared ADC access - see port_adc.h.

// Enables the ADC HAL module for this translation unit only (see
// stm32h7xx_hal_adc.c in TestCase/common for why) - must come before main.h
// pulls in stm32h7xx_hal.h, which is what conditionally declares
// ADC_HandleTypeDef/HAL_ADC_* based on this macro.
#include "testcase_hal_guard.h"   /* fires if this peripheral becomes real -- read it */
#define HAL_ADC_MODULE_ENABLED

#include "port_adc.h"
#include "main.h"

#define ADC_RESOLUTION_BITS   ADC_RESOLUTION_16B
#define ADC_FULL_SCALE        65535U
#define ADC_VREF_FALLBACK_MV  3300U
#define ADC_VREF_SAMPLES      16U

static ADC_HandleTypeDef hadc1;   /* PA6 (AIN2), PA0 (T-PS), PA3 (T-HS) */
static ADC_HandleTypeDef hadc3;   /* PC3_C (AIN1), VREFINT */

static uint32_t adc_vdda_mv = ADC_VREF_FALLBACK_MV;
static int      adc_vdda_measured;
static uint32_t adc_vrefint_raw;
static int      adc_ready;

static void PortAdc_GpioInit(void)
{
    __HAL_RCC_GPIOA_CLK_ENABLE();

    GPIO_InitTypeDef gpio = {0};
    gpio.Mode = GPIO_MODE_ANALOG;
    gpio.Pull = GPIO_NOPULL;
    gpio.Pin  = GPIO_PIN_0 | GPIO_PIN_3 | GPIO_PIN_6;
    HAL_GPIO_Init(GPIOA, &gpio);

    /* PC3_C is a dedicated analog pad - no GPIO config reaches it. Its only
     * control is the SYSCFG analog switch, handled per read. */
}

static int PortAdc_InitInstance(ADC_HandleTypeDef *h, ADC_TypeDef *instance)
{
    h->Instance = instance;
    h->Init.ClockPrescaler           = ADC_CLOCK_ASYNC_DIV4;
    h->Init.Resolution               = ADC_RESOLUTION_BITS;
    h->Init.ScanConvMode             = ADC_SCAN_DISABLE;
    h->Init.EOCSelection             = ADC_EOC_SINGLE_CONV;
    h->Init.ContinuousConvMode       = DISABLE;
    h->Init.NbrOfConversion          = 1;
    h->Init.DiscontinuousConvMode    = DISABLE;
    h->Init.ExternalTrigConv         = ADC_SOFTWARE_START;
    h->Init.ExternalTrigConvEdge     = ADC_EXTERNALTRIGCONVEDGE_NONE;
    h->Init.ConversionDataManagement = ADC_CONVERSIONDATA_DR;
    h->Init.Overrun                  = ADC_OVR_DATA_OVERWRITTEN;
    h->Init.LeftBitShift             = ADC_LEFTBITSHIFT_NONE;
    h->Init.OversamplingMode         = DISABLE;

    if (HAL_ADC_Init(h) != HAL_OK) {
        return 0;
    }
    if (HAL_ADCEx_Calibration_Start(h, ADC_CALIB_OFFSET, ADC_SINGLE_ENDED) != HAL_OK) {
        return 0;
    }
    return 1;
}

static int PortAdc_ReadRaw(ADC_HandleTypeDef *h, uint32_t channel, uint32_t *value)
{
    ADC_ChannelConfTypeDef cfg = {0};
    cfg.Channel      = channel;
    cfg.Rank         = ADC_REGULAR_RANK_1;
    /* The LM50 drives the sampling cap directly - no filter cap on the net -
     * so the sampling window has to be long or the reading comes out low. */
    cfg.SamplingTime = ADC_SAMPLETIME_387CYCLES_5;
    cfg.SingleDiff   = ADC_SINGLE_ENDED;
    cfg.OffsetNumber = ADC_OFFSET_NONE;
    cfg.Offset       = 0;

    if (HAL_ADC_ConfigChannel(h, &cfg) != HAL_OK) {
        return 0;
    }
    if (HAL_ADC_Start(h) != HAL_OK) {
        return 0;
    }
    if (HAL_ADC_PollForConversion(h, 10) != HAL_OK) {
        HAL_ADC_Stop(h);
        return 0;
    }
    *value = HAL_ADC_GetValue(h);
    HAL_ADC_Stop(h);
    return 1;
}

static uint32_t PortAdc_ToMillivolts(uint32_t raw)
{
    return (raw * adc_vdda_mv) / ADC_FULL_SCALE;
}

/* Averaged, because this one number scales every reading taken against it - a
 * single VREFINT sample carries its own noise straight into every millivolt,
 * both temperatures and every DAC code. */
static void PortAdc_MeasureVdda(void)
{
    uint32_t sum = 0, taken = 0;

    for (uint32_t i = 0; i < ADC_VREF_SAMPLES; i++) {
        uint32_t raw = 0;
        if (PortAdc_ReadRaw(&hadc3, ADC_CHANNEL_VREFINT, &raw) && raw != 0U) {
            sum += raw;
            taken++;
        }
    }
    if (taken == 0U) {
        return;
    }

    adc_vrefint_raw = sum / taken;
    adc_vdda_mv = __LL_ADC_CALC_VREFANALOG_VOLTAGE(adc_vrefint_raw, ADC_RESOLUTION_BITS);
    adc_vdda_measured = 1;
}

int PortAdc_Init(void)
{
    if (adc_ready) {
        return 1;
    }

    RCC_PeriphCLKInitTypeDef periph = {0};
    periph.PeriphClockSelection = RCC_PERIPHCLK_ADC;
    periph.AdcClockSelection    = RCC_ADCCLKSOURCE_CLKP;
    if (HAL_RCCEx_PeriphCLKConfig(&periph) != HAL_OK) {
        return 0;
    }

    __HAL_RCC_SYSCFG_CLK_ENABLE();
    __HAL_RCC_ADC12_CLK_ENABLE();
    __HAL_RCC_ADC3_CLK_ENABLE();

    PortAdc_GpioInit();

    if (!PortAdc_InitInstance(&hadc1, ADC1)) {
        return 0;
    }
    if (!PortAdc_InitInstance(&hadc3, ADC3)) {
        return 0;
    }

    PortAdc_MeasureVdda();
    adc_ready = 1;
    return 1;
}

uint32_t PortAdc_VddaMv(void)      { return adc_vdda_mv; }
uint32_t PortAdc_VrefintRaw(void)  { return adc_vrefint_raw; }
int      PortAdc_VddaMeasured(void) { return adc_vdda_measured; }

int PortAdc_VddaTrusted(void)
{
    return adc_vdda_measured &&
           adc_vdda_mv >= PORT_ADC_VDDA_MIN_MV &&
           adc_vdda_mv <= PORT_ADC_VDDA_MAX_MV;
}

int PortAdc_ReadAin(int ch, uint32_t *raw, uint32_t *mv)
{
    uint32_t v = 0;
    int ok;

    if (ch == 1) {
        /* SYSCFG_PMCR.PC3SO decides whether the analog pad is also tied to the
         * digital PC3 cell. Reset default is tied; analog sampling wants it
         * open, so set it every read rather than relying on init order. */
        SYSCFG->PMCR |= SYSCFG_PMCR_PC3SO;
        ok = PortAdc_ReadRaw(&hadc3, ADC_CHANNEL_1, &v);
    } else if (ch == 2) {
        ok = PortAdc_ReadRaw(&hadc1, ADC_CHANNEL_3, &v);
    } else {
        return 0;
    }

    if (!ok) {
        return 0;
    }
    if (raw) { *raw = v; }
    if (mv)  { *mv  = PortAdc_ToMillivolts(v); }
    return 1;
}

int PortAdc_ReadAin1SwitchClosed(uint32_t *raw)
{
    uint32_t v = 0;

    SYSCFG->PMCR &= ~SYSCFG_PMCR_PC3SO;
    int ok = PortAdc_ReadRaw(&hadc3, ADC_CHANNEL_1, &v);
    SYSCFG->PMCR |= SYSCFG_PMCR_PC3SO;

    if (!ok) {
        return 0;
    }
    if (raw) { *raw = v; }
    return 1;
}

int PortAdc_ReadTemp(int ch, uint32_t *mv, int32_t *decic)
{
    uint32_t channel;
    uint32_t v = 0;

    if (ch == 1) {
        channel = ADC_CHANNEL_16;   /* PA0, short-circuit protection */
    } else if (ch == 2) {
        channel = ADC_CHANNEL_15;   /* PA3, high-side FETs */
    } else {
        return 0;
    }

    if (!PortAdc_ReadRaw(&hadc1, channel, &v)) {
        return 0;
    }

    uint32_t millivolts = PortAdc_ToMillivolts(v);
    if (mv) { *mv = millivolts; }
    if (decic) {
        *decic = ((int32_t)millivolts - PORT_TEMP_OFFSET_MV) * 10 / PORT_TEMP_MV_PER_DEGC;
    }
    return 1;
}

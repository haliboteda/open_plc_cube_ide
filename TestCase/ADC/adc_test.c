// adc_test.c
//
// Board bring-up cases 3 and 11 - see adc_test.h.

// Enables the ADC HAL module for this translation unit only (see
// stm32h7xx_hal_adc.c in TestCase/common for why) - must come before main.h
// pulls in stm32h7xx_hal.h, which is what conditionally declares
// ADC_HandleTypeDef/HAL_ADC_* based on this macro.
#include "testcase_hal_guard.h"   /* fires if this peripheral becomes real -- read it */
#define HAL_ADC_MODULE_ENABLED

#include "adc_test.h"
#include "main.h"
#include <stdio.h>

#define ADC_TEST_RESOLUTION_BITS  ADC_RESOLUTION_16B
#define ADC_TEST_FULL_SCALE       65535U
#define ADC_TEST_VREF_FALLBACK_MV 3300U

#define ADC_TEST_AIN_PERIOD_MS    1000U
#define ADC_TEST_TEMP_PERIOD_MS   3000U

/* LM50: V = 10mV/degC * T + 500mV */
#define TEMP_OFFSET_MV            500
#define TEMP_MV_PER_DEGC          10
#define TEMP_VALID_MIN_DECIC      (-250)   /* -25.0 degC, sensor spec floor */
#define TEMP_VALID_MAX_DECIC      (1000)   /* +100.0 degC, sensor spec ceiling */

static ADC_HandleTypeDef hadc1;   /* PA6 (AIN2), PA0 (T-PS), PA3 (T-HS) */
static ADC_HandleTypeDef hadc3;   /* PC3_C (AIN1), VREFINT */

/* VDDA outside this band means VREF+ is not being held up, and every reading
 * below it is meaningless - worth saying out loud, because the symptom
 * otherwise looks like a broken ADC rather than a missing supply. */
#define ADC_TEST_VDDA_MIN_MV      2000U
#define ADC_TEST_VDDA_MAX_MV      3600U

static uint32_t adc_vdda_mv = ADC_TEST_VREF_FALLBACK_MV;
static int      adc_vdda_measured;
static uint32_t adc_vrefint_raw;
static uint32_t adc_ain_due_ms;
static uint32_t adc_temp_due_ms;
static int      adc_ready;

static void ADC_Test_GPIO_Init(void)
{
    __HAL_RCC_GPIOA_CLK_ENABLE();

    GPIO_InitTypeDef gpio = {0};
    gpio.Mode = GPIO_MODE_ANALOG;
    gpio.Pull = GPIO_NOPULL;
    gpio.Pin  = GPIO_PIN_0 | GPIO_PIN_3 | GPIO_PIN_6;
    HAL_GPIO_Init(GPIOA, &gpio);

    /* PC3_C is a dedicated analog pad - no GPIO config reaches it. Its only
     * control is the SYSCFG analog switch, handled in ADC_Test_ReadAin1. */
}

static int ADC_Test_InitInstance(ADC_HandleTypeDef *h, ADC_TypeDef *instance)
{
    h->Instance = instance;
    h->Init.ClockPrescaler           = ADC_CLOCK_ASYNC_DIV4;
    h->Init.Resolution               = ADC_TEST_RESOLUTION_BITS;
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

static int ADC_Test_Read(ADC_HandleTypeDef *h, uint32_t channel, uint32_t *value)
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

static uint32_t ADC_Test_ToMillivolts(uint32_t raw)
{
    return (raw * adc_vdda_mv) / ADC_TEST_FULL_SCALE;
}

uint32_t ADC_Test_GetVrefMv(void)
{
    return adc_vdda_measured ? adc_vdda_mv : 0U;
}

static int ADC_Test_VddaTrusted(void)
{
    return adc_vdda_measured &&
           adc_vdda_mv >= ADC_TEST_VDDA_MIN_MV &&
           adc_vdda_mv <= ADC_TEST_VDDA_MAX_MV;
}

/* PC3_C carries Analog In 1. SYSCFG_PMCR.PC3SO decides whether that pad is
 * also tied to the digital PC3 cell (reset default: tied). Which state this
 * board wants has never been measured, so read it both ways and print both -
 * one power-up settles the question instead of guessing. */
static int ADC_Test_ReadAin1(uint32_t *raw_switch_closed, uint32_t *raw_switch_open)
{
    int ok;

    SYSCFG->PMCR &= ~SYSCFG_PMCR_PC3SO;
    ok = ADC_Test_Read(&hadc3, ADC_CHANNEL_1, raw_switch_closed);

    SYSCFG->PMCR |= SYSCFG_PMCR_PC3SO;
    ok &= ADC_Test_Read(&hadc3, ADC_CHANNEL_1, raw_switch_open);

    return ok;
}

/* Averaged, because this one number scales every reading the test prints -
 * a single VREFINT sample carries its own noise straight into the ADC
 * millivolts, both temperatures and every DAC code. */
#define ADC_TEST_VREF_SAMPLES 16U

static void ADC_Test_MeasureVdda(void)
{
    uint32_t sum = 0, taken = 0;

    for (uint32_t i = 0; i < ADC_TEST_VREF_SAMPLES; i++) {
        uint32_t raw = 0;
        if (ADC_Test_Read(&hadc3, ADC_CHANNEL_VREFINT, &raw) && raw != 0U) {
            sum += raw;
            taken++;
        }
    }
    if (taken == 0U) {
        return;
    }

    adc_vrefint_raw = sum / taken;
    adc_vdda_mv = __LL_ADC_CALC_VREFANALOG_VOLTAGE(adc_vrefint_raw,
                                                   ADC_TEST_RESOLUTION_BITS);
    adc_vdda_measured = 1;
}

int ADC_Test_Init(void)
{
    RCC_PeriphCLKInitTypeDef periph = {0};
    periph.PeriphClockSelection = RCC_PERIPHCLK_ADC;
    periph.AdcClockSelection    = RCC_ADCCLKSOURCE_CLKP;
    if (HAL_RCCEx_PeriphCLKConfig(&periph) != HAL_OK) {
        return 0;
    }

    __HAL_RCC_SYSCFG_CLK_ENABLE();
    __HAL_RCC_ADC12_CLK_ENABLE();
    __HAL_RCC_ADC3_CLK_ENABLE();

    ADC_Test_GPIO_Init();

    if (!ADC_Test_InitInstance(&hadc1, ADC1)) {
        return 0;
    }
    if (!ADC_Test_InitInstance(&hadc3, ADC3)) {
        return 0;
    }

    ADC_Test_MeasureVdda();
    printf("[ADC] VREF+ measured %lu mV (%s, VREFINT raw=%lu)\r\n",
           (unsigned long)adc_vdda_mv,
           adc_vdda_measured ? "via VREFINT" : "VREFINT read failed, assumed",
           (unsigned long)adc_vrefint_raw);

    if (adc_vdda_measured && !ADC_Test_VddaTrusted()) {
        printf("[ADC] !! VREF+ IS OUT OF RANGE - every reading below is meaningless.\r\n"
               "[ADC] !! This board carries no external reference, so VREF+ exists\r\n"
               "[ADC] !! only while the MCU's own VREFBUF drives that pin. A\r\n"
               "[ADC] !! full-scale VREFINT reading means it is below 1.216 V.\r\n");
    }

    printf("[ADC] Analog In 1 = voltage range (JP9 2-3 + JP5 1-2 bridged),\r\n"
           "[ADC] Analog In 2 = current range (JP8 1-2 + JP6 2-3 bridged).\r\n"
           "[ADC] Readings below are the raw conversion and the pin voltage only -\r\n"
           "[ADC] no front-end scaling is applied to them.\r\n");

    adc_ready       = 1;
    adc_ain_due_ms  = HAL_GetTick();
    adc_temp_due_ms = HAL_GetTick();
    return 1;
}

void ADC_Test_TickAnalogIn(uint32_t now_ms)
{
    if (!adc_ready || (int32_t)(now_ms - adc_ain_due_ms) < 0) {
        return;
    }
    adc_ain_due_ms = now_ms + ADC_TEST_AIN_PERIOD_MS;

    uint32_t raw1_closed = 0, raw1_open = 0, raw2 = 0;
    int ok1 = ADC_Test_ReadAin1(&raw1_closed, &raw1_open);
    int ok2 = ADC_Test_Read(&hadc1, ADC_CHANNEL_3, &raw2);

    if (!ok1 || !ok2) {
        printf("[T3 ] Analog In: the ADC did not return a reading (%s / %s)\r\n",
               ok1 ? "input 1 ok" : "input 1 failed",
               ok2 ? "input 2 ok" : "input 2 failed");
        return;
    }

    /* Straight readout, no front-end maths - just the converter result and the
     * pin voltage it corresponds to. */
    printf("[T3 ] Analog In 1 (PC3_C, voltage range): raw %lu, %lu mV at the pin"
           "  [switch closed: raw %lu]\r\n",
           (unsigned long)raw1_open,
           (unsigned long)ADC_Test_ToMillivolts(raw1_open),
           (unsigned long)raw1_closed);
    printf("[T3 ] Analog In 2 (PA6,   current range): raw %lu, %lu mV at the pin\r\n",
           (unsigned long)raw2,
           (unsigned long)ADC_Test_ToMillivolts(raw2));
}

static int32_t ADC_Test_ToDeciCelsius(uint32_t mv)
{
    return ((int32_t)mv - TEMP_OFFSET_MV) * 10 / TEMP_MV_PER_DEGC;
}

/* Sign carried separately: integer division loses it for -0.9..-0.1 degC. */
static void ADC_Test_FormatTemp(char *out, size_t len, int32_t decic)
{
    const char *sign = (decic < 0) ? "-" : "";
    int32_t mag = (decic < 0) ? -decic : decic;
    snprintf(out, len, "%s%ld.%01ld", sign, (long)(mag / 10), (long)(mag % 10));
}

void ADC_Test_TickTemperature(uint32_t now_ms)
{
    if (!adc_ready || (int32_t)(now_ms - adc_temp_due_ms) < 0) {
        return;
    }
    adc_temp_due_ms = now_ms + ADC_TEST_TEMP_PERIOD_MS;

    uint32_t raw_ps = 0, raw_hs = 0;
    int ok_ps = ADC_Test_Read(&hadc1, ADC_CHANNEL_16, &raw_ps);   /* PA0 */
    int ok_hs = ADC_Test_Read(&hadc1, ADC_CHANNEL_15, &raw_hs);   /* PA3 */

    if (!ok_ps || !ok_hs) {
        printf("[T11] Temperature: the ADC did not return a reading\r\n");
        return;
    }

    uint32_t mv_ps = ADC_Test_ToMillivolts(raw_ps);
    uint32_t mv_hs = ADC_Test_ToMillivolts(raw_hs);
    int32_t decic_ps = ADC_Test_ToDeciCelsius(mv_ps);
    int32_t decic_hs = ADC_Test_ToDeciCelsius(mv_hs);

    char ps[12], hs[12];
    ADC_Test_FormatTemp(ps, sizeof(ps), decic_ps);
    ADC_Test_FormatTemp(hs, sizeof(hs), decic_hs);

    /* A temperature computed from a collapsed VREF+ can still land inside the
     * plausible band by luck. Saying "sensible" then would be a false pass. */
    const char *verdict;
    if (!ADC_Test_VddaTrusted()) {
        verdict = "CANNOT BE TRUSTED, VREF+ is wrong";
    } else if (decic_ps >= TEMP_VALID_MIN_DECIC && decic_ps <= TEMP_VALID_MAX_DECIC &&
               decic_hs >= TEMP_VALID_MIN_DECIC && decic_hs <= TEMP_VALID_MAX_DECIC) {
        verdict = "both look sensible";
    } else {
        verdict = "OUT OF RANGE, so the sensor or the reference is wrong";
    }

    printf("[T11] Board temperature: PA0 input side %lu mV = %s C, "
           "PA3 output side %lu mV = %s C - %s\r\n",
           (unsigned long)mv_ps, ps, (unsigned long)mv_hs, hs, verdict);
}

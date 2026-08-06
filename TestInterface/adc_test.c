// adc_test.c
//
// Standalone ADC bring-up test - see adc_test.h.
//
// Tests the external Analog IN 1/2 inputs (PA6/PC3, Klemmblock D) by
// injecting a known DC voltage - e.g. one AA battery (~1.5V) across the
// input terminal and GND - and comparing what the ADC converts against
// ADC_TEST_INJECTED_MV below (which you set to whatever a multimeter
// reads across the battery). The printed ratio tells you whether this
// input is a direct connection (ratio ~1.00) or has an unknown divider
// ahead of it (ratio != 1.00, e.g. 0.50 for a 2:1 divider).
//
// Do NOT inject into PA0/PA3 (on-board TEMP_SCPROT/TEMP_HSSW sensors) -
// those pins already have a sensor chip actively driving them.

// Enables the ADC HAL module for this translation unit only (see
// stm32h7xx_hal_adc.c in this same folder for why) - must come before
// main.h pulls in stm32h7xx_hal.h, which is what conditionally declares
// ADC_HandleTypeDef/HAL_ADC_* based on this macro.
#define HAL_ADC_MODULE_ENABLED

#include "adc_test.h"
#include "main.h"
#include <stdio.h>

#define ADC_TEST_VREF_MV        3300U   /* approximate, not VREFINT-calibrated */
#define ADC_TEST_FULL_SCALE     65535U  /* 16-bit resolution */
#define ADC_TEST_PERIOD_MS      1000U

/* Set this to whatever your multimeter reads across the injected battery
 * before powering on - only used to print the measured/expected ratio,
 * does not affect the ADC conversion itself. */
#define ADC_TEST_INJECTED_MV    1500U

#define ADC_TEST_CH_AIN1        ADC_CHANNEL_3   /* PA6 - external Analog IN 1 (Klemmblock D) */
#define ADC_TEST_CH_AIN2        ADC_CHANNEL_13  /* PC3 - external Analog IN 2 (Klemmblock D) */

static ADC_HandleTypeDef hadc1;

static void ADC_Test_GPIO_Init(void)
{
    __HAL_RCC_GPIOA_CLK_ENABLE();
    __HAL_RCC_GPIOC_CLK_ENABLE();

    GPIO_InitTypeDef gpio = {0};
    gpio.Mode = GPIO_MODE_ANALOG;
    gpio.Pull = GPIO_NOPULL;

    gpio.Pin = GPIO_PIN_6;
    HAL_GPIO_Init(GPIOA, &gpio);

    gpio.Pin = GPIO_PIN_3;
    HAL_GPIO_Init(GPIOC, &gpio);
}

static int ADC_Test_Init(void)
{
    RCC_PeriphCLKInitTypeDef periphClk = {0};
    periphClk.PeriphClockSelection = RCC_PERIPHCLK_ADC;
    periphClk.AdcClockSelection = RCC_ADCCLKSOURCE_CLKP;
    if (HAL_RCCEx_PeriphCLKConfig(&periphClk) != HAL_OK) {
        return 0;
    }

    __HAL_RCC_ADC12_CLK_ENABLE();

    hadc1.Instance = ADC1;
    hadc1.Init.ClockPrescaler = ADC_CLOCK_ASYNC_DIV4;
    hadc1.Init.Resolution = ADC_RESOLUTION_16B;
    hadc1.Init.ScanConvMode = ADC_SCAN_DISABLE;
    hadc1.Init.EOCSelection = ADC_EOC_SINGLE_CONV;
    hadc1.Init.ContinuousConvMode = DISABLE;
    hadc1.Init.NbrOfConversion = 1;
    hadc1.Init.DiscontinuousConvMode = DISABLE;
    hadc1.Init.ExternalTrigConv = ADC_SOFTWARE_START;
    hadc1.Init.ExternalTrigConvEdge = ADC_EXTERNALTRIGCONVEDGE_NONE;
    hadc1.Init.ConversionDataManagement = ADC_CONVERSIONDATA_DR;
    hadc1.Init.Overrun = ADC_OVR_DATA_OVERWRITTEN;
    hadc1.Init.LeftBitShift = ADC_LEFTBITSHIFT_NONE;
    hadc1.Init.OversamplingMode = DISABLE;
    if (HAL_ADC_Init(&hadc1) != HAL_OK) {
        return 0;
    }

    return 1;
}

static int ADC_Test_ReadChannel(uint32_t channel, uint32_t *value)
{
    ADC_ChannelConfTypeDef sConfig = {0};
    sConfig.Channel = channel;
    sConfig.Rank = ADC_REGULAR_RANK_1;
    sConfig.SamplingTime = ADC_SAMPLETIME_387CYCLES_5;
    sConfig.SingleDiff = ADC_SINGLE_ENDED;
    sConfig.OffsetNumber = ADC_OFFSET_NONE;
    sConfig.Offset = 0;
    if (HAL_ADC_ConfigChannel(&hadc1, &sConfig) != HAL_OK) {
        return 0;
    }

    if (HAL_ADC_Start(&hadc1) != HAL_OK) {
        return 0;
    }
    if (HAL_ADC_PollForConversion(&hadc1, 10) != HAL_OK) {
        HAL_ADC_Stop(&hadc1);
        return 0;
    }
    *value = HAL_ADC_GetValue(&hadc1);
    HAL_ADC_Stop(&hadc1);
    return 1;
}

static void ADC_Test_PrintChannel(const char *name, int ok, uint32_t raw, uint32_t mv)
{
    if (!ok) {
        printf("ADC_TEST: %s read FAIL\r\n", name);
        return;
    }
    uint32_t ratio_x100 = (mv * 100U) / ADC_TEST_INJECTED_MV;
    printf("ADC_TEST: %s raw=%lu (%lu mV, expected %lu mV, ratio %lu.%02lu)\r\n",
           name, (unsigned long)raw, (unsigned long)mv, (unsigned long)ADC_TEST_INJECTED_MV,
           (unsigned long)(ratio_x100 / 100U), (unsigned long)(ratio_x100 % 100U));
}

void ADC_Test_Run(void)
{
    printf("ADC_TEST: external inputs - PA6=ADC1_INP3(AIN1) PC3=ADC1_INP13(AIN2)\r\n");
    printf("ADC_TEST: inject a known DC voltage (e.g. one AA battery) across AIN1/AIN2 and GND,\r\n"
           "          set ADC_TEST_INJECTED_MV to what your multimeter reads across it\r\n");

    ADC_Test_GPIO_Init();
    if (!ADC_Test_Init()) {
        printf("ADC_TEST: FAIL reason=adc_init\r\n");
        return;
    }

    for (;;) {
        uint32_t raw_ain1 = 0, raw_ain2 = 0;
        int ok_ain1 = ADC_Test_ReadChannel(ADC_TEST_CH_AIN1, &raw_ain1);
        int ok_ain2 = ADC_Test_ReadChannel(ADC_TEST_CH_AIN2, &raw_ain2);

        uint32_t mv1 = (raw_ain1 * ADC_TEST_VREF_MV) / ADC_TEST_FULL_SCALE;
        uint32_t mv2 = (raw_ain2 * ADC_TEST_VREF_MV) / ADC_TEST_FULL_SCALE;

        ADC_Test_PrintChannel("AIN1", ok_ain1, raw_ain1, mv1);
        ADC_Test_PrintChannel("AIN2", ok_ain2, raw_ain2, mv2);

        HAL_Delay(ADC_TEST_PERIOD_MS);
    }
}

// dac_test.c
//
// Board bring-up case 4 - see dac_test.h.

// Enables the DAC HAL module for this translation unit only (see
// stm32h7xx_hal_dac.c in TestCase/common for why) - must come before main.h
// pulls in stm32h7xx_hal.h.
#include "testcase_hal_guard.h"   /* fires if this peripheral becomes real -- read it */
#define HAL_DAC_MODULE_ENABLED

#include "dac_test.h"
#include "ADC/adc_test.h"
#include "main.h"
#include <stdio.h>

/* The DAC's full scale is VREF+, which on this board is whatever VREFBUF is
 * driving - there is no external reference. The real value is taken from the
 * ADC module, which measures it against VREFINT; this constant is only the
 * fallback for when that measurement failed. Assuming 3300 here was wrong and
 * put every output about 24 percent high in code terms. */
#define DAC_TEST_VREF_FALLBACK_MV 2500U
#define DAC_TEST_FULL_SCALE    4095U   /* 12-bit, right aligned */
#define DAC_TEST_PERIOD_MS     3000U

#define DAC_TEST_AOUT1_MV      500U
#define DAC_TEST_AOUT2_MV      1500U

/* XTR111 with RSET = 1024R: Iout = Vin * 10 / 1024, i.e. uA = mV * 10000 / 1024 */
#define XTR111_RSET_OHM        1024U

static DAC_HandleTypeDef hdac1;
static uint32_t dac_vref_mv = DAC_TEST_VREF_FALLBACK_MV;
static uint32_t dac_due_ms;
static int      dac_ready;

static uint32_t DAC_Test_MvToCode(uint32_t mv)
{
    return (mv * DAC_TEST_FULL_SCALE) / dac_vref_mv;
}

static uint32_t DAC_Test_CodeToMv(uint32_t code)
{
    return (code * dac_vref_mv) / DAC_TEST_FULL_SCALE;
}

static uint32_t DAC_Test_ExpectedMicroamps(uint32_t mv)
{
    return (mv * 10000U) / XTR111_RSET_OHM;
}

static int DAC_Test_StartChannel(uint32_t channel, uint32_t mv)
{
    DAC_ChannelConfTypeDef cfg = {0};
    cfg.DAC_SampleAndHold            = DAC_SAMPLEANDHOLD_DISABLE;
    cfg.DAC_Trigger                  = DAC_TRIGGER_NONE;
    /* Buffer on: R17/R18 are 1k straight across the DAC output, which pulls
     * about 2 mA at full scale - far more than an unbuffered output can hold. */
    cfg.DAC_OutputBuffer             = DAC_OUTPUTBUFFER_ENABLE;
    cfg.DAC_ConnectOnChipPeripheral  = DAC_CHIPCONNECT_EXTERNAL;
    cfg.DAC_UserTrimming             = DAC_TRIMMING_FACTORY;

    if (HAL_DAC_ConfigChannel(&hdac1, &cfg, channel) != HAL_OK) {
        return 0;
    }
    if (HAL_DAC_SetValue(&hdac1, channel, DAC_ALIGN_12B_R,
                         DAC_Test_MvToCode(mv)) != HAL_OK) {
        return 0;
    }
    if (HAL_DAC_Start(&hdac1, channel) != HAL_OK) {
        return 0;
    }
    return 1;
}

int DAC_Test_Init(void)
{
    uint32_t measured = ADC_Test_GetVrefMv();
    if (measured != 0U) {
        dac_vref_mv = measured;
    }
    printf("[DAC] full scale = VREF+ = %lu mV (%s)\r\n",
           (unsigned long)dac_vref_mv,
           measured ? "measured" : "fallback, VREF+ was not measurable");

    __HAL_RCC_GPIOA_CLK_ENABLE();
    __HAL_RCC_DAC12_CLK_ENABLE();

    GPIO_InitTypeDef gpio = {0};
    gpio.Mode = GPIO_MODE_ANALOG;
    gpio.Pull = GPIO_NOPULL;
    gpio.Pin  = GPIO_PIN_4 | GPIO_PIN_5;
    HAL_GPIO_Init(GPIOA, &gpio);

    hdac1.Instance = DAC1;
    if (HAL_DAC_Init(&hdac1) != HAL_OK) {
        return 0;
    }

    if (!DAC_Test_StartChannel(DAC_CHANNEL_1, DAC_TEST_AOUT1_MV)) {
        return 0;
    }
    if (!DAC_Test_StartChannel(DAC_CHANNEL_2, DAC_TEST_AOUT2_MV)) {
        return 0;
    }

    dac_ready  = 1;
    dac_due_ms = HAL_GetTick();
    return 1;
}

void DAC_Test_Tick(uint32_t now_ms)
{
    if (!dac_ready || (int32_t)(now_ms - dac_due_ms) < 0) {
        return;
    }
    dac_due_ms = now_ms + DAC_TEST_PERIOD_MS;

    uint32_t code1 = DAC_Test_MvToCode(DAC_TEST_AOUT1_MV);
    uint32_t code2 = DAC_Test_MvToCode(DAC_TEST_AOUT2_MV);
    uint32_t act1  = DAC_Test_CodeToMv(code1);
    uint32_t act2  = DAC_Test_CodeToMv(code2);
    uint32_t ua1   = DAC_Test_ExpectedMicroamps(act1);
    uint32_t ua2   = DAC_Test_ExpectedMicroamps(act2);

    printf("[T4 ] Analog Out 1 is driving %lu mV - an ammeter in that loop should read %lu.%03lu mA\r\n",
           (unsigned long)act1,
           (unsigned long)(ua1 / 1000U), (unsigned long)(ua1 % 1000U));
    printf("[T4 ] Analog Out 2 is driving %lu mV - should read %lu.%03lu mA\r\n",
           (unsigned long)act2,
           (unsigned long)(ua2 / 1000U), (unsigned long)(ua2 % 1000U));
    printf("[T4 ] those two currents are only correct while jumpers JP3 and JP4 are open\r\n");
}

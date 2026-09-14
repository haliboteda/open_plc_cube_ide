// port_dac.c
//
// Shared DAC access - see port_dac.h.

// Enables the DAC HAL module for this translation unit only (see
// stm32h7xx_hal_dac.c in TestCase/common for why) - must come before main.h
// pulls in stm32h7xx_hal.h.
#include "testcase_hal_guard.h"   /* fires if this peripheral becomes real -- read it */
#define HAL_DAC_MODULE_ENABLED

#include "port_dac.h"
#include "port_adc.h"
#include "main.h"

/* Assuming 3300 here was wrong and put every output about 24 percent high in
 * code terms, so the fallback matches what VREFBUF actually drives. */
#define DAC_VREF_FALLBACK_MV 2500U
#define DAC_FULL_SCALE       4095U   /* 12-bit, right aligned */

static DAC_HandleTypeDef hdac1;
static uint32_t dac_vref_mv = DAC_VREF_FALLBACK_MV;
static int      dac_ready;

static uint32_t PortDac_MvToCode(uint32_t mv)
{
    return (mv * DAC_FULL_SCALE) / dac_vref_mv;
}

uint32_t PortDac_VrefMv(void)
{
    return dac_vref_mv;
}

uint32_t PortDac_QuantisedMv(uint32_t mv)
{
    return (PortDac_MvToCode(mv) * dac_vref_mv) / DAC_FULL_SCALE;
}

uint32_t PortDac_ExpectedMicroamps(uint32_t mv)
{
    return (mv * 10000U) / PORT_XTR111_RSET_OHM;
}

int PortDac_Init(void)
{
    if (dac_ready) {
        return 1;
    }

    uint32_t measured = PortAdc_VddaMeasured() ? PortAdc_VddaMv() : 0U;
    if (measured != 0U) {
        dac_vref_mv = measured;
    }

    __HAL_RCC_GPIOA_CLK_ENABLE();
    __HAL_RCC_DAC12_CLK_ENABLE();

    GPIO_InitTypeDef gpio = {0};
    gpio.Mode = GPIO_MODE_ANALOG;
    gpio.Pull = GPIO_NOPULL;
    gpio.Pin  = GPIO_PIN_4 | GPIO_PIN_5;
    HAL_GPIO_Init(GPIOA, &gpio);

    /* The two fault flags, as plain inputs. No pull: the flag's idle state is
     * the transceiver's business and a pull here would decide it instead, so a
     * floating pin has to read as floating rather than as whatever this file
     * preferred. */
    __HAL_RCC_GPIOI_CLK_ENABLE();
    __HAL_RCC_GPIOE_CLK_ENABLE();
    gpio.Mode = GPIO_MODE_INPUT;
    gpio.Pull = GPIO_NOPULL;
    gpio.Pin  = PORT_AOUT1_EF_PIN;
    HAL_GPIO_Init(PORT_AOUT1_EF_PORT, &gpio);
    gpio.Pin  = PORT_AOUT2_EF_PIN;
    HAL_GPIO_Init(PORT_AOUT2_EF_PORT, &gpio);

    hdac1.Instance = DAC1;
    if (HAL_DAC_Init(&hdac1) != HAL_OK) {
        return 0;
    }

    dac_ready = 1;
    return 1;
}

int PortDac_SetMv(int ch, uint32_t mv)
{
    uint32_t channel;

    if (!dac_ready) {
        return 0;
    }
    if (ch == 1) {
        channel = DAC_CHANNEL_1;
    } else if (ch == 2) {
        channel = DAC_CHANNEL_2;
    } else {
        return 0;
    }

    DAC_ChannelConfTypeDef cfg = {0};
    cfg.DAC_SampleAndHold           = DAC_SAMPLEANDHOLD_DISABLE;
    cfg.DAC_Trigger                 = DAC_TRIGGER_NONE;
    /* Buffer on: R17/R18 are 1k straight across the DAC output, which pulls
     * about 2 mA at full scale - far more than an unbuffered output can hold. */
    cfg.DAC_OutputBuffer            = DAC_OUTPUTBUFFER_ENABLE;
    cfg.DAC_ConnectOnChipPeripheral = DAC_CHIPCONNECT_EXTERNAL;
    cfg.DAC_UserTrimming            = DAC_TRIMMING_FACTORY;

    if (HAL_DAC_ConfigChannel(&hdac1, &cfg, channel) != HAL_OK) {
        return 0;
    }
    if (HAL_DAC_SetValue(&hdac1, channel, DAC_ALIGN_12B_R,
                         PortDac_MvToCode(mv)) != HAL_OK) {
        return 0;
    }
    if (HAL_DAC_Start(&hdac1, channel) != HAL_OK) {
        return 0;
    }
    return 1;
}

int PortDac_FaultLevel(int ch)
{
    GPIO_TypeDef *port;
    uint16_t pin;

    switch (ch) {
    case 1:  port = PORT_AOUT1_EF_PORT; pin = PORT_AOUT1_EF_PIN; break;
    case 2:  port = PORT_AOUT2_EF_PORT; pin = PORT_AOUT2_EF_PIN; break;
    default: return 0;
    }
    return (HAL_GPIO_ReadPin(port, pin) == GPIO_PIN_SET) ? 1 : 0;
}

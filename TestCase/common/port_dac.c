// port_dac.c
//
// Shared DAC access - see port_dac.h.

// Enables the local DAC HAL copy for this file only; must precede main.h
// (testcase_hal_guard.h).
#include "testcase_hal_guard.h"   /* fires if this peripheral becomes real -- read it */
#define HAL_DAC_MODULE_ENABLED

#include "port_dac.h"
#include "port_adc.h"
#include "main.h"

/* The fallback is what VREFBUF drives, not VDDA. */
#define DAC_VREF_FALLBACK_MV 2500U
#define DAC_FULL_SCALE       4095U   /* 12-bit, right aligned */

static DAC_HandleTypeDef hdac1;
static uint32_t dac_vref_mv = DAC_VREF_FALLBACK_MV;
static int      dac_ready;

static uint32_t PortDac_MvToCode(uint32_t mv)
{
    uint32_t code = (mv * DAC_FULL_SCALE) / dac_vref_mv;

    /* DHR12R holds 12 bits; anything above full scale would wrap to near 0. */
    return (code > DAC_FULL_SCALE) ? DAC_FULL_SCALE : code;
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

    /* The fault pins as plain inputs, no pull, so the board decides the idle. */
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

/* Buffer off at code 0, on otherwise: buffered, 0 leaves ~0.16 mA at the
 * terminal; unbuffered cannot drive R17/R18 above 0. See $PROD
 * docs/tables/DECISIONS.md 152.
 * MODEx is writable only while ENx = 0 (RM0433 DAC_MCR), so a buffer change
 * always goes stop -> configure -> value -> start. */
static int PortDac_Write(int ch, uint32_t channel, uint32_t code)
{
    static int configured[PORT_AOUT_COUNT];
    uint32_t shift = (ch == 1) ? DAC_MCR_MODE1_Pos : DAC_MCR_MODE2_Pos;
    int off = (code == 0U);
    int is_off = ((hdac1.Instance->MCR >> shift) & DAC_MCR_MODE1_1) != 0U;

    if (!configured[ch - 1] || off != is_off) {
        DAC_ChannelConfTypeDef cfg = {0};
        cfg.DAC_SampleAndHold           = DAC_SAMPLEANDHOLD_DISABLE;
        cfg.DAC_Trigger                 = DAC_TRIGGER_NONE;
        cfg.DAC_OutputBuffer            = off ? DAC_OUTPUTBUFFER_DISABLE
                                              : DAC_OUTPUTBUFFER_ENABLE;
        cfg.DAC_ConnectOnChipPeripheral = DAC_CHIPCONNECT_EXTERNAL;
        cfg.DAC_UserTrimming            = DAC_TRIMMING_FACTORY;

        (void)HAL_DAC_Stop(&hdac1, channel);
        if (HAL_DAC_ConfigChannel(&hdac1, &cfg, channel) != HAL_OK) {
            return 0;
        }
        configured[ch - 1] = 1;
    }
    if (HAL_DAC_SetValue(&hdac1, channel, DAC_ALIGN_12B_R, code) != HAL_OK) {
        return 0;
    }
    if (HAL_DAC_Start(&hdac1, channel) != HAL_OK) {
        return 0;
    }
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

    return PortDac_Write(ch, channel, PortDac_MvToCode(mv));
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

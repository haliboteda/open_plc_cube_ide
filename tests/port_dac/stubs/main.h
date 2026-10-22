/* Host stand-in for main.h: only what port_dac.c touches. The DAC behind it
 * is fake_dac.c, which keeps RM0433's rule that MODEx ignores writes while
 * the channel is enabled. */
#ifndef MAIN_H_STUB
#define MAIN_H_STUB

#include <stdint.h>

typedef enum { HAL_OK = 0, HAL_ERROR = 1 } HAL_StatusTypeDef;

/* ---- DAC ---------------------------------------------------------------- */
typedef struct {
    uint32_t CR;
    uint32_t MCR;
    uint32_t DHR12R[2];
} DAC_TypeDef;

extern DAC_TypeDef test_dac1;
#define DAC1 (&test_dac1)

typedef struct {
    DAC_TypeDef *Instance;
} DAC_HandleTypeDef;

typedef struct {
    uint32_t DAC_SampleAndHold;
    uint32_t DAC_Trigger;
    uint32_t DAC_OutputBuffer;
    uint32_t DAC_ConnectOnChipPeripheral;
    uint32_t DAC_UserTrimming;
} DAC_ChannelConfTypeDef;

#define DAC_CHANNEL_1               0x00U
#define DAC_CHANNEL_2               0x10U
#define DAC_CR_EN1                  (1U << 0)
#define DAC_MCR_MODE1_Pos           0U
#define DAC_MCR_MODE2_Pos           16U
#define DAC_MCR_MODE1               (7U << 0)
#define DAC_MCR_MODE1_1             (1U << 1)
#define DAC_OUTPUTBUFFER_ENABLE     0U
#define DAC_OUTPUTBUFFER_DISABLE    DAC_MCR_MODE1_1
#define DAC_SAMPLEANDHOLD_DISABLE   0U
#define DAC_TRIGGER_NONE            0U
#define DAC_CHIPCONNECT_EXTERNAL    0U
#define DAC_TRIMMING_FACTORY        0U
#define DAC_ALIGN_12B_R             0U

HAL_StatusTypeDef HAL_DAC_Init(DAC_HandleTypeDef *h);
HAL_StatusTypeDef HAL_DAC_ConfigChannel(DAC_HandleTypeDef *h,
                                        const DAC_ChannelConfTypeDef *c,
                                        uint32_t ch);
HAL_StatusTypeDef HAL_DAC_SetValue(DAC_HandleTypeDef *h, uint32_t ch,
                                   uint32_t align, uint32_t v);
HAL_StatusTypeDef HAL_DAC_Start(DAC_HandleTypeDef *h, uint32_t ch);
HAL_StatusTypeDef HAL_DAC_Stop(DAC_HandleTypeDef *h, uint32_t ch);

/* ---- GPIO / RCC: inert ---------------------------------------------------- */
typedef struct { uint32_t unused; } GPIO_TypeDef;
extern GPIO_TypeDef test_gpio;
#define GPIOA (&test_gpio)
#define GPIOE (&test_gpio)
#define GPIOI (&test_gpio)

typedef struct {
    uint32_t Pin;
    uint32_t Mode;
    uint32_t Pull;
} GPIO_InitTypeDef;

typedef enum { GPIO_PIN_RESET = 0, GPIO_PIN_SET = 1 } GPIO_PinState;

#define GPIO_PIN_3       (1U << 3)
#define GPIO_PIN_4       (1U << 4)
#define GPIO_PIN_5       (1U << 5)
#define GPIO_MODE_INPUT  0U
#define GPIO_MODE_ANALOG 3U
#define GPIO_NOPULL      0U

static inline void HAL_GPIO_Init(GPIO_TypeDef *p, GPIO_InitTypeDef *i) { (void)p; (void)i; }
static inline GPIO_PinState HAL_GPIO_ReadPin(GPIO_TypeDef *p, uint16_t pin)
{
    (void)p; (void)pin;
    return GPIO_PIN_RESET;
}

#define __HAL_RCC_GPIOA_CLK_ENABLE() ((void)0)
#define __HAL_RCC_GPIOE_CLK_ENABLE() ((void)0)
#define __HAL_RCC_GPIOI_CLK_ENABLE() ((void)0)
#define __HAL_RCC_DAC12_CLK_ENABLE() ((void)0)

#endif

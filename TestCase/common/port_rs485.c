// port_rs485.c
//
// Shared USART2 / SP3485EN access - see port_rs485.h.

#include "port_rs485.h"

static UART_HandleTypeDef huart_rs485;

/* Overruns since the last PortRs485_Init. Reported rather than hidden: a lost
 * byte is a real event on a half-duplex pair, and a counter that says how
 * often is the difference between "the wiring is dead" and "the reader was
 * late". */
static uint32_t s_rs485_overruns;

uint32_t PortRs485_Overruns(void)
{
    return s_rs485_overruns;
}

void PortRs485_ResetOverruns(void)
{
    s_rs485_overruns = 0u;
}


void PortRs485_DriveEnable(int on)
{
    HAL_GPIO_WritePin(PORT_RS485_DIR_PORT, PORT_RS485_DIR_PIN,
                      on ? GPIO_PIN_SET : GPIO_PIN_RESET);
}

int PortRs485_Init(uint32_t baud)
{
    __HAL_RCC_GPIOD_CLK_ENABLE();
    __HAL_RCC_USART2_CLK_ENABLE();

    GPIO_InitTypeDef gpio = {0};

    gpio.Mode  = GPIO_MODE_OUTPUT_PP;
    gpio.Pull  = GPIO_NOPULL;
    gpio.Speed = GPIO_SPEED_FREQ_LOW;
    gpio.Pin   = PORT_RS485_DIR_PIN;
    HAL_GPIO_Init(PORT_RS485_DIR_PORT, &gpio);
    PortRs485_DriveEnable(0);

    gpio.Mode      = GPIO_MODE_AF_PP;
    gpio.Pull      = GPIO_NOPULL;
    gpio.Speed     = GPIO_SPEED_FREQ_HIGH;
    gpio.Alternate = GPIO_AF7_USART2;
    gpio.Pin       = PORT_RS485_TX_PIN;
    HAL_GPIO_Init(PORT_RS485_DIR_PORT, &gpio);

    /* RO goes high-Z when /RE is driven high, so the pull-up is what makes an
     * off receiver read as an idle line instead of as noise. */
    gpio.Pull = GPIO_PULLUP;
    gpio.Pin  = PORT_RS485_RX_PIN;
    HAL_GPIO_Init(PORT_RS485_DIR_PORT, &gpio);

    huart_rs485.Instance                    = USART2;
    huart_rs485.Init.BaudRate               = baud;
    huart_rs485.Init.WordLength             = UART_WORDLENGTH_8B;
    huart_rs485.Init.StopBits               = UART_STOPBITS_1;
    huart_rs485.Init.Parity                 = UART_PARITY_NONE;
    huart_rs485.Init.Mode                   = UART_MODE_TX_RX;
    huart_rs485.Init.HwFlowCtl              = UART_HWCONTROL_NONE;
    huart_rs485.Init.OverSampling           = UART_OVERSAMPLING_16;
    huart_rs485.Init.OneBitSampling         = UART_ONE_BIT_SAMPLE_DISABLE;
    huart_rs485.Init.ClockPrescaler         = UART_PRESCALER_DIV1;
    huart_rs485.AdvancedInit.AdvFeatureInit = UART_ADVFEATURE_NO_INIT;

    s_rs485_overruns = 0u;
    return (HAL_UART_Init(&huart_rs485) == HAL_OK);
}

int PortRs485_SendRaw(const uint8_t *data, uint16_t len)
{
    return (int)HAL_UART_Transmit(&huart_rs485, (uint8_t *)data, len, 200);
}

int PortRs485_Send(const uint8_t *data, uint16_t len)
{
    PortRs485_DriveEnable(1);
    int st = PortRs485_SendRaw(data, len);
    PortRs485_DriveEnable(0);
    return st;
}

/* Raw registers, deliberately NOT HAL_UART_Receive.
 *
 * ⚠️ An overrun latches ORE, and HAL_UART_Receive returns an error with that
 * flag still set - it does not clear it, and its own RxState machine then
 * refuses later calls. One lost byte therefore made the receiver deaf for the
 * rest of the session, recoverable only by a hardware reset. Measured
 * 2026-09-09: after a single pt.stop/pt.start the pair reported rxbytes=0
 * forever while the peer was provably echoing every line.
 *
 * At 115200 a byte occupies 87 us and this USART has no FIFO enabled, so an
 * overrun is not exotic - it is what happens whenever a superloop pass takes
 * longer than that, which any pass that prints a frame does.
 *
 * This is the same shape UART4's own handler uses for the control channel
 * (TestCase/porttool/porttool.c) - clear the error flags, then take the byte
 * if one is there. That one has always worked; this one was the odd path out. */
int PortRs485_RecvByte(uint8_t *out)
{
    USART_TypeDef *u = huart_rs485.Instance;
    uint32_t isr;

    if (u == NULL) {
        return 0;
    }
    isr = u->ISR;

    if ((isr & (USART_ISR_ORE | USART_ISR_FE | USART_ISR_NE)) != 0u) {
        u->ICR = USART_ICR_ORECF | USART_ICR_FECF | USART_ICR_NECF;
        if ((isr & USART_ISR_ORE) != 0u) {
            s_rs485_overruns++;
        }
    }

    if ((isr & USART_ISR_RXNE_RXFNE) == 0u) {
        return 0;
    }
    if (out != NULL) {
        *out = (uint8_t)(u->RDR & 0xFFu);
    } else {
        (void)u->RDR;
    }
    return 1;
}

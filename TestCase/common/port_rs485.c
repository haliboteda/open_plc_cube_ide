// port_rs485.c
//
// Shared USART2 / SP3485EN access - see port_rs485.h.

#include "port_rs485.h"

static UART_HandleTypeDef huart_rs485;

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

int PortRs485_RecvByte(uint8_t *out)
{
    return (HAL_UART_Receive(&huart_rs485, out, 1, 0) == HAL_OK);
}

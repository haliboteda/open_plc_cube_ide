// rs232_test.c
//
// Standalone RS232 receive bring-up test - see rs232_test.h.

#include "rs232_test.h"
#include "main.h"
#include "usart.h"
#include <stdio.h>

#define RS232_TEST_POLL_TIMEOUT_MS 50U

void RS232_Test_Run(void)
{
    printf("RS232_TEST: echo running - type into the RS232 terminal, each byte should echo back\r\n");

    for (;;) {
        uint8_t byte;
        if (HAL_UART_Receive(&huart4, &byte, 1, RS232_TEST_POLL_TIMEOUT_MS) == HAL_OK) {
            HAL_UART_Transmit(&huart4, &byte, 1, HAL_MAX_DELAY);
            printf("RS232_TEST: rx 0x%02X ('%c')\r\n", (unsigned)byte,
                   (byte >= 32 && byte < 127) ? (char)byte : '.');
        }
    }
}

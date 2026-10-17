// port_rs485.h
//
// USART2 / SP3485EN access for the port tool: the pin map and the direction
// handling.
//
// Half duplex: PD4 drives /RE and DE as one net (Hardware/Production/
// UpperDeck/netlist.ipc:497-498), so the board cannot hear itself; a second
// device is required. PD4 is the only control.

#ifndef TESTCASE_COMMON_PORT_RS485_H_
#define TESTCASE_COMMON_PORT_RS485_H_

#include "main.h"
#include <stdint.h>

#define PORT_RS485_DEFAULT_BAUD 115200U

#define PORT_RS485_DIR_PORT     GPIOD
#define PORT_RS485_DIR_PIN      GPIO_PIN_4
#define PORT_RS485_TX_PIN       GPIO_PIN_5
#define PORT_RS485_RX_PIN       GPIO_PIN_6

/* Configures the direction pin, the AF pins and USART2 at `baud`, 8N1.
 * Returns 0 if HAL_UART_Init failed. Callers that want to exercise PD4/PD5 as
 * plain GPIO must do it before this, while they are still not muxed to AF. */
int PortRs485_Init(uint32_t baud);

/* 1 = transmit, 0 = receive. */
void PortRs485_DriveEnable(int on);

/* Transmits, leaving the direction pin wherever the caller put it.
 * Returns the HAL status. */
int PortRs485_SendRaw(const uint8_t *data, uint16_t len);

/* Turns the driver on, transmits, turns it off. Returns the HAL status.
 * HAL_UART_Transmit returns only after TC, so the last bit is on the wire
 * before the driver goes off. */
int PortRs485_Send(const uint8_t *data, uint16_t len);

/* Stores one byte and returns 1 if one was waiting, 0 otherwise. Never blocks. */
int PortRs485_RecvByte(uint8_t *out);

/* Overruns since PortRs485_Init: tells "the reader was late" from "the pair is
 * dead". */
uint32_t PortRs485_Overruns(void);

/* Zeroes that counter, at session start. */
void PortRs485_ResetOverruns(void);

#endif /* TESTCASE_COMMON_PORT_RS485_H_ */

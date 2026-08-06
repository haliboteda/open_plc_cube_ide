// rs232_test.h
//
// Standalone RS232 receive bring-up test for the STM32H743 OpenPLC board.
// Not part of the bootloader's core logic - safe to delete once RS232
// receive has been validated.
//
// Echoes back over UART4 (RS232, 115200) whatever byte it receives - type
// into a serial terminal connected to the Klemmblock C05(TxD)/C06(RxD)
// RS232 port and confirm each character you send comes back. TX direction
// is already proven by every printf in this codebase; this only exercises
// RX, which had no working implementation before (UART_StartReception()
// etc. in usart.c/.h are unfinished stubs, never called from anywhere).
//
// Runs forever (does not return).

#ifndef INC_RS232_TEST_H_
#define INC_RS232_TEST_H_

void RS232_Test_Run(void);

#endif /* INC_RS232_TEST_H_ */

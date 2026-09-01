// rs485_test.h
//
// RS485 bring-up: USART2 through the SP3485EN transceiver on the Upper Deck.
//
// External port: RS485 A = terminal A10 (UpperDeck J11-3)
//                RS485 B = terminal A11 (UpperDeck J11-2)
//
// MCU pins: PD5 = USART2_TX -> U6 DI
//           PD6 = USART2_RX <- U6 RO
//           PD4 = direction  -> U6 /RE and DE, which share one net
//
// ⚠ /RE and DE are the same net (netlist.ipc:497-498), so the receiver is off
// whenever the driver is on. The board cannot hear itself: proving RS485 works
// needs a second device on A/B. Host side: $TOOL/TestCase/tools/rs485_echo.py
//
// The transceiver has no enable pin - pin 8 is tied straight to +3V3 - so PD4
// is the only control there is.
//
// Three phases, then it stays in the last one:
//   R1  pin level, needs nothing attached: toggle PD4 and PD5 as GPIO, read back
//   R2  transmit a banner every 2 s so an adapter or a scope sees traffic
//   R4  echo whatever arrives back to the sender
//
// Runs forever (does not return).

#ifndef TESTCASE_RS485_TEST_H_
#define TESTCASE_RS485_TEST_H_

void RS485_Test_Run(void);

#endif /* TESTCASE_RS485_TEST_H_ */

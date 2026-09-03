// din_test.h
//
// *** The idle reading depends on whether 24 V is connected. With 24 V the
// *** comparator's + node is pulled to 0 V through 1k1 and an unconnected
// *** channel reads 0; on ST-Link's 3.3 V alone the LM339 has no supply, the
// *** 10k pull-up wins, and the same channel reads 1. Neither is a fault.
//
// Board bring-up case 1: read the eight Digital In pins and print what is on
// them.
//
//   PB5  PC6  PB6  PB7  PH10  PH11  PI5  PI6
//
// All eight are plain high-impedance inputs - nothing driven, and no internal
// pull-up or pull-down. Each of these lines already carries an external 10k
// pull-up to +3V3 through a 50R series resistor and shares its node with an
// LM339LV open-collector comparator output (see docs/design/HARDWARE-FACTS.md),
// so an internal pull would fight the board and distort the reading.
//
// One line per second with the level of every pin.
//
// Runs as one tick of the combined bring-up runner - see bringup_test.h.

#ifndef TESTCASE_DIN_TEST_H_
#define TESTCASE_DIN_TEST_H_

#include <stdint.h>

#define DIN_TEST_PINS 8

void DIN_Test_Init(void);
void DIN_Test_Tick(uint32_t now_ms);

#endif /* TESTCASE_DIN_TEST_H_ */

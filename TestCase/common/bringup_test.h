// bringup_test.h
//
// Runs board bring-up cases 1, 2, 3, 4 and 11 at the same time, from one
// flash. The five were checked against each other before being combined:
//
//   case 1  Digital In, read only       GPIO only
//   case 2  Relay                       GPIO only, reuses Core/Src/relay.c
//   case 3  Analog In                   ADC3 (PC3_C) + ADC1 (PA6)
//   case 4  Analog Out                  DAC1 both channels
//   case 11 Temperature                 ADC1 (PA0, PA3)
//
// No pin appears twice, and none of them collides with the 39 FMC pins the
// external SDRAM needs (PG3 and PD3 carry FMC alternate functions this project
// does not use - the SDRAM clock is PG8 and addressing stops at A12).
//
// The one shared resource is ADC1, driven by cases 3 and 11. Both go through
// TestCase/ADC/adc_test.c, which owns the single handle - a second
// HAL_ADC_Init() on the same instance would silently overwrite the first.
//
// Every case is a non-blocking tick. Case 2 puts a 2 s square wave on a scope,
// so nothing here may block long enough to show up as jitter on those edges.
//
// Keyboard control over the RS232 console (terminal C05/C06, 115200 8N1):
//   1 2 3 4 b   toggle that case on/off  (b = case 11, temperature)
//   a           all on
//   ?           help
//
// Case 1 drives nothing - the eight pins are high-impedance inputs and it
// only prints what the board puts on them. See din_test.h.

#ifndef TESTCASE_BRINGUP_TEST_H_
#define TESTCASE_BRINGUP_TEST_H_

void BringUp_Test_Run(void);   /* never returns */

#endif /* TESTCASE_BRINGUP_TEST_H_ */

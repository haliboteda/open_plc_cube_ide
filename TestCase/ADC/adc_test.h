// adc_test.h
//
// Board bring-up cases 3 (Analog In) and 11 (on-board temperature).
// Both live here because they share ADC1 - two HAL_ADC_Init() calls on one
// instance would overwrite each other.
//
// Case 3, Analog In - external wiring, injected by hand:
//   Analog In 1 (UpperDeck J3-4) -> PC3_C, ADC3_INP1
//   Analog In 2 (UpperDeck J4-1) -> PA6,   ADC12_INP3
//   Analog GND  (UpperDeck J4-4 / J4-5)
//   NOTE the port labels are swapped relative to the requirement text: on
//   this board Analog In 1 is the PC3_C pin, not PA6.
//
// Case 11, temperature - on board, no wiring:
//   PA0 = ADC1_INP16  (LM50BIM3 @ LowerDeck U1, reverse-polarity side, T-PS)
//   PA3 = ADC12_INP15 (LM50BIM3 @ LowerDeck U2, high-side switch, T-HS)
//   V = 10mV * T + 500mV, -25..+100 degC. Verified: no divider, buffer,
//   series resistor or filter cap between sensor VO and the MCU ball.
//
// Case 3 prints raw counts and both terminal-voltage interpretations, and
// deliberately does NOT declare PASS/FAIL: the input divider ratio depends on
// four solder jumpers (JP5/JP6/JP8/JP9) whose factory state is not documented
// anywhere - see the missing-information table in
// docs/test/BOARD-BRINGUP-CASES.md. Case 11 does declare PASS/FAIL, because
// the sensor and the formula are both confirmed.
//
// ADC is not a CubeMX peripheral in this project - the vendor HAL files live
// in TestCase/common/ and HAL_ADC_MODULE_ENABLED is defined per translation
// unit. See TestCase/common/testcase_hal_guard.h.
//
// Runs as one tick of the combined bring-up runner - see bringup_test.h.

#ifndef INC_ADC_TEST_H_
#define INC_ADC_TEST_H_

#include <stdint.h>

int      ADC_Test_Init(void);
/* Measured VREF+ in mV, or 0 if VREFINT could not be read. */
uint32_t ADC_Test_GetVrefMv(void);
void ADC_Test_TickAnalogIn(uint32_t now_ms);
void ADC_Test_TickTemperature(uint32_t now_ms);

#endif /* INC_ADC_TEST_H_ */

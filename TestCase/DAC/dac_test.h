// dac_test.h
//
// Board bring-up case 4: Analog Out.
//
// External ports the ammeter goes in series with:
//   Analog Out 1 (UpperDeck J4-2) -> ammeter -> Analog GND (UpperDeck J4-4 / J4-5)
//   Analog Out 2 (UpperDeck J4-3) -> same
//
// MCU pins: PA4 = DAC1_OUT1, PA5 = DAC1_OUT2.
//
// The DAC feeds an XTR111AIDGQT voltage-to-current converter with RSET =
// 1024R 1% (R9/R10, confirmed against the BOM), so Iout = Vin * 10 / 1024.
// 0.5 V -> 4.883 mA, 1.5 V -> 14.648 mA.
//
// No PASS/FAIL: those expected currents only hold while JP3/JP4 are open. If
// either is bridged, the XTR111 input becomes a summing node with /VREF and
// both numbers change - and neither the jumper state nor /VREF's voltage is
// documented. The firmware prints what it set and what to expect; the person
// reading the ammeter decides. See docs/test/BOARD-BRINGUP-CASES.md.
//
// There is no software enable to check: the XTR111 OD pin is tied to ground
// through 10k. The fault flags on PI4/PE3 are not readable either - their
// pull-up network looks drawn backwards, asserted-low only reaches ~3.0 V.
//
// DAC is not a CubeMX peripheral in this project - the vendor HAL files live
// in TestCase/common/. See TestCase/common/testcase_hal_guard.h.
//
// Runs as one tick of the combined bring-up runner - see bringup_test.h.

#ifndef TESTCASE_DAC_TEST_H_
#define TESTCASE_DAC_TEST_H_

#include <stdint.h>

int  DAC_Test_Init(void);
void DAC_Test_Tick(uint32_t now_ms);

#endif /* TESTCASE_DAC_TEST_H_ */

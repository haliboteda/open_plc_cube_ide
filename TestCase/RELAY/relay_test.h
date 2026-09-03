// relay_test.h
//
// *** 24 V must be connected: the 5 V that drives the coils comes down from
// *** it, and on ST-Link power alone no relay can pull in.
// *** The contacts are dry. A scope across them shows nothing whatever the
// *** relay is doing - put a source and a current-limiting resistor in series
// *** with one contact pair and probe across the resistor.
//
// Board bring-up case: the six on-board relays.
//
// MCU pins, driver and contact terminals:
//
//   Relay 1  PI8   T7 (SI2356DS)  LowerDeck X8-1 / X8-2   terminals B01 / B02
//   Relay 2  PI10  T6             LowerDeck X8-3 / X7-1   terminals B03 / B04
//   Relay 3  PI11  T5             LowerDeck X7-2 / X7-3   terminals B05 / B06
//   Relay 4  PG7   T4             LowerDeck X6-1 / X6-2   terminals B07 / B08
//   Relay 5  PG3   T3             LowerDeck X6-3 / X5-1   terminals B09 / B10
//   Relay 6  PD3   T2             LowerDeck X5-2 / X5-3   terminals B11 / B12
//
// The six pins are GPIO outputs in the .ioc and Locked=true there, so
// MX_GPIO_Init() has already configured them - this test only writes them,
// through Core/Src/relay.c, which is the same code the bootloader's power-on
// self-test uses.
//
// Hardware: relay HF41F/005-HST, 5 V coil fed from 5V0; flyback BAS516
// (D3-D8); 33k gate pull-down per channel (R3-R8).
//
// Measuring:
//
//   * The MCU side is the easy probe: the pin itself, or the gate of T2-T7 on
//     the Lower Deck. It proves the firmware is switching - it does NOT prove
//     the relay pulled in.
//   * Listen: six relays switching together are audible in a quiet room, which
//     is the cheapest confirmation that the coils are actually driven.
//
// The square wave is 2 s high / 2 s low on all six at once, so an
// oscilloscope has a slow, unambiguous edge to trigger on. Nothing here may
// block: the runner drives four other cases between ticks.
//
// Runs as one tick of the combined bring-up runner - see bringup_test.h.

#ifndef TESTCASE_RELAY_TEST_H_
#define TESTCASE_RELAY_TEST_H_

#include <stdint.h>

void Relay_Test_Init(void);
void Relay_Test_Tick(uint32_t now_ms);
/* Park every relay released, so the board is left in a known state. */
void Relay_Test_AllOff(void);

#endif /* TESTCASE_RELAY_TEST_H_ */

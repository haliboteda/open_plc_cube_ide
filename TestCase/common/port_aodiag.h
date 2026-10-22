// port_aodiag.h
//
// *** TEMPORARY diagnostic: remove once the AO noise source is found. ***
// Hardware side of pt.aodiag (porttool_aodiag.c). Documented in
// $PROD/maps/fixture-test-stages/issues/STG-41-where-does-the-ao-noise-come-from.md

#ifndef TESTCASE_COMMON_PORT_AODIAG_H_
#define TESTCASE_COMMON_PORT_AODIAG_H_

#include <stdint.h>

/* Quiet window: masks every NVIC interrupt, stops SysTick and the HAL time
 * base (TIM6), stops the ETH MAC/DMA if it was started, then sleeps in WFI
 * until a TIM2 one-shot expires. Restores all of it and advances the HAL tick
 * by the time slept. Returns the milliseconds slept. */
uint32_t PortAodiag_QuietSleep(uint32_t secs);

/* Busy-waits ms with interrupts live, calling step (if not NULL) every
 * step_ms. Returns how many times step ran. */
uint32_t PortAodiag_Spin(uint32_t ms, uint32_t step_ms, void (*step)(void));

/* DAC output buffer on/off for ch 1|2; the channel is re-enabled after.
 * PortDac_SetMv() sets the buffer for its value (off at 0). Returns 0 on a bad channel. */
int PortAodiag_DacBuffer(int ch, int on);

/* Disables DAC ch 1|2 (pin left to the board). PortDac_SetMv() re-enables. */
int PortAodiag_DacOff(int ch);

/* VREFBUF scale 0..3 (2.5 / 2.048 / 1.8 / 1.5 V); waits for VRR and settles
 * 20 ms like PortVref_Enable(). Returns 0 on a bad scale or timeout. */
int PortAodiag_VrefScale(uint32_t scale);

#endif /* TESTCASE_COMMON_PORT_AODIAG_H_ */

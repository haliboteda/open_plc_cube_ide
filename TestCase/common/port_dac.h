// port_dac.h
//
// Shared DAC1 access for the two Analog Out channels. The handle stays inside
// port_dac.c so that including this header does not drag in the HAL DAC module
// guard.
//
// Full scale is VREF+, which on this board is whatever VREFBUF drives - there
// is no external reference. PortVref_Enable() and PortAdc_Init() must both run
// first: the real reference value comes from the ADC measuring VREFINT.

#ifndef TESTCASE_COMMON_PORT_DAC_H_
#define TESTCASE_COMMON_PORT_DAC_H_

#include "main.h"
#include <stdint.h>

#define PORT_AOUT_COUNT 2

/* AOUT1_EF / AOUT2_EF, out of the pin assignment table
 * (Hardware/STM32H743IIK6_GPIO_ASSIGNMENT_*.xlsx rows 94 and 95). */
#define PORT_AOUT1_EF_PORT GPIOI
#define PORT_AOUT1_EF_PIN  GPIO_PIN_4
#define PORT_AOUT2_EF_PORT GPIOE
#define PORT_AOUT2_EF_PIN  GPIO_PIN_3

/* XTR111 with RSET = 1024R (R9 / R10): Iout = Vin * 10 / 1024 */
#define PORT_XTR111_RSET_OHM 1024U

/* The XTR111's fault flag for each channel, brought back to the MCU as
 * AOUT1_EF on PI4 and AOUT2_EF on PE3.
 *
 * *** The raw pin level, not a verdict. *** Which level means "fault" is not
 * stated anywhere in Hardware/ - the overview says only that the signal comes
 * back, and the pin assignment table has both pins as 预留/Reserved. So this
 * reports what the pin reads and the plan decides what that means. Reporting
 * it as "fault=0/1" would be inventing the half nobody has checked.
 *
 * ch is 1 or 2. Returns 0 or 1; anything else returns 0. */
int PortDac_FaultLevel(int ch);

/* Brings up DAC1 and latches the reference. Returns 0 on failure.
 * Safe to call more than once. */
int PortDac_Init(void);

/* The reference the codes below are computed against. */
uint32_t PortDac_VrefMv(void);

/* Drives Analog Out 1 (PA4, terminal D14) or 2 (PA5, D15). Returns 0 if the
 * channel is out of range or the HAL rejected it. */
int PortDac_SetMv(int ch, uint32_t mv);

/* What the pin actually carries after 12-bit quantisation of `mv`. */
uint32_t PortDac_QuantisedMv(uint32_t mv);

/* Loop current the XTR111 should produce for a given pin voltage, in uA.
 * *** Only correct while jumpers JP3 and JP4 are open. */
uint32_t PortDac_ExpectedMicroamps(uint32_t mv);

#endif /* TESTCASE_COMMON_PORT_DAC_H_ */

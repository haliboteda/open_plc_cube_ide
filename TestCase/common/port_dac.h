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

#include <stdint.h>

#define PORT_AOUT_COUNT 2

/* XTR111 with RSET = 1024R (R9 / R10): Iout = Vin * 10 / 1024 */
#define PORT_XTR111_RSET_OHM 1024U

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

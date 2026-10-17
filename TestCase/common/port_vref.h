// port_vref.h
//
// Turns on the MCU's internal VREFBUF at scale 0 (2.5 V), shared by the ADC and
// the DAC: the board has no external reference, and scale 0 matches the analog
// front ends' full scale (HARDWARE-FACTS.md 模拟量).

#ifndef TESTCASE_COMMON_PORT_VREF_H_
#define TESTCASE_COMMON_PORT_VREF_H_

/* Enables VREFBUF at scale 0 and waits for it to settle. Returns 0 on timeout.
 * Must run before any ADC or DAC init. Safe to call more than once. */
int PortVref_Enable(void);

#endif /* TESTCASE_COMMON_PORT_VREF_H_ */

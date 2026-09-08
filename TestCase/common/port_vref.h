// port_vref.h
//
// Turns on the MCU's internal VREFBUF. Shared because both the ADC and the DAC
// are useless without it and each would otherwise carry its own copy.
//
// This board has no external reference - the Bridge BOM contains no reference
// IC at all - so VREF+ carries nothing but its decoupling until VREFBUF drives
// it. Without this the ADC returns exact powers of two (0x8000, 0x4000: the SAR
// never converges) and the DAC outputs sit near zero.
//
// Scale 0 (about 2.5 V) is the pick: VDDA is 3.3 V so it is allowed, and the
// Analog In front end divides by 0.2494, putting a 0-10 V terminal swing at
// 0-2.494 V - full scale on a 2.5 V reference almost exactly.

#ifndef TESTCASE_COMMON_PORT_VREF_H_
#define TESTCASE_COMMON_PORT_VREF_H_

/* Enables VREFBUF at scale 0 and waits for it to settle. Returns 0 on timeout.
 * Must run before any ADC or DAC init. Safe to call more than once. */
int PortVref_Enable(void);

#endif /* TESTCASE_COMMON_PORT_VREF_H_ */

// port_adc.h
//
// Shared ADC1 / ADC3 access: the two Analog In channels and the two board
// temperature sensors. Handles stay inside port_adc.c so that including this
// header does not drag in the HAL ADC module guard.
//
// PortVref_Enable() must run before PortAdc_Init() (port_vref.h).

#ifndef TESTCASE_COMMON_PORT_ADC_H_
#define TESTCASE_COMMON_PORT_ADC_H_

#include <stdint.h>

#define PORT_AIN_COUNT   2
#define PORT_TEMP_COUNT  2

/* VDDA outside this band: every reading against it is meaningless. */
#define PORT_ADC_VDDA_MIN_MV 2000U
#define PORT_ADC_VDDA_MAX_MV 3600U

/* LM50: V = 10 mV/degC * T + 500 mV */
#define PORT_TEMP_OFFSET_MV  500
#define PORT_TEMP_MV_PER_DEGC 10

/* Brings up ADC1 and ADC3 and measures VDDA against VREFINT. Returns 0 on
 * failure. Safe to call more than once. */
int PortAdc_Init(void);

/* Measured VDDA in mV; the fallback constant if VREFINT could not be read. */
uint32_t PortAdc_VddaMv(void);

/* 1 when VDDA came from a VREFINT reading rather than the fallback constant.
 * Separate from PortAdc_VddaTrusted(): a measurement can succeed and still land
 * outside the plausible band. */
int PortAdc_VddaMeasured(void);

/* 1 only when VDDA was actually measured and lands inside the band above. */
int PortAdc_VddaTrusted(void);

/* Analog In 1 (PC3_C, terminal D12) or 2 (PA6, D13): raw and millivolts at the
 * MCU pin, no front-end scaling (the range is a jumper, HARDWARE-FACTS.md
 * 模拟量). Returns 0 if the conversion failed. */
int PortAdc_ReadAin(int ch, uint32_t *raw, uint32_t *mv);

/* Board temperature 1 (PA0, short-circuit protection) or 2 (PA3, high-side
 * FETs). Returns 0 if the conversion failed. */
int PortAdc_ReadTemp(int ch, uint32_t *mv, int32_t *decic);

#endif /* TESTCASE_COMMON_PORT_ADC_H_ */

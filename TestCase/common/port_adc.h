// port_adc.h
//
// Shared ADC1 / ADC3 access: the two Analog In channels and the two board
// temperature sensors. Handles stay inside port_adc.c so that including this
// header does not drag in the HAL ADC module guard.
//
// PortVref_Enable() must run before PortAdc_Init() - see port_vref.h for why.

#ifndef TESTCASE_COMMON_PORT_ADC_H_
#define TESTCASE_COMMON_PORT_ADC_H_

#include <stdint.h>

#define PORT_AIN_COUNT   2
#define PORT_TEMP_COUNT  2

/* VDDA outside this band means VREF+ is not being held up, and every reading
 * taken against it is meaningless - worth saying out loud, because the symptom
 * otherwise looks like a broken ADC rather than a missing supply. */
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

/* Averaged VREFINT count behind PortAdc_VddaMv(), 0 if it was never measured. */
uint32_t PortAdc_VrefintRaw(void);

/* 1 when VDDA came from a VREFINT reading rather than the fallback constant.
 * Separate from PortAdc_VddaTrusted(): a measurement can succeed and still land
 * outside the plausible band. */
int PortAdc_VddaMeasured(void);

/* 1 only when VDDA was actually measured and lands inside the band above. */
int PortAdc_VddaTrusted(void);

/* Analog In 1 (PC3_C, terminal D12) or 2 (PA6, D13). Returns 0 if the
 * conversion failed. *** No front-end scaling: raw and the voltage at the MCU
 * pin only. The terminal value depends on which range the board is bridged for,
 * which is a soldered choice - see $PROD/docs/hardware/HARDWARE-FACTS.md. */
int PortAdc_ReadAin(int ch, uint32_t *raw, uint32_t *mv);

/* Analog In 1 with the PC3 analog switch closed, for comparison against the
 * normal (open) reading. Closing it loads the node - about 16 percent. */
int PortAdc_ReadAin1SwitchClosed(uint32_t *raw);

/* Board temperature 1 (PA0, short-circuit protection) or 2 (PA3, high-side
 * FETs). Returns 0 if the conversion failed. */
int PortAdc_ReadTemp(int ch, uint32_t *mv, int32_t *decic);

#endif /* TESTCASE_COMMON_PORT_ADC_H_ */

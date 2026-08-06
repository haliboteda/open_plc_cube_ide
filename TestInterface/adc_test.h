// adc_test.h
//
// Standalone ADC bring-up test for the STM32H743 OpenPLC board.
// Not part of the bootloader's core logic - safe to delete once the
// ADC driver has been validated.
//
// Channels under test - see Hardware/Production/LowerDeck/{BOM,netlist.ipc}
// and the Bridge board schematic:
//   PA0 = ADC1_INP16 - short-circuit protection temp sensor (TEMP_SCPROT),
//         on-board, no external wiring needed (TI LM50BIM3/NOPB @ U1)
//   PA3 = ADC1_INP15 - HSFET/high-side-switch temp sensor (TEMP_HSSW),
//         on-board, no external wiring needed (TI LM50BIM3/NOPB @ U2)
//   PA6 = ADC1_INP3  - external Analog IN 1 (Klemmblock D), needs an
//         injected known voltage - divider ratio ahead of this pin (if
//         any) is not confirmed from the schematics on hand, calibrate
//         empirically against a multimeter
//   PC3 = ADC1_INP13 - external Analog IN 2 (Klemmblock D), same caveat
//
// Prints raw ADC counts + an approximate voltage (assumes Vref~=3.3V, not
// calibrated against VREFINT) for both channels once a second over UART4
// (RS232, 115200). This only proves the ADC peripheral itself converts and
// responds to a real, physically meaningful signal (warm up the HSFET
// heatsink and watch TEMP_HSSW's voltage move) - it does NOT validate the
// external Analog IN 1/2 (Klemmblock D12/D13, PC3/PA6) inputs, which need
// a known injected voltage and a multimeter to calibrate the divider ratio.
//
// This project never had the ADC HAL module (ADC was never enabled in the
// .ioc, so CubeMX never copied it in) - rather than touch the CubeMX-owned
// Core/ or Drivers/ folders, the missing vendor files (stm32h7xx_hal_adc.c,
// stm32h7xx_hal_adc_ex.c, stm32h7xx_hal_adc.h, stm32h7xx_hal_adc_ex.h,
// stm32h7xx_ll_adc.h - copied from a matching HAL v1.11.5 STM32Cube_FW_H7
// package) live right here alongside the test code instead. HAL_ADC_MODULE_ENABLED
// is #define'd locally at the top of adc_test.c and of the two copied-in
// stm32h7xx_hal_adc[_ex].c files (rather than in stm32h7xx_hal_conf.h or as
// a project-wide -D), so nothing outside this folder was touched at all -
// no .cproject change, no Core/ change. When the ADC driver is adopted for real
// (ported into the Arduino core / a proper MX_ADC1_Init()), regenerate
// ADC support via CubeMX instead of relying on these copied-in files.

#ifndef INC_ADC_TEST_H_
#define INC_ADC_TEST_H_

void ADC_Test_Run(void);

#endif /* INC_ADC_TEST_H_ */

/*
 * testcase_hal_guard.h
 *
 * TestCase keeps private copies of the ADC, DAC, SDMMC and FDCAN HAL drivers,
 * since those peripherals are not in the .ioc; each local .c defines its own
 * HAL_xxx_MODULE_ENABLED. Once CubeMX manages one of them, Drivers/ gains the
 * official driver and the copies collide. CubeMX then generates
 * Core/Inc/<peripheral>.h, so this header turns that into one actionable
 * #error. Include it FIRST in every TestCase file that fakes a HAL module.
 *
 * Skipped under PORTTOOL_HOST_TEST (T4-01): host toolchains ship headers with
 * these names (mingw's <adc.h>).
 */

#ifndef TESTCASE_HAL_GUARD_H_
#define TESTCASE_HAL_GUARD_H_

#if defined(__has_include) && !defined(PORTTOOL_HOST_TEST)

#if __has_include("adc.h")
#error "ADC is a real CubeMX peripheral now (Core/Inc/adc.h exists). Delete the local copies TestCase/common/stm32h7xx_hal_adc.c/.h, stm32h7xx_hal_adc_ex.c/.h and stm32h7xx_ll_adc.h; drop the '#define HAL_ADC_MODULE_ENABLED' from port_adc.c; then rework port_adc.c to only USE the peripheral (check hadc.State instead of initialising it). Same treatment sdram_test.c got when FMC was enabled -- see the notes at the top of sdram_test.h."
#endif

#if __has_include("dac.h")
#error "DAC is a real CubeMX peripheral now (Core/Inc/dac.h exists). Delete the local copies TestCase/common/stm32h7xx_hal_dac.c/.h and stm32h7xx_hal_dac_ex.c/.h; drop the '#define HAL_DAC_MODULE_ENABLED' from port_dac.c; then rework port_dac.c to only USE the peripheral (check hdac.State instead of initialising it). Same treatment sdram_test.c got when FMC was enabled -- see the notes at the top of sdram_test.h."
#endif

#if __has_include("sdmmc.h")
#error "SDMMC is a real CubeMX peripheral now (Core/Inc/sdmmc.h exists). Delete the local copies TestCase/common/stm32h7xx_hal_sd.c/.h, stm32h7xx_hal_sd_ex.c/.h, stm32h7xx_ll_sdmmc.c/.h and stm32h7xx_ll_delayblock.c/.h; drop the '#define HAL_SD_MODULE_ENABLED' from sd_test.c; then rework sd_test.c to only USE the peripheral. Same treatment sdram_test.c got when FMC was enabled -- see the notes at the top of sdram_test.h."
#endif

#if __has_include("fdcan.h")
#error "FDCAN is a real CubeMX peripheral now (Core/Inc/fdcan.h exists). Delete the local copies TestCase/common/stm32h7xx_hal_fdcan.c/.h; drop the '#define HAL_FDCAN_MODULE_ENABLED' from port_can.h; then rework port_can.c to only USE the peripheral (check hfdcan.State instead of initialising it, and drop its local RCC/GPIO/NVIC setup -- CubeMX's HAL_FDCAN_MspInit does that). Same treatment sdram_test.c got when FMC was enabled -- see the notes at the top of sdram_test.h."
#endif

#endif /* __has_include */

#endif /* TESTCASE_HAL_GUARD_H_ */

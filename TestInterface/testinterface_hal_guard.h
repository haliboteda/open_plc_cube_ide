/*
 * testinterface_hal_guard.h
 *
 * TestInterface keeps private copies of a few vendor HAL drivers, because the
 * peripherals they drive (ADC, SDMMC) are not in the .ioc -- so CubeMX never
 * copied the official ones into Drivers/, and there is nothing else to link
 * against. Each local driver .c therefore #defines its own
 * HAL_xxx_MODULE_ENABLED before including stm32h7xx_hal.h, which makes
 * stm32h7xx_hal_conf.h pull in the local header.
 *
 * That arrangement is fine right up to the moment one of those peripherals IS
 * enabled in the .ioc. Then Drivers/ gains the official driver, hal_conf.h
 * resolves the header to the official one, and the local .c files collapse into
 * a screenful of "conflicting types" plus duplicate symbols at link time -- with
 * nothing anywhere saying which files to delete. That is exactly what happened
 * when FMC was enabled and the local stm32h7xx_hal_sdram.c / stm32h7xx_ll_fmc.c
 * copies had to go.
 *
 * This header turns that into one actionable message instead. CubeMX generates a
 * Core/Inc/<peripheral>.h for every peripheral it manages -- crc.h, gpio.h,
 * rtc.h, usart.h and fmc.h are all there today -- so the appearance of adc.h or
 * sdmmc.h is a reliable "this is a real peripheral now" signal.
 *
 * Include this FIRST in every TestInterface file that fakes a HAL module.
 */

#ifndef TESTINTERFACE_HAL_GUARD_H_
#define TESTINTERFACE_HAL_GUARD_H_

#if defined(__has_include)

#if __has_include("adc.h")
#error "ADC is a real CubeMX peripheral now (Core/Inc/adc.h exists). Delete the local copies TestInterface/stm32h7xx_hal_adc.c/.h, stm32h7xx_hal_adc_ex.c/.h and stm32h7xx_ll_adc.h; drop the '#define HAL_ADC_MODULE_ENABLED' from adc_test.c; then rework adc_test.c to only USE the peripheral (check hadc.State instead of initialising it). Same treatment sdram_test.c got when FMC was enabled -- see the notes at the top of sdram_test.h."
#endif

#if __has_include("sdmmc.h")
#error "SDMMC is a real CubeMX peripheral now (Core/Inc/sdmmc.h exists). Delete the local copies TestInterface/stm32h7xx_hal_sd.c/.h, stm32h7xx_hal_sd_ex.c/.h, stm32h7xx_ll_sdmmc.c/.h and stm32h7xx_ll_delayblock.c/.h; drop the '#define HAL_SD_MODULE_ENABLED' from sd_test.c; then rework sd_test.c to only USE the peripheral. Same treatment sdram_test.c got when FMC was enabled -- see the notes at the top of sdram_test.h."
#endif

#endif /* __has_include */

#endif /* TESTINTERFACE_HAL_GUARD_H_ */

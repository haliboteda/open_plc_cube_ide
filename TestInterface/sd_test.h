// sd_test.h
//
// Standalone microSD card bring-up test for the STM32H743 OpenPLC board.
// Not part of the bootloader's core logic - safe to delete once the SD
// card reader has been validated and its init ported into a real
// MX_SDMMC1_SD_Init()/MX_FATFS_Init().
//
// Hardware under test (Bridge board, connector J6, Molex 104031-0811
// microSD socket - see Hardware/Bridge_overview.txt section 3):
//   CMD  -> PD2  (SDIO1_CMD)
//   CLK  -> PC12 (SDIO1_CLK)
//   DAT0 -> PC8  (SDIO1_D0)          <- only DAT0 wired, so 1-bit bus only
//   CD   -> PE6  (mechanical card-detect switch, not the SD DAT3/CD line)
//
// This project's .ioc never enabled SDMMC1 (no pins, no
// HAL_SD_MODULE_ENABLED, no MX_SDMMC1_SD_Init, no FatFs anywhere in
// Middlewares/ - same situation as ADC/SDRAM before this), so - same
// pattern as adc_test.c/sdram_test.c - the missing vendor files
// (stm32h7xx_hal_sd.c/.h, stm32h7xx_ll_sdmmc.c/.h) plus the FatFs core
// (ff.c/.h, diskio.c/.h, ff_gen_drv.c/.h, integer.h, ffconf.h) live right
// here alongside the test code, with HAL_SD_MODULE_ENABLED #define'd
// locally rather than touching Core/, the .ioc or .cproject.
//
// The GPIO pin map, HAL_SD_Init() parameters (1-bit bus, ClockDiv=0) and
// the SDMMC clock source (PLL2 -> ~50MHz kernel clock, M=2/N=12/P=2/Q=2/
// R=3) are copied from a known-working sibling firmware for the same
// Bridge board (E:\WorkSpace\Schaeffer-AG\ref\Hello_World_OpenPLC\Core\Src\sdmmc.c
// and its main.c's PeriphCommonClock_Config()), not reverse-engineered.
//
// Unlike the SDRAM test, this test does NOT use raw sector pokes for the
// integrity check - your card already has real data/filesystem on it, so
// clobbering arbitrary sectors would risk corrupting it. Instead it mounts
// FatFs (older ChaN R0.12c release, same as the reference project) and
// exercises the card through the normal file API, writing one small,
// distinctively-named test file rather than touching anything else on the
// card.
//
// Only polling-mode HAL_SD_ReadBlocks()/WriteBlocks() are used (no _IT/_DMA
// variants), so SDMMC1_IRQHandler is never needed - stm32h7xx_it.c (core-
// owned) is left untouched.
//
// Two independent entry points - uncomment exactly one in main.c at a
// time (see pwm_test.h/sdram_test.h for the same convention). Both run
// forever (do not return).
//
//   SD_Test_Info()           - answers "is a card detected, how big is
//       it?": brings up SDMMC1, reads the PE6 detect pin, calls
//       HAL_SD_Init() + HAL_SD_GetCardInfo(), and prints card type
//       (SDSC/SDHC/SDXC), capacity, block size and speed class. Re-checks
//       the detect pin and card state every 2s forever, so you can watch
//       it react live if you pull the card out and reinsert it.
//
//   SD_Test_FileIntegrity()  - answers "can I actually read/write it
//       correctly?": mounts FatFs, writes a 4KiB file ("0:/PLCTEST.BIN")
//       with a pseudo-random pattern, closes it, reopens and reads it
//       back, and compares both a full byte-for-byte match and a CRC32.
//       Repeats forever (regenerating the pattern each cycle) as a
//       continuous soak test - same idea as SDRAM_Test_Retention().

#ifndef INC_SD_TEST_H_
#define INC_SD_TEST_H_

void SD_Test_Info(void);
void SD_Test_FileIntegrity(void);

#endif /* INC_SD_TEST_H_ */

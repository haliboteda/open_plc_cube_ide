// sdram_test.h
//
// Standalone external SDRAM bring-up test for the STM32H743 OpenPLC board.
// Not part of the bootloader's core logic - safe to delete once the SDRAM
// has been validated and its FMC init ported into a real MX_FMC_Init().
//
// Hardware under test (Bridge board, fully internal - no external wiring):
//   U6 = AS4C32M16SB-7BIN, 32M x16bit x4 banks = 512Mbit = 64MiB SDRAM,
//   wired to the STM32H743's FMC controller, mapped at 0xC0000000 (Bank1)
//   once initialized. See Hardware/Bridge_overview.txt and the schematic
//   sheet "1436_01_SCHAE-BR_SHEET06-Memory.SchDoc" for the chip-side pinout.
//
// This project's .ioc never enabled the FMC/SDRAM peripheral (no pins, no
// HAL_SDRAM_MODULE_ENABLED, no MX_FMC_Init - see "To test.txt" -> External
// RAM -> "Needs to be activated"), so - same pattern as adc_test.c - the
// missing vendor files (stm32h7xx_hal_sdram.c/.h, stm32h7xx_ll_fmc.c/.h)
// live right here alongside the test code, with HAL_SDRAM_MODULE_ENABLED
// #define'd locally rather than touching Core/ or the .ioc.
//
// The GPIO pin map, HAL_SDRAM_Init() parameters, timing values and the
// SDRAM command bring-up sequence (clock-enable -> PALL -> auto-refresh x2
// -> load-mode-register -> refresh-rate) are NOT reverse-engineered here -
// they are copied from a known-working sibling firmware for the same
// Bridge board (E:\WorkSpace\Schaeffer-AG\ref\Hello_World_OpenPLC\Core\Src\fmc.c,
// written by the hardware engineer and confirmed against the schematic's
// FMC_* net names), so this is the same bring-up already proven on this
// exact chip/board combination.
//
// Note on caching: FMC Bank1 (0xC0000000-0xCFFFFFFF) falls in the Cortex-M7
// default memory map's "External device" region, which is Device-type and
// therefore never cached even with D-Cache globally enabled (see main.c's
// SCB_EnableDCache()) - unless an MPU region explicitly overrides it to
// Normal/cacheable, which nothing here does. Every load/store in this file
// really does go to the physical SDRAM controller, which matters a lot for
// SDRAM_Test_CubeProgrammerVerify() below (an external tool's writes must
// be visible to the next firmware read, and vice versa).
//
// Three independent entry points - uncomment exactly one in main.c at a
// time (see pwm_test.h/rs232_test.h for the same convention). All three
// run forever (do not return).
//
//   SDRAM_Test_Capacity()            - answers "how big is it really?":
//       data-bus walking-1/0 test (catches shorted/floating D0-D15),
//       address-bus walking test across all 26 byte-address bits including
//       the very top of the 64MiB window (catches shorted/open/aliased
//       address lines), optionally (SDRAM_TEST_RUN_FULL_SWEEP) a full
//       0x00/0xFF/0x55/0xAA sweep of the whole 64MiB. Prints a final
//       "CAPACITY CONFIRMED = 64 MiB" or a precise mismatch description.
//
//   SDRAM_Test_Retention()           - answers "write random addresses,
//       wait 5s, read back correctly?": writes SDRAM_TEST_NUM_RANDOM_ADDR
//       pseudo-random word-aligned addresses with an address-derived
//       signature, HAL_Delay(5000), reads them all back and compares.
//       A pass proves the auto-refresh configured in the bring-up sequence
//       is actually running (SDRAM cells lose their charge in tens of ms
//       without it) - repeats forever as a continuous soak test, reseeding
//       the address set each cycle.
//
//   SDRAM_Test_CubeProgrammerVerify() - answers "can I write via
//       STM32CubeProgrammer and read it back with an integrity check?":
//       runs the bring-up ONLY (does not touch memory contents), then
//       every second prints a zlib-compatible CRC32 + a 16-byte hex
//       preview of a configurable [offset, length) window. Connect
//       ST-Link, open STM32CubeProgrammer's "Read & Write Memory" panel
//       (no target reset - use its "no reset" connection mode), write your
//       file (image, .bin, anything - it's written as raw bytes regardless
//       of format) starting at 0xC0000000 + SDRAM_TEST_VERIFY_OFFSET, and
//       watch the printed CRC32 change on the RS232/UART4 terminal. Verify
//       independently on the PC with the same byte range of your original
//       file, e.g. in Python:
//         import zlib; print(hex(zlib.crc32(open("file","rb").read()
//                                            [:SDRAM_TEST_VERIFY_LENGTH])))
//       A matching CRC32 proves the written bytes survived the round trip
//       through the physical SDRAM byte-for-byte.

#ifndef INC_SDRAM_TEST_H_
#define INC_SDRAM_TEST_H_

void SDRAM_Test_Capacity(void);
void SDRAM_Test_Retention(void);
void SDRAM_Test_CubeProgrammerVerify(void);

#endif /* INC_SDRAM_TEST_H_ */

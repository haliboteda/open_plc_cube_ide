// sdram_test.h
//
// External SDRAM diagnostics for the STM32H743 OpenPLC board.
//
// Hardware under test (Bridge board, fully internal - no external wiring):
//   U6 = AS4C32M16SB-7BIN, 32M x16bit x4 banks = 512Mbit = 64MiB SDRAM,
//   wired to the STM32H743's FMC controller, mapped at 0xC0000000 (Bank1).
//   See Hardware/Bridge_overview.txt and the schematic sheet
//   "1436_01_SCHAE-BR_SHEET06-Memory.SchDoc" for the chip-side pinout.
//
// These started life as a standalone bring-up test, back when the FMC was not
// in the .ioc at all: this folder carried its own pin map, controller init,
// SDRAM command sequence, MPU patch and even its own copies of the vendor
// driver sources. The FMC is now a real CubeMX peripheral, so all of that has
// moved out and these are pure diagnostics that only *use* the peripheral:
//
//   pin map + clocks         -> Core/Src/fmc.c   HAL_FMC_MspInit()   (CubeMX)
//   controller init          -> Core/Src/fmc.c   MX_FMC_Init()       (CubeMX)
//   SDRAM power-up sequence  -> Core/Src/fmc.c   FMC_SDRAM_PowerUpSequence()
//   vendor driver sources    -> Drivers/STM32H7xx_HAL_Driver/        (CubeMX)
//   MPU access to 0xC0000000 -> MPU_Config() region 0, SubRegionDisable 0xC7
//
// Consequence: every entry point below must run AFTER MX_FMC_Init(), which in
// main.c means Phase 2. They check hsdram1.State and bail out with a clear
// message rather than poking a dead controller.
//
// The timing values and the command sequence were never reverse-engineered
// here - they came from a known-working sibling firmware for the same Bridge
// board: FMC_Init() in ref/Hello_World_OpenPLC/Core/Src/main.c, written by the
// hardware engineer and confirmed against the schematic's FMC_* net names. They
// are now what MX_FMC_Init() carries, value for value.
//
// Note on caching: FMC Bank1 (0xC0000000-0xCFFFFFFF) is in the Cortex-M7
// default memory map's "External device" region, which is Device-type and so
// never cached - and the bootloader runs with both caches off anyway. Every
// load/store here really does reach the physical SDRAM controller, which
// matters for SDRAM_Test_CubeProgrammerVerify() below: an external tool's
// writes must be visible to the next firmware read, and vice versa.
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
//       never touches memory contents, then
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

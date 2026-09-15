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
// Consequence: the controller has to be up before any of this touches
// 0xC0000000. Every entry point below therefore checks hsdram1.State and, if
// the controller is not ready, calls MX_FMC_Init() itself before going on.
//
// ⚠️ That on-demand call is not decoration. The port tool reaches these from
// main.c's Phase 1, where only clocks, GPIO and UART4 are up - MX_FMC_Init()
// does not run until Phase 2, which this image never reaches. Without it every
// SDRAM entry reported "not_initialised" (found 2026-09-07, before the first
// bench run).
//
// MPU region 0 is open for 0xC0000000 in both phases: main.c calls
// MPU_Config() before either.
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

// Three entry points below are the exception to "all three run forever": they
// perform one pass, fill in what they measured and return. They exist for
// pt.run, where the PC applies the limit - so they report numbers and no
// verdict.
//
//   SDRAM_Test_Probe()          - bring-up, data bus, address bus.
//   SDRAM_Test_SweepOnce()      - the whole 64MiB, four patterns, with the
//       mismatch count, where the first one was and how long each half took.
//       This is the "SDRAM stress test, zero errors" the production test
//       guide asks for at stations 6 and 10.
//   SDRAM_Test_RetentionOnce()  - one write/wait/read-back cycle, so the PC
//       decides how many cycles a station runs instead of the firmware
//       looping forever.

#ifndef INC_SDRAM_TEST_H_
#define INC_SDRAM_TEST_H_

#include <stdint.h>

typedef struct {
    uint32_t base;       /* where the window is mapped */
    uint32_t size_bytes; /* the size the driver was built for */
    uint8_t  ready;      /* the FMC controller was already brought up */
    uint8_t  databus_ok; /* all 16 data lines independent */
    uint8_t  addrbus_ok; /* no shorted, open or aliased address line */
} sdram_probe_t;

typedef struct {
    uint8_t  ready;            /* the FMC controller was already brought up */
    uint32_t patterns;         /* how many whole-array patterns were run */
    uint32_t words_each;       /* 32-bit words touched per pattern */
    uint32_t mismatches;       /* summed over every pattern */
    uint32_t first_bad_offset; /* byte offset of the first mismatch */
    uint32_t first_bad_pattern;/* the pattern that saw it */
    uint32_t write_ms;         /* summed over every pattern */
    uint32_t verify_ms;
} sdram_sweep_t;

typedef struct {
    uint8_t  ready;
    uint32_t checked;          /* random addresses written and read back */
    uint32_t failed;
    uint32_t wait_ms;          /* how long the data was left sitting */
    uint32_t first_bad_addr;
    uint32_t seed;             /* which address set this cycle used */
} sdram_retention_t;

typedef struct {
    uint8_t  ready;
    uint32_t offset;   /* byte offset into the array that was summed */
    uint32_t length;   /* how many bytes, after clamping to the array */
    uint32_t crc;      /* zlib-compatible CRC32 of that window */
} sdram_crc_t;

void SDRAM_Test_Probe(sdram_probe_t *out);
int  SDRAM_Test_SweepOnce(sdram_sweep_t *out);
int  SDRAM_Test_RetentionOnce(sdram_retention_t *out);

/* CRC32 of one window, once. Reads and never writes, so it is safe to run over
 * bytes somebody else put there - which is the whole point: an external tool
 * writes the array and this says what actually landed.
 *
 * `offset` and `length` come from the plan rather than being fixed here: which
 * window proves something is a production decision, and a firmware that fixed
 * it would have to be reflashed to check a different file (DECISIONS.md 22).
 * A length of 0 means "to the end of the array"; both are clamped so a plan
 * cannot ask it to read past the mapping. */
int  SDRAM_Test_Crc32Once(uint32_t offset, uint32_t length, sdram_crc_t *out);

/* The two halves of a retention cycle, so a caller that must not block can do
 * the waiting on its own clock. The wait is the test - the data has to sit
 * there long enough that auto-refresh is the only thing keeping it - and it is
 * also the only part that cannot happen inside a session tick.
 *
 * The address set lives between the calls, so Verify reads back exactly what
 * Write put down. Write again before verifying and the old set is gone.
 *
 * Both are silent: whoever drives them decides what to print. */
void SDRAM_Test_RetentionWrite(uint32_t *rng_state, sdram_retention_t *out);
void SDRAM_Test_RetentionVerify(sdram_retention_t *out);

void SDRAM_Test_Capacity(void);
void SDRAM_Test_Retention(void);
void SDRAM_Test_CubeProgrammerVerify(void);

#endif /* INC_SDRAM_TEST_H_ */

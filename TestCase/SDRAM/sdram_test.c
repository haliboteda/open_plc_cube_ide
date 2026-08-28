// sdram_test.c
//
// Standalone external SDRAM bring-up test - see sdram_test.h for the full
// picture (hardware, why the vendor files are copied in here, and what
// each of the three entry points proves).

#include "sdram_test.h"
#include "main.h"
#include "fmc.h"      /* hsdram1 -- the FMC/SDRAM is a real peripheral now */
#include <stdio.h>

#define SDRAM_BASE_ADDR   0xC0000000UL
#define SDRAM_SIZE_BYTES  0x04000000UL /* 64 MiB = 512Mbit AS4C32M16SB-7BIN */

#ifndef SDRAM_TEST_RUN_FULL_SWEEP
#define SDRAM_TEST_RUN_FULL_SWEEP 1 /* set to 0 to skip the slower whole-array pass */
#endif

#define SDRAM_TEST_NUM_RANDOM_ADDR 64U

#define SDRAM_TEST_VERIFY_OFFSET  0x00000000UL
#define SDRAM_TEST_VERIFY_LENGTH  285UL /* set this to your test file's exact byte size -
                                            the firmware always CRCs this many bytes
                                            regardless of how long the file you wrote
                                            actually was, so a mismatched length here
                                            is the most common false "integrity failure" */

/* ---- Bring-up is no longer this file's job --------------------------------
 *
 * When these tests were written the FMC was not in the .ioc at all, so this
 * file carried its own copies of the pin map, the controller init, the SDRAM
 * command sequence, the vendor driver sources, and an MPU patch. All of that
 * has since moved to where it belongs:
 *
 *   pin map + clocks        -> Core/Src/fmc.c   HAL_FMC_MspInit()   (CubeMX)
 *   controller init         -> Core/Src/fmc.c   MX_FMC_Init()       (CubeMX)
 *   SDRAM power-up sequence -> Core/Src/fmc.c   FMC_SDRAM_PowerUpSequence()
 *   vendor driver sources   -> Drivers/STM32H7xx_HAL_Driver/        (CubeMX)
 *   MPU access to 0xC0000000 -> MPU_Config() region 0, SubRegionDisable 0xC7
 *
 * So the tests below just use the peripheral, and all this function does is
 * confirm somebody already brought it up. It must therefore be called after
 * MX_FMC_Init(), which in main.c means Phase 2. ------------------------- */

static int SDRAM_Test_Bringup(void)
{
    printf("SDRAM_TEST: FMC Bank1 @ 0x%08lX, %lu MiB (AS4C32M16SB-7BIN)\r\n",
           (unsigned long)SDRAM_BASE_ADDR, (unsigned long)(SDRAM_SIZE_BYTES / (1024UL * 1024UL)));

    if (hsdram1.State != HAL_SDRAM_STATE_READY) {
        printf("SDRAM_TEST: FAIL reason=not_initialised (state=%u) - "
               "MX_FMC_Init() has not run, or it failed\r\n",
               (unsigned)hsdram1.State);
        return 0;
    }
    printf("SDRAM_TEST: controller ready, power-up sequence already done by MX_FMC_Init()\r\n");
    return 1;
}

/* ---- Data bus test: walking-1/walking-0 at one fixed cell ------------- */

#define SDRAM_TEST_SCRATCH_WORD_OFFSET 0x123UL /* arbitrary, not a power of two */

static int SDRAM_Test_DataBus(void)
{
    volatile uint16_t *cell = ((volatile uint16_t *)SDRAM_BASE_ADDR) + SDRAM_TEST_SCRATCH_WORD_OFFSET;
    int ok = 1;
    uint32_t bit;

    for (bit = 0; bit < 16U; bit++) {
        uint16_t pattern = (uint16_t)(1UL << bit);
        *cell = pattern;
        uint16_t actual = *cell;
        if (actual != pattern) {
            printf("SDRAM_TEST: DATA BUS FAIL - walking-1 D%lu: wrote 0x%04X got 0x%04X\r\n",
                   (unsigned long)bit, pattern, actual);
            ok = 0;
        }
    }
    for (bit = 0; bit < 16U; bit++) {
        uint16_t pattern = (uint16_t)~(1UL << bit);
        *cell = pattern;
        uint16_t actual = *cell;
        if (actual != pattern) {
            printf("SDRAM_TEST: DATA BUS FAIL - walking-0 D%lu: wrote 0x%04X got 0x%04X\r\n",
                   (unsigned long)bit, pattern, actual);
            ok = 0;
        }
    }

    if (ok) {
        printf("SDRAM_TEST: data bus OK - all 16 data lines (D0-D15) independent\r\n");
    }
    return ok;
}

/* ---- Address bus test: powers-of-two word offsets + top word --------- */

#define SDRAM_TEST_ADDR_BITS 25U /* 64MiB / 2 bytes-per-word = 2^25 words */

static int SDRAM_Test_AddressBus(void)
{
    volatile uint16_t *base = (volatile uint16_t *)SDRAM_BASE_ADDR;
    uint32_t top_word_offset = (SDRAM_SIZE_BYTES / 2U) - 1U;
    int ok = 1;
    uint32_t bit;

    base[0] = 0x1234U;
    for (bit = 0; bit < SDRAM_TEST_ADDR_BITS; bit++) {
        uint32_t word_offset = 1UL << bit;
        base[word_offset] = (uint16_t)(0xA500U | bit);
    }
    base[top_word_offset] = 0x5A5AU;

    if (base[0] != 0x1234U) {
        printf("SDRAM_TEST: ADDRESS BUS FAIL - reference cell (word 0) corrupted, read 0x%04X\r\n",
               (unsigned)base[0]);
        ok = 0;
    }
    for (bit = 0; bit < SDRAM_TEST_ADDR_BITS; bit++) {
        uint32_t word_offset = 1UL << bit;
        uint16_t expected = (uint16_t)(0xA500U | bit);
        uint16_t actual = base[word_offset];
        if (actual != expected) {
            printf("SDRAM_TEST: ADDRESS BUS FAIL - A%lu (word offset 0x%08lX / byte 0x%08lX): "
                   "expected 0x%04X got 0x%04X\r\n",
                   (unsigned long)bit, (unsigned long)word_offset, (unsigned long)(word_offset * 2U),
                   expected, actual);
            ok = 0;
        }
    }
    if (base[top_word_offset] != 0x5A5AU) {
        printf("SDRAM_TEST: ADDRESS BUS FAIL - top word (byte offset 0x%08lX) expected 0x5A5A got 0x%04X\r\n",
               (unsigned long)(top_word_offset * 2U), (unsigned)base[top_word_offset]);
        ok = 0;
    }

    if (ok) {
        printf("SDRAM_TEST: address bus OK - %lu address bits independent, top word (0x%08lX) reachable\r\n",
               (unsigned long)SDRAM_TEST_ADDR_BITS, (unsigned long)(top_word_offset * 2U));
    }
    return ok;
}

/* ---- Optional full-range sweep: 0x00/0xFF/0x55/0xAA over all 64MiB ---- */

#if SDRAM_TEST_RUN_FULL_SWEEP

static int SDRAM_Test_FullSweepPattern(uint32_t pattern32, const char *label)
{
    volatile uint32_t *mem = (volatile uint32_t *)SDRAM_BASE_ADDR;
    uint32_t words = SDRAM_SIZE_BYTES / 4U;
    uint32_t i;
    uint32_t start = HAL_GetTick();
    uint32_t write_ms;
    uint32_t read_ms;
    uint32_t mismatches = 0;
    uint32_t first_bad_offset = 0;

    for (i = 0; i < words; i++) {
        mem[i] = pattern32;
    }
    write_ms = HAL_GetTick() - start;

    start = HAL_GetTick();
    for (i = 0; i < words; i++) {
        if (mem[i] != pattern32) {
            if (mismatches == 0U) {
                first_bad_offset = i * 4U;
            }
            mismatches++;
        }
    }
    read_ms = HAL_GetTick() - start;

    if (mismatches == 0U) {
        printf("SDRAM_TEST: full sweep %s OK (write %lums, verify %lums)\r\n",
               label, (unsigned long)write_ms, (unsigned long)read_ms);
        return 1;
    }
    printf("SDRAM_TEST: full sweep %s FAIL - %lu mismatches, first at byte offset 0x%08lX\r\n",
           label, (unsigned long)mismatches, (unsigned long)first_bad_offset);
    return 0;
}

static int SDRAM_Test_FullSweep(void)
{
    int ok = 1;
    ok &= SDRAM_Test_FullSweepPattern(0x00000000UL, "0x00");
    ok &= SDRAM_Test_FullSweepPattern(0xFFFFFFFFUL, "0xFF");
    ok &= SDRAM_Test_FullSweepPattern(0x55555555UL, "0x55");
    ok &= SDRAM_Test_FullSweepPattern(0xAAAAAAAAUL, "0xAA");
    return ok;
}

#endif /* SDRAM_TEST_RUN_FULL_SWEEP */

/* ---- Public entry point 1/3: how big is it really? -------------------- */

void SDRAM_Test_Capacity(void)
{
    int ok;

    printf("SDRAM_TEST: capacity test - data bus, address bus"
#if SDRAM_TEST_RUN_FULL_SWEEP
           ", full 64MiB sweep"
#endif
           "\r\n");

    if (!SDRAM_Test_Bringup()) {
        printf("SDRAM_TEST: FAIL reason=bringup\r\n");
        for (;;) { HAL_Delay(1000); }
    }

    ok = SDRAM_Test_DataBus();
    ok &= SDRAM_Test_AddressBus();
#if SDRAM_TEST_RUN_FULL_SWEEP
    ok &= SDRAM_Test_FullSweep();
#endif

    if (ok) {
        printf("SDRAM_TEST: CAPACITY CONFIRMED = %lu MiB (0x%08lX bytes) @ 0x%08lX, all buses OK\r\n",
               (unsigned long)(SDRAM_SIZE_BYTES / (1024UL * 1024UL)),
               (unsigned long)SDRAM_SIZE_BYTES, (unsigned long)SDRAM_BASE_ADDR);
    } else {
        printf("SDRAM_TEST: CAPACITY TEST FAILED - see failures above for the affected line/pattern\r\n");
    }

    for (;;) {
        HAL_Delay(5000);
        printf("SDRAM_TEST: capacity test finished (%s) - see result above\r\n", ok ? "PASS" : "FAIL");
    }
}

/* ---- Public entry point 2/3: random addresses survive a 5s wait? ------ */

static uint32_t SDRAM_Test_Rand(uint32_t *state)
{
    uint32_t x = *state;
    x ^= x << 13;
    x ^= x >> 17;
    x ^= x << 5;
    *state = x;
    return x;
}

static uint32_t SDRAM_Test_Signature(uint32_t address)
{
    return address ^ 0xDEADBEEFUL;
}

void SDRAM_Test_Retention(void)
{
    static uint32_t addresses[SDRAM_TEST_NUM_RANDOM_ADDR];
    uint32_t rng_state = HAL_GetTick() | 1U; /* xorshift needs a non-zero seed */

    printf("SDRAM_TEST: retention test - write %u random addresses, wait 5s, read back\r\n",
           (unsigned)SDRAM_TEST_NUM_RANDOM_ADDR);

    if (!SDRAM_Test_Bringup()) {
        printf("SDRAM_TEST: FAIL reason=bringup\r\n");
        for (;;) { HAL_Delay(1000); }
    }

    for (;;) {
        uint32_t i;
        uint32_t fail_count = 0;

        printf("SDRAM_TEST: retention cycle - rng_seed=0x%08lX\r\n", (unsigned long)rng_state);

        for (i = 0; i < SDRAM_TEST_NUM_RANDOM_ADDR; i++) {
            uint32_t word_index = SDRAM_Test_Rand(&rng_state) % (SDRAM_SIZE_BYTES / 4U);
            uint32_t address = SDRAM_BASE_ADDR + (word_index * 4U);
            addresses[i] = address;
            *(volatile uint32_t *)address = SDRAM_Test_Signature(address);
        }

        printf("SDRAM_TEST: written, waiting 5s (proves auto-refresh keeps cells alive)...\r\n");
        HAL_Delay(5000);

        for (i = 0; i < SDRAM_TEST_NUM_RANDOM_ADDR; i++) {
            uint32_t address = addresses[i];
            uint32_t expected = SDRAM_Test_Signature(address);
            uint32_t actual = *(volatile uint32_t *)address;
            if (actual != expected) {
                printf("SDRAM_TEST: RETENTION FAIL @ 0x%08lX: expected 0x%08lX got 0x%08lX\r\n",
                       (unsigned long)address, (unsigned long)expected, (unsigned long)actual);
                fail_count++;
            }
        }

        if (fail_count == 0U) {
            printf("SDRAM_TEST: retention PASS - all %u addresses correct after 5s\r\n",
                   (unsigned)SDRAM_TEST_NUM_RANDOM_ADDR);
        } else {
            printf("SDRAM_TEST: retention FAIL - %lu/%u addresses lost data (refresh not working?)\r\n",
                   (unsigned long)fail_count, (unsigned)SDRAM_TEST_NUM_RANDOM_ADDR);
        }

        HAL_Delay(1000);
    }
}

/* ---- Public entry point 3/3: STM32CubeProgrammer write + firmware verify */

static uint32_t SDRAM_Test_Crc32(const volatile uint8_t *data, uint32_t len)
{
    uint32_t crc = 0xFFFFFFFFUL;
    uint32_t i;
    uint32_t b;

    for (i = 0; i < len; i++) {
        crc ^= data[i];
        for (b = 0; b < 8U; b++) {
            crc = (crc & 1U) ? (crc >> 1) ^ 0xEDB88320UL : (crc >> 1);
        }
    }
    return crc ^ 0xFFFFFFFFUL;
}

void SDRAM_Test_CubeProgrammerVerify(void)
{
    volatile uint8_t *region = (volatile uint8_t *)(SDRAM_BASE_ADDR + SDRAM_TEST_VERIFY_OFFSET);

    printf("SDRAM_TEST: CubeProgrammer verify - bring-up only, memory contents untouched\r\n");

    if (!SDRAM_Test_Bringup()) {
        printf("SDRAM_TEST: FAIL reason=bringup\r\n");
        for (;;) { HAL_Delay(1000); }
    }

    printf("SDRAM_TEST: watching 0x%08lX + 0x%lX bytes (%lu KiB) - write your file there via\r\n"
           "            STM32CubeProgrammer's Read & Write Memory panel (connect with \"no reset\"),\r\n"
           "            then compare the CRC32 below against your PC-side computation, e.g. Python:\r\n"
           "            zlib.crc32(open('file','rb').read()[:0x%lX])\r\n",
           (unsigned long)(SDRAM_BASE_ADDR + SDRAM_TEST_VERIFY_OFFSET),
           (unsigned long)SDRAM_TEST_VERIFY_LENGTH,
           (unsigned long)(SDRAM_TEST_VERIFY_LENGTH / 1024UL),
           (unsigned long)SDRAM_TEST_VERIFY_LENGTH);

    for (;;) {
        uint32_t crc = SDRAM_Test_Crc32(region, SDRAM_TEST_VERIFY_LENGTH);
        uint32_t i;

        printf("SDRAM_TEST: CRC32(0x%08lX, %lu bytes) = 0x%08lX | first16=",
               (unsigned long)(SDRAM_BASE_ADDR + SDRAM_TEST_VERIFY_OFFSET),
               (unsigned long)SDRAM_TEST_VERIFY_LENGTH, (unsigned long)crc);
        for (i = 0; i < 16U && i < SDRAM_TEST_VERIFY_LENGTH; i++) {
            printf("%02X ", (unsigned)region[i]);
        }
        printf("\r\n");
        HAL_Delay(1000);
    }
}

// sdram_test.c
//
// Standalone external SDRAM bring-up test - see sdram_test.h for the full
// picture (hardware, why the vendor files are copied in here, and what
// each of the three entry points proves).
//
// Measured on this board (2026-08-31): zeroing runs at 91 MB/s - 16 MiB in
// 175.1 ms, so a full 64 MiB takes about 700 ms. Bringing the controller up
// costs 1.4 ms. Use these to judge whether a run is healthy.

#include "sdram_test.h"
#include "main.h"
#include "fmc.h"      /* hsdram1 -- the FMC/SDRAM is a real peripheral now */
#include <stdio.h>
#include <string.h>

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

    /* The port tool image reaches these tests from main.c's Phase 1, where
     * MX_FMC_Init() has not run yet - only clocks, GPIO and UART4 are up. So
     * bring the controller up on demand rather than reporting a dead one.
     *
     * This asks CubeMX's own init to run; it does not reimplement it. The
     * timings, the chip power-up sequence and the pin map all stay in fmc.c,
     * which is still their single source. MPU region 0 is already open for
     * 0xC0000000 by this point (main.c calls MPU_Config() before either
     * phase), so the window is addressable as soon as the controller is up. */
    if (hsdram1.State != HAL_SDRAM_STATE_READY) {
        printf("SDRAM_TEST: controller not up yet, running MX_FMC_Init()\r\n");
        MX_FMC_Init();
    }

    if (hsdram1.State != HAL_SDRAM_STATE_READY) {
        printf("SDRAM_TEST: FAIL reason=not_initialised (state=%u) - "
               "MX_FMC_Init() ran and the controller is still not ready\r\n",
               (unsigned)hsdram1.State);
        return 0;
    }
    printf("SDRAM_TEST: controller ready, power-up sequence done by MX_FMC_Init()\r\n");
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

/* ---- Full-range sweep: 0x00/0xFF/0x55/0xAA over all 64MiB -------------
 *
 * `acc` is optional. With it the caller gets the numbers as well as the
 * verdict, which is what SDRAM_Test_SweepOnce() reports to the PC; without it
 * the prose on the log line is the whole result, which is what the handover
 * entry has always done. */

static int SDRAM_Test_FullSweepPattern(uint32_t pattern32, const char *label,
                                       sdram_sweep_t *acc)
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

    if (acc != NULL) {
        acc->patterns++;
        acc->words_each = words;
        acc->write_ms  += write_ms;
        acc->verify_ms += read_ms;
        if (mismatches > 0U && acc->mismatches == 0U) {
            acc->first_bad_offset  = first_bad_offset;
            acc->first_bad_pattern = pattern32;
        }
        acc->mismatches += mismatches;
    }

    if (mismatches == 0U) {
        printf("SDRAM_TEST: full sweep %s OK (write %lums, verify %lums)\r\n",
               label, (unsigned long)write_ms, (unsigned long)read_ms);
        return 1;
    }
    printf("SDRAM_TEST: full sweep %s FAIL - %lu mismatches, first at byte offset 0x%08lX\r\n",
           label, (unsigned long)mismatches, (unsigned long)first_bad_offset);
    return 0;
}

static int SDRAM_Test_FullSweep(sdram_sweep_t *acc)
{
    int ok = 1;
    ok &= SDRAM_Test_FullSweepPattern(0x00000000UL, "0x00", acc);
    ok &= SDRAM_Test_FullSweepPattern(0xFFFFFFFFUL, "0xFF", acc);
    ok &= SDRAM_Test_FullSweepPattern(0x55555555UL, "0x55", acc);
    ok &= SDRAM_Test_FullSweepPattern(0xAAAAAAAAUL, "0xAA", acc);
    return ok;
}

/* ---- Public entry point: the same checks, reported as numbers ---------
 *
 * Same three checks the capacity entry runs, but it returns, and it reports
 * what it measured instead of a verdict. The verdict belongs to the PC (see
 * ../../docs/design/DECISIONS.md 22), which is what lets a production limit
 * change without reflashing.
 *
 * The prose above still goes out on the log line: this is what a person reads
 * while watching, and dropping it would make the machine-readable path harder
 * to debug than the one it replaces. */

void SDRAM_Test_Probe(sdram_probe_t *out)
{
    if (out == NULL) {
        return;
    }

    out->base = SDRAM_BASE_ADDR;
    out->size_bytes = SDRAM_SIZE_BYTES;
    out->ready = SDRAM_Test_Bringup() ? 1 : 0;
    out->databus_ok = 0;
    out->addrbus_ok = 0;

    if (!out->ready) {
        return;
    }

    out->databus_ok = SDRAM_Test_DataBus() ? 1 : 0;
    out->addrbus_ok = SDRAM_Test_AddressBus() ? 1 : 0;
}

/* The whole array, four patterns, as numbers. This is the stress pass the
 * production test guide asks for at stations 6 and 10 ("recognise 64 MB,
 * stress test zero errors"), and it takes tens of seconds - the PC's timeout
 * for it has to allow for that. */

int SDRAM_Test_SweepOnce(sdram_sweep_t *out)
{
    sdram_sweep_t local;

    if (out == NULL) {
        out = &local;
    }
    memset(out, 0, sizeof(*out));

    out->ready = SDRAM_Test_Bringup() ? 1 : 0;
    if (!out->ready) {
        return 0;
    }

    return SDRAM_Test_FullSweep(out) && out->mismatches == 0U;
}

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
    ok &= SDRAM_Test_FullSweep(NULL);
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

#define SDRAM_TEST_RETENTION_WAIT_MS 5000U

/* The address set the last write pass chose, read back by the verify pass.
 * File scope rather than local, because the two halves are now separate calls:
 * a caller that must not block waits between them on its own clock. */
static uint32_t sdram_retention_addresses[SDRAM_TEST_NUM_RANDOM_ADDR];

/* The two halves, silent. Whoever drives them decides what to print - the
 * blocking entry below prints its prose around them, and a session cannot
 * print per cycle at all without burying the control port it shares with every
 * other port. */

void SDRAM_Test_RetentionWrite(uint32_t *rng_state, sdram_retention_t *out)
{
    uint32_t i;

    out->seed    = *rng_state;
    out->checked = SDRAM_TEST_NUM_RANDOM_ADDR;
    out->failed  = 0;
    out->wait_ms = SDRAM_TEST_RETENTION_WAIT_MS;
    out->first_bad_addr = 0;

    for (i = 0; i < SDRAM_TEST_NUM_RANDOM_ADDR; i++) {
        uint32_t word_index = SDRAM_Test_Rand(rng_state) % (SDRAM_SIZE_BYTES / 4U);
        uint32_t address = SDRAM_BASE_ADDR + (word_index * 4U);
        sdram_retention_addresses[i] = address;
        *(volatile uint32_t *)address = SDRAM_Test_Signature(address);
    }
}

void SDRAM_Test_RetentionVerify(sdram_retention_t *out)
{
    uint32_t i;

    out->failed = 0;
    out->first_bad_addr = 0;

    for (i = 0; i < SDRAM_TEST_NUM_RANDOM_ADDR; i++) {
        uint32_t address = sdram_retention_addresses[i];
        uint32_t expected = SDRAM_Test_Signature(address);
        uint32_t actual = *(volatile uint32_t *)address;
        if (actual != expected) {
            if (out->failed == 0U) {
                out->first_bad_addr = address;
            }
            out->failed++;
        }
    }
}

/* One cycle: write the signatures, let them sit, read them back. `rng_state`
 * carries across calls so each cycle picks a different address set. */

static void SDRAM_Test_RetentionCycle(uint32_t *rng_state, sdram_retention_t *out)
{
    SDRAM_Test_RetentionWrite(rng_state, out);
    printf("SDRAM_TEST: retention cycle - rng_seed=0x%08lX\r\n", (unsigned long)out->seed);

    printf("SDRAM_TEST: written, waiting 5s (proves auto-refresh keeps cells alive)...\r\n");
    HAL_Delay(SDRAM_TEST_RETENTION_WAIT_MS);

    SDRAM_Test_RetentionVerify(out);
    if (out->failed > 0U) {
        printf("SDRAM_TEST: RETENTION FAIL - first bad @ 0x%08lX\r\n",
               (unsigned long)out->first_bad_addr);
    }

    if (out->failed == 0U) {
        printf("SDRAM_TEST: retention PASS - all %u addresses correct after 5s\r\n",
               (unsigned)SDRAM_TEST_NUM_RANDOM_ADDR);
    } else {
        printf("SDRAM_TEST: retention FAIL - %lu/%u addresses lost data (refresh not working?)\r\n",
               (unsigned long)out->failed, (unsigned)SDRAM_TEST_NUM_RANDOM_ADDR);
    }
}

/* One cycle and back to the command loop, so a station decides how many
 * cycles to run instead of the firmware looping forever. */

int SDRAM_Test_RetentionOnce(sdram_retention_t *out)
{
    static uint32_t rng_state;
    sdram_retention_t local;

    if (out == NULL) {
        out = &local;
    }
    memset(out, 0, sizeof(*out));

    if (rng_state == 0U) {
        rng_state = HAL_GetTick() | 1U; /* xorshift needs a non-zero seed */
    }

    out->ready = SDRAM_Test_Bringup() ? 1 : 0;
    if (!out->ready) {
        return 0;
    }

    SDRAM_Test_RetentionCycle(&rng_state, out);
    return out->failed == 0U;
}

void SDRAM_Test_Retention(void)
{
    uint32_t rng_state = HAL_GetTick() | 1U; /* xorshift needs a non-zero seed */

    printf("SDRAM_TEST: retention test - write %u random addresses, wait 5s, read back\r\n",
           (unsigned)SDRAM_TEST_NUM_RANDOM_ADDR);

    if (!SDRAM_Test_Bringup()) {
        printf("SDRAM_TEST: FAIL reason=bringup\r\n");
        for (;;) { HAL_Delay(1000); }
    }

    for (;;) {
        sdram_retention_t r;

        SDRAM_Test_RetentionCycle(&rng_state, &r);
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

int SDRAM_Test_Crc32Once(uint32_t offset, uint32_t length, sdram_crc_t *out)
{
    sdram_crc_t local;

    if (out == NULL) {
        out = &local;
    }
    memset(out, 0, sizeof(*out));

    /* Clamped rather than refused. A window that runs off the end of the
     * mapping is a plan that needs fixing, but reading past it on this part
     * wraps silently and would produce a CRC that looks like an answer. */
    if (offset >= SDRAM_SIZE_BYTES) {
        offset = 0U;
    }
    if (length == 0U || length > (SDRAM_SIZE_BYTES - offset)) {
        length = SDRAM_SIZE_BYTES - offset;
    }

    out->offset = offset;
    out->length = length;

    out->ready = SDRAM_Test_Bringup() ? 1 : 0;
    if (!out->ready) {
        return 0;
    }

    out->crc = SDRAM_Test_Crc32(
        (const volatile uint8_t *)(SDRAM_BASE_ADDR + offset), length);
    return 1;
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

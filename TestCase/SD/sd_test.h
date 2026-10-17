// sd_test.h
//
// microSD card diagnostics for the STM32H743 OpenPLC board, called by the
// port tool's sd session and pt.run sd.*. Not part of the bootloader.
//
// Hardware under test (Bridge board, connector J6, Molex 104031-0811
// microSD socket - see Hardware/Bridge_overview.txt section 3):
//   CMD  -> PD2  (SDIO1_CMD)
//   CLK  -> PC12 (SDIO1_CLK)
//   DAT0 -> PC8  (SDIO1_D0)          <- *** only DAT0 wired: 1-bit bus only
//   CD   -> PE6  (mechanical card-detect switch, not the SD DAT3/CD line)
//
// This project's .ioc never enabled SDMMC1 (no pins, no
// HAL_SD_MODULE_ENABLED, no MX_SDMMC1_SD_Init, no FatFs anywhere in
// Middlewares/ - same situation as ADC/SDRAM before this), so - same
// pattern as port_adc.c - the missing vendor files
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

#ifndef INC_SD_TEST_H_
#define INC_SD_TEST_H_

// Every call below performs one pass, reports what it measured and returns.
// They exist for pt.run, where the PC applies the limit - so they report
// numbers and no verdict. SD_Test_IntegrityOnce() writes a 4KiB pseudo-random
// file ("0:/PLCTEST.BIN"), reads it back and compares byte for byte and by
// CRC32.
//
// SD_Test_StressOnce() is SD_Test_IntegrityOnce() repeated: the production
// test guide asks for an "SD read/write stress test" at stations 6 and 10, and
// one 4 KiB round is a functional check, not a stress one.

#include <stdint.h>

/* What is on the card, read from the boot sector rather than from FatFs.
 *
 * ⚠️ EXFAT is the one that matters. With _FS_EXFAT 0 an exFAT card fails
 * f_mount with FR_NO_FILESYSTEM, so a station that only looked at "mounted"
 * could not tell a good card that needs reformatting from a dead one - and
 * would have an operator throw the good card away. */
#define SD_FS_NONE     0u   /* no boot signature at all */
#define SD_FS_FAT12    1u
#define SD_FS_FAT16    2u
#define SD_FS_FAT32    3u
#define SD_FS_EXFAT    4u
#define SD_FS_UNKNOWN  5u   /* signature present, none of the above */

typedef struct {
    uint8_t  detected;     /* the PE6 detect pin, before any bus traffic */
    uint8_t  ready;        /* identification succeeded */
    uint32_t block_count;
    uint32_t block_size;
    uint32_t capacity_mib;
    uint32_t card_type;
    uint8_t  version_2x;
    uint32_t card_class;
    uint8_t  fs_type;      /* one of SD_FS_*; NONE when the card is unreadable */
    uint32_t hal_error;    /* hsd1.ErrorCode after a failed identification */
} sd_probe_t;

typedef struct {
    uint8_t  mounted;
    uint8_t  wrote;
    uint8_t  read_back;
    uint8_t  identical;    /* the bytes came back the same AND the CRCs match */
    uint32_t bytes;
    uint32_t write_crc;
    uint32_t read_crc;
    int      fresult;      /* the last FRESULT; 0 is FR_OK, -1 never mounted */
} sd_integrity_t;

typedef struct {
    uint8_t  mounted;
    uint32_t passes;        /* write/read/verify rounds attempted */
    uint32_t passed;        /* rounds that came back identical */
    uint32_t bytes_each;
    uint32_t bytes_total;   /* written, and read again, so traffic is 2x this */
    uint32_t elapsed_ms;
    uint32_t first_bad_pass;/* 0 when nothing failed; 1-based otherwise */
    int      fresult;       /* the last FRESULT seen; -1 never mounted */
} sd_stress_t;

/* Just the detect switch on PE6, with nothing else touched.
 *
 * *** Separate from SD_Test_Probe on purpose. A probe brings SDMMC1 up and
 * *** reads the card's registers, which takes long enough to be felt in a
 * *** superloop; this is one GPIO read, so a session can poll it every pass
 * *** and report an insertion the moment it happens. What KIND of card went
 * *** in is still the probe's job.
 *
 * Initialises the pin on the first call, so a caller that only wants this does
 * not have to know the pin exists. */
int  SD_Test_Detected(void);

void SD_Test_Probe(sd_probe_t *out);
/* One write/read/verify round of `bytes` bytes. 0 means the built-in default.
 *
 * The transfer is chunked internally, so `bytes` may be far larger than any
 * buffer - a plan can ask for a firmware image's worth. The data is
 * regenerated from its seed on the read pass rather than held in RAM. */
int  SD_Test_IntegrityOnce(uint32_t bytes, sd_integrity_t *out);
/* How fast the card moves data, in bytes per second each way.
 *
 * ⚠️ Deliberately does NOT verify the content - that is what sd.integrity is
 * for. Comparing here would put this loop's own compare cost into the rate and
 * make a fast card look slow.
 *
 * What it catches that integrity cannot: a card that works but is far slower
 * than its class claims. Such a card passes every correctness test and then
 * makes a firmware update time out in the field. */
typedef struct {
    uint8_t  mounted;
    uint32_t bytes;         /* moved each way */
    uint32_t write_ms;
    uint32_t read_ms;
    uint32_t write_bps;     /* bytes per second; 0 when the time was too short */
    uint32_t read_bps;
    int      fresult;       /* -1 never mounted */
} sd_speed_t;

/* `bytes` each way, 0 for the built-in default. */
int  SD_Test_Speed(uint32_t bytes, sd_speed_t *out);

/* `passes` rounds of `bytes` each. 0 for either means the built-in default.
 *
 * How much traffic counts as proof is a production decision, so a plan says
 * it - the firmware only measures (DECISIONS.md 22). */
int  SD_Test_StressOnce(uint32_t bytes, uint32_t passes, sd_stress_t *out);


/* ---- File access for the panel's SD file transfer ---------------------
 *
 * One file at a time, inside SD_FILE_DIR, used by porttool_sdfile.c
 * (PORTTOOL-FLOW.md B.11). Every call mounts on demand, so a card pulled
 * after SD_File_Eject and pushed back in works without a reset. Return
 * values are FRESULT (0 is FR_OK); -1 means the card did not come up. */
#define SD_FILE_DIR "0:/PORTTOOL"

typedef void (*sd_file_each_t)(const char *name, uint32_t size,
                               uint16_t fdate, uint16_t ftime, void *ctx);

int SD_File_OpenWrite(const char *name);
int SD_File_OpenRead(const char *name, uint32_t *size);
int SD_File_Write(const uint8_t *buf, uint32_t n);
int SD_File_Read(uint8_t *buf, uint32_t n, uint32_t *got);
int SD_File_Close(void);
int SD_File_List(sd_file_each_t each, void *ctx, uint32_t *count);
int SD_File_Remove(const char *name);
int SD_File_Eject(void);

#endif /* INC_SD_TEST_H_ */

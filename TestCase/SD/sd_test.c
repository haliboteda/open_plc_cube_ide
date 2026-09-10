// sd_test.c
//
// Standalone microSD card bring-up test - see sd_test.h for the full
// picture (hardware, why the vendor files are copied in here, and what
// each of the two entry points proves).

// See stm32h7xx_hal_sd.c/stm32h7xx_ll_sdmmc.c in this same folder for why
// this is defined locally instead of touching Core/Inc/stm32h7xx_hal_conf.h.
#include "testcase_hal_guard.h"   /* fires if this peripheral becomes real -- read it */
#define HAL_SD_MODULE_ENABLED

#include "sd_test.h"
#include "main.h"
#include "ff.h"
#include "ff_gen_drv.h"
#include <stdio.h>
#include <string.h>

#define SD_TEST_DETECT_PORT GPIOE
#define SD_TEST_DETECT_PIN  GPIO_PIN_6 /* active-low: reads LOW when a card is inserted */

#define SD_TEST_FILE_NAME  "0:/PLCTEST.BIN"
#define SD_TEST_FILE_SIZE  4096U

static SD_HandleTypeDef hsd1;

/* Long enough for a single-block read on a slow card; the bring-up path uses
 * the same order of magnitude for its own transfers. */
#define SD_TEST_IO_TIMEOUT_MS 1000U

static uint8_t sd_read_fs_type(void);      /* defined below SD_Test_Probe */
static int     SD_Test_WaitTransferReady(void);
static FATFS sd_test_fs;
static char sd_test_path[4];
static uint8_t sd_test_driver_linked = 0;

/* ---- SDMMC1 bring-up (clock, GPIO, controller init) -------------------
   Pin map, HAL_SD_Init() parameters and PLL2/SDMMC clock source copied
   from the sibling firmware's sdmmc.c + main.c (see sd_test.h header) -
   proven working on this exact board/chip, not reverse-engineered here. */

static void SD_Test_GPIO_ClockInit(void)
{
    RCC_PeriphCLKInitTypeDef periphClk = {0};
    periphClk.PeriphClockSelection = RCC_PERIPHCLK_SDMMC;
    periphClk.PLL2.PLL2M = 2;
    periphClk.PLL2.PLL2N = 12;
    periphClk.PLL2.PLL2P = 2;
    periphClk.PLL2.PLL2Q = 2;
    periphClk.PLL2.PLL2R = 3;
    periphClk.PLL2.PLL2RGE = RCC_PLL2VCIRANGE_3;
    periphClk.PLL2.PLL2VCOSEL = RCC_PLL2VCOMEDIUM;
    periphClk.PLL2.PLL2FRACN = 0;
    periphClk.SdmmcClockSelection = RCC_SDMMCCLKSOURCE_PLL2;
    HAL_RCCEx_PeriphCLKConfig(&periphClk);

    __HAL_RCC_SDMMC1_CLK_ENABLE();
    __HAL_RCC_GPIOC_CLK_ENABLE();
    __HAL_RCC_GPIOD_CLK_ENABLE();

    GPIO_InitTypeDef gpio = {0};
    gpio.Mode = GPIO_MODE_AF_PP;
    gpio.Pull = GPIO_NOPULL;
    gpio.Speed = GPIO_SPEED_FREQ_VERY_HIGH;
    gpio.Alternate = GPIO_AF12_SDIO1;

    gpio.Pin = GPIO_PIN_12 | GPIO_PIN_8; /* PC12=SDIO1_CLK, PC8=SDIO1_D0 */
    HAL_GPIO_Init(GPIOC, &gpio);

    gpio.Pin = GPIO_PIN_2; /* PD2=SDIO1_CMD */
    HAL_GPIO_Init(GPIOD, &gpio);
}

/* Overrides the weak default in stm32h7xx_hal_sd.c - called by
   HAL_SD_Init() below. */
void HAL_SD_MspInit(SD_HandleTypeDef *hsd)
{
    (void)hsd;
    SD_Test_GPIO_ClockInit();
}

static void SD_Test_DetectPinInit(void)
{
    __HAL_RCC_GPIOE_CLK_ENABLE();

    GPIO_InitTypeDef gpio = {0};
    gpio.Pin = SD_TEST_DETECT_PIN;
    gpio.Mode = GPIO_MODE_INPUT;
    gpio.Pull = GPIO_PULLUP; /* assumes a switch to GND on insertion - see sd_test.h */
    HAL_GPIO_Init(SD_TEST_DETECT_PORT, &gpio);
}

static int SD_Test_IsCardDetected(void)
{
    return HAL_GPIO_ReadPin(SD_TEST_DETECT_PORT, SD_TEST_DETECT_PIN) == GPIO_PIN_RESET;
}

static const char *SD_Test_CardTypeString(uint32_t card_type)
{
    switch (card_type) {
    case CARD_SDSC:      return "SDSC (<2GB)";
    case CARD_SDHC_SDXC: return "SDHC/SDXC";
    case CARD_SECURED:   return "SD Secured";
    default:             return "unknown";
    }
}

static int SD_Test_Bringup(void)
{
    printf("SDCARD_TEST: bring-up - SDMMC1 1-bit bus (PD2/PC12/PC8), detect=PE6\r\n");

    SD_Test_DetectPinInit();
    if (!SD_Test_IsCardDetected()) {
        printf("SDCARD_TEST: WARNING - detect pin (PE6) reads \"no card\" - continuing anyway "
               "in case the pin sense is inverted on this board\r\n");
    }

    hsd1.Instance = SDMMC1;
    hsd1.Init.ClockEdge           = SDMMC_CLOCK_EDGE_RISING;
    hsd1.Init.ClockPowerSave      = SDMMC_CLOCK_POWER_SAVE_DISABLE;
    hsd1.Init.BusWide             = SDMMC_BUS_WIDE_1B;
    hsd1.Init.HardwareFlowControl = SDMMC_HARDWARE_FLOW_CONTROL_DISABLE;
    hsd1.Init.ClockDiv            = 0;

    if (HAL_SD_Init(&hsd1) != HAL_OK) {
        printf("SDCARD_TEST: FAIL reason=hal_sd_init (no card, or bus wiring issue)\r\n");
        return 0;
    }

    /* HAL_SD_GetCardInfo() in this driver only fills CardType/CardVersion/
       Class/RelCardAdd/BlockNbr/BlockSize/LogBlockNbr/LogBlockSize - it
       never touches CardSpeed, so that field is left as whatever garbage
       was on the stack. Zero-init and don't print CardSpeed. */
    HAL_SD_CardInfoTypeDef info = {0};
    HAL_SD_GetCardInfo(&hsd1, &info);
    printf("SDCARD_TEST: card detected - type=%s version=%s class=%lu\r\n",
           SD_Test_CardTypeString(info.CardType),
           (info.CardVersion == CARD_V2_X) ? "2.x" : "1.x",
           (unsigned long)info.Class);
    printf("SDCARD_TEST: capacity = %lu blocks x %lu bytes = %lu MiB\r\n",
           (unsigned long)info.LogBlockNbr, (unsigned long)info.LogBlockSize,
           (unsigned long)(((uint64_t)info.LogBlockNbr * info.LogBlockSize) / (1024UL * 1024UL)));
    return 1;
}

/* ---- Public entry point 1/2: is a card detected, how big is it? ------- */

void SD_Test_Info(void)
{
    int ok;
    int was_present;
    int now_present;

    printf("SDCARD_TEST: card info test\r\n");
    ok = SD_Test_Bringup();
    was_present = SD_Test_IsCardDetected();

    for (;;) {
        HAL_Delay(2000);
        now_present = SD_Test_IsCardDetected();

        /* A removed card invalidates the RCA session HAL_SD_Init() assigned -
           HAL_SD_GetCardState() would just keep reporting a bogus state (0,
           not any real HAL_SD_CARD_* value) forever otherwise. Re-run the
           full bring-up (fresh CMD0/CMD8/ACMD41/CMD2/CMD3 identification) on
           every ABSENT->PRESENT transition to pick up a new session. */
        if (now_present && !was_present) {
            printf("SDCARD_TEST: card (re)inserted - re-running bring-up for a fresh session\r\n");
            ok = SD_Test_Bringup();
        } else if (!now_present && was_present) {
            printf("SDCARD_TEST: card removed\r\n");
        }
        was_present = now_present;

        if (!now_present) {
            printf("SDCARD_TEST: detect_pin=ABSENT\r\n");
            continue;
        }
        if (!ok) {
            printf("SDCARD_TEST: bring-up FAILED - see reason above\r\n");
            continue;
        }
        printf("SDCARD_TEST: detect_pin=PRESENT card_state=%d\r\n", (int)HAL_SD_GetCardState(&hsd1));
    }
}

/* ---- FatFs diskio glue (replaces the reference project's separate
   bsp_driver_sd.c/sd_diskio.c layers with one direct wrapper around
   HAL_SD_ReadBlocks/WriteBlocks) ------------------------------------ */

static int SD_Test_WaitTransferReady(void)
{
    uint32_t deadline = HAL_GetTick() + 500U;
    while (HAL_SD_GetCardState(&hsd1) != HAL_SD_CARD_TRANSFER) {
        if (HAL_GetTick() > deadline) {
            return 0;
        }
    }
    return 1;
}

static DSTATUS SD_Test_DiskInitialize(BYTE lun)
{
    (void)lun;
    return (HAL_SD_GetCardState(&hsd1) == HAL_SD_CARD_TRANSFER) ? 0 : STA_NOINIT;
}

static DSTATUS SD_Test_DiskStatus(BYTE lun)
{
    (void)lun;
    return (HAL_SD_GetCardState(&hsd1) == HAL_SD_CARD_TRANSFER) ? 0 : STA_NOINIT;
}

static DRESULT SD_Test_DiskRead(BYTE lun, BYTE *buff, DWORD sector, UINT count)
{
    (void)lun;
    if (HAL_SD_ReadBlocks(&hsd1, buff, sector, count, 30000U) != HAL_OK) {
        return RES_ERROR;
    }
    return SD_Test_WaitTransferReady() ? RES_OK : RES_ERROR;
}

static DRESULT SD_Test_DiskWrite(BYTE lun, const BYTE *buff, DWORD sector, UINT count)
{
    (void)lun;
    if (HAL_SD_WriteBlocks(&hsd1, (uint8_t *)buff, sector, count, 30000U) != HAL_OK) {
        return RES_ERROR;
    }
    return SD_Test_WaitTransferReady() ? RES_OK : RES_ERROR;
}

static DRESULT SD_Test_DiskIoctl(BYTE lun, BYTE cmd, void *buff)
{
    (void)lun;
    HAL_SD_CardInfoTypeDef info;

    switch (cmd) {
    case CTRL_SYNC:
        return RES_OK;
    case GET_SECTOR_COUNT:
        HAL_SD_GetCardInfo(&hsd1, &info);
        *(DWORD *)buff = info.LogBlockNbr;
        return RES_OK;
    case GET_SECTOR_SIZE:
        HAL_SD_GetCardInfo(&hsd1, &info);
        *(WORD *)buff = (WORD)info.LogBlockSize;
        return RES_OK;
    case GET_BLOCK_SIZE:
        *(DWORD *)buff = 1U; /* erase block size in sectors - not queried, report 1 */
        return RES_OK;
    default:
        return RES_PARERR;
    }
}

static const Diskio_drvTypeDef sd_test_driver = {
    SD_Test_DiskInitialize,
    SD_Test_DiskStatus,
    SD_Test_DiskRead,
    SD_Test_DiskWrite,
    SD_Test_DiskIoctl,
};

static uint32_t SD_Test_Rand(uint32_t *state)
{
    uint32_t x = *state;
    x ^= x << 13;
    x ^= x >> 17;
    x ^= x << 5;
    *state = x;
    return x;
}


/* ---- Public entry point 2/2: write + read back + integrity check ----- */

/* One write/read/verify round, filled in for whoever asked.
 *
 * This is the body the forever loop below used to hold inline. It was pulled
 * out so pt.run can perform exactly one round and come back with numbers - the
 * PC decides whether they are acceptable (../../docs/design/DECISIONS.md 22),
 * which a loop that prints PASS and sleeps five seconds cannot support. */
/* One chunk's worth of buffer, whatever the total is.
 *
 * ⚠️ bytes= has to be free to be a megabyte or more - a production plan may
 * well want to move as much as a firmware image does - and a buffer that grows
 * with it does not fit: the internal SRAM is a megabyte in total. So the
 * transfer is chunked, and the read pass REGENERATES the expected data from
 * the seed rather than keeping it. That also makes the test more like the real
 * thing: firmware writes an image block by block, and a single 4 KiB write may
 * never leave the card's own cache. */
#define SD_TEST_CHUNK 4096U

/* Accumulating form, so a CRC can span chunks. Same polynomial and the same
 * final inversion as SD_Test_Crc32 - that one is now this one, run once. */
static uint32_t SD_Test_Crc32Update(uint32_t crc, const uint8_t *data, uint32_t len)
{
    uint32_t i, b;

    for (i = 0; i < len; i++) {
        crc ^= data[i];
        for (b = 0; b < 8U; b++) {
            crc = (crc & 1U) ? (crc >> 1) ^ 0xEDB88320UL : (crc >> 1);
        }
    }
    return crc;
}

static int SD_Test_IntegrityRound(uint32_t bytes, sd_integrity_t *out)
{
    static uint8_t buf[SD_TEST_CHUNK];
    static uint8_t want[SD_TEST_CHUNK];
    static uint32_t rng_state;

    FRESULT  fr;
    FIL      fil;
    UINT     done = 0;
    uint32_t left, chunk, i;
    uint32_t seed;
    uint32_t crc_w = 0xFFFFFFFFUL;
    uint32_t crc_r = 0xFFFFFFFFUL;
    int      ok = 1;

    if (bytes == 0U) {
        bytes = SD_TEST_FILE_SIZE;
    }
    if (rng_state == 0U) {
        rng_state = HAL_GetTick() | 1U;
    }

    out->bytes     = bytes;
    out->mounted   = 0;
    out->wrote     = 0;
    out->read_back = 0;
    out->identical = 0;
    out->write_crc = 0;
    out->read_crc  = 0;
    out->fresult   = 0;

    fr = f_mount(&sd_test_fs, sd_test_path, 1);
    out->fresult = (int)fr;
    if (fr != FR_OK) {
        printf("SDCARD_TEST: f_mount FAILED (FRESULT=%d)\r\n", (int)fr);
        if (fr == FR_NO_FILESYSTEM) {
            /* The one failure here that is not the board's fault, and the one
             * a station will hit: this FatFs is built without exFAT
             * (_FS_EXFAT 0 in ffconf.h), and any card over 32 GB ships
             * exFAT-formatted. sd.probe's fs= field names it outright. */
            printf("SDCARD_TEST: no FAT filesystem - this build has no exFAT "
                   "support, so the card must be FAT16/FAT32\r\n");
        }
        return 0;
    }
    out->mounted = 1;

    /* --- write pass ---------------------------------------------------- */
    seed = rng_state;
    fr = f_open(&fil, SD_TEST_FILE_NAME, FA_CREATE_ALWAYS | FA_WRITE);
    if (fr != FR_OK) {
        printf("SDCARD_TEST: FAIL reason=f_open_write (FRESULT=%d)\r\n", (int)fr);
        out->fresult = (int)fr;
        ok = 0;
    } else {
        left = bytes;
        while ((left > 0U) && ok) {
            chunk = (left > SD_TEST_CHUNK) ? SD_TEST_CHUNK : left;
            for (i = 0; i < chunk; i++) {
                buf[i] = (uint8_t)SD_Test_Rand(&rng_state);
            }
            crc_w = SD_Test_Crc32Update(crc_w, buf, chunk);

            fr = f_write(&fil, buf, chunk, &done);
            if ((fr != FR_OK) || (done != chunk)) {
                printf("SDCARD_TEST: FAIL reason=f_write (FRESULT=%d, wrote %u/%u,"
                       " %lu bytes in)\r\n", (int)fr, (unsigned)done,
                       (unsigned)chunk, (unsigned long)(bytes - left));
                out->fresult = (int)fr;
                ok = 0;
                break;
            }
            left -= chunk;
        }
        f_close(&fil);
        if (ok) {
            out->wrote = 1;
            out->write_crc = crc_w ^ 0xFFFFFFFFUL;
        }
    }

    /* --- read pass, comparing against the regenerated data -------------- */
    if (ok) {
        rng_state = seed;
        fr = f_open(&fil, SD_TEST_FILE_NAME, FA_READ);
        if (fr != FR_OK) {
            printf("SDCARD_TEST: FAIL reason=f_open_read (FRESULT=%d)\r\n", (int)fr);
            out->fresult = (int)fr;
            ok = 0;
        } else {
            left = bytes;
            while ((left > 0U) && ok) {
                chunk = (left > SD_TEST_CHUNK) ? SD_TEST_CHUNK : left;
                memset(buf, 0, chunk);
                fr = f_read(&fil, buf, chunk, &done);
                if ((fr != FR_OK) || (done != chunk)) {
                    printf("SDCARD_TEST: FAIL reason=f_read (FRESULT=%d, read %u/%u,"
                           " %lu bytes in)\r\n", (int)fr, (unsigned)done,
                           (unsigned)chunk, (unsigned long)(bytes - left));
                    out->fresult = (int)fr;
                    ok = 0;
                    break;
                }
                crc_r = SD_Test_Crc32Update(crc_r, buf, chunk);

                for (i = 0; i < chunk; i++) {
                    want[i] = (uint8_t)SD_Test_Rand(&rng_state);
                }
                if (memcmp(buf, want, chunk) != 0) {
                    printf("SDCARD_TEST: INTEGRITY FAIL - data differs %lu bytes"
                           " in\r\n", (unsigned long)(bytes - left));
                    ok = 0;
                    break;
                }
                left -= chunk;
            }
            f_close(&fil);
            if (ok) {
                out->read_back = 1;
                out->read_crc  = crc_r ^ 0xFFFFFFFFUL;
            }
        }
    }

    if (ok) {
        if (out->read_crc != out->write_crc) {
            printf("SDCARD_TEST: INTEGRITY FAIL - write_crc=0x%08lX read_crc=0x%08lX\r\n",
                   (unsigned long)out->write_crc, (unsigned long)out->read_crc);
            ok = 0;
        } else {
            out->identical = 1;
        }
    }
    return ok;
}

/* Shared by both entry points: the card and the FatFs driver have to be up
 * before a round can run. */
static int SD_Test_MountReady(void)
{
    if (!SD_Test_Bringup()) {
        printf("SDCARD_TEST: FAIL reason=bringup\r\n");
        return 0;
    }
    if (!sd_test_driver_linked) {
        if (FATFS_LinkDriver(&sd_test_driver, sd_test_path) != 0) {
            printf("SDCARD_TEST: FAIL reason=fatfs_link_driver\r\n");
            return 0;
        }
        sd_test_driver_linked = 1;
    }
    return 1;
}

void SD_Test_Probe(sd_probe_t *out)
{
    HAL_SD_CardInfoTypeDef info = {0};

    if (out == NULL) {
        return;
    }
    SD_Test_DetectPinInit();
    out->detected = (uint8_t)(SD_Test_IsCardDetected() ? 1 : 0);
    out->ready = (uint8_t)(SD_Test_Bringup() ? 1 : 0);
    out->block_count = 0;
    out->block_size = 0;
    out->capacity_mib = 0;
    out->card_type = 0;
    out->version_2x = 0;
    out->card_class = 0;
    out->hal_error  = 0;

    if (!out->ready) {
        /* ⚠️ Why identification failed, not just that it did. "detect pin says
         * a card is in, but the bus could not identify it" has several very
         * different causes - a card that never answers CMD0, one that answers
         * but rejects the voltage, one whose CSD does not parse - and the
         * error code is the only thing that tells them apart. Without it the
         * next person has to guess between a bad card, a bad socket and a
         * clock that is too fast. */
        out->hal_error = hsd1.ErrorCode;
        return;
    }
    HAL_SD_GetCardInfo(&hsd1, &info);
    out->block_count = info.LogBlockNbr;
    out->block_size  = info.LogBlockSize;
    out->capacity_mib = (uint32_t)(((uint64_t)info.LogBlockNbr * info.LogBlockSize) /
                                   (1024UL * 1024UL));
    out->card_type   = info.CardType;
    out->version_2x  = (uint8_t)((info.CardVersion == CARD_V2_X) ? 1 : 0);
    out->card_class  = info.Class;
    out->fs_type     = sd_read_fs_type();
}

/* --- Which filesystem is on the card ------------------------------------ */

/* Read from the boot sector, NOT from FatFs.
 *
 * ⚠️ This is the whole point of the field: with _FS_EXFAT 0, an exFAT card
 * fails f_mount with FR_NO_FILESYSTEM, so FATFS.fs_type is never filled and
 * the card is indistinguishable from a broken one. An operator told "the card
 * failed" replaces a perfectly good card instead of reformatting it. Reading
 * the signature says which of the two it is without mounting anything.
 *
 * Signature offsets are from the FAT and exFAT on-disk specifications: exFAT
 * puts "EXFAT   " at byte 3 of its boot sector, FAT32 puts "FAT32   " at 82,
 * and FAT12/16 put theirs at 54. An MBR is followed to its first partition,
 * because a card formatted by Windows normally has one. */
static uint8_t sd_fs_from_sector(const uint8_t *s)
{
    if ((s[510] != 0x55u) || (s[511] != 0xAAu)) {
        return SD_FS_NONE;
    }
    if (memcmp(&s[3], "EXFAT   ", 8) == 0) {
        return SD_FS_EXFAT;
    }
    if (memcmp(&s[82], "FAT32   ", 8) == 0) {
        return SD_FS_FAT32;
    }
    if (memcmp(&s[54], "FAT16   ", 8) == 0) {
        return SD_FS_FAT16;
    }
    if (memcmp(&s[54], "FAT12   ", 8) == 0) {
        return SD_FS_FAT12;
    }
    if (memcmp(&s[54], "FAT     ", 8) == 0) {
        return SD_FS_FAT16;   /* older formatters wrote the generic string */
    }
    return SD_FS_UNKNOWN;
}

static uint8_t sd_read_fs_type(void)
{
    static uint8_t sec[512] __attribute__((aligned(4)));
    uint8_t  kind;
    uint32_t part_lba;

    if (HAL_SD_ReadBlocks(&hsd1, sec, 0U, 1U, SD_TEST_IO_TIMEOUT_MS) != HAL_OK) {
        return SD_FS_NONE;
    }
    /* The card needs a moment before the next command; the diskio glue's own
     * wait is what this reuses rather than spinning a second way. */
    (void) SD_Test_WaitTransferReady();

    kind = sd_fs_from_sector(sec);
    if (kind != SD_FS_UNKNOWN) {
        return kind;
    }

    /* Not a boot sector itself - follow the MBR's first partition entry. The
     * type byte at 0x1C2 being non-zero is what says there is one. */
    if (sec[0x1C2] == 0x00u) {
        return SD_FS_UNKNOWN;
    }
    part_lba = (uint32_t)sec[0x1C6] | ((uint32_t)sec[0x1C7] << 8) |
               ((uint32_t)sec[0x1C8] << 16) | ((uint32_t)sec[0x1C9] << 24);
    if (part_lba == 0U) {
        return SD_FS_UNKNOWN;
    }
    if (HAL_SD_ReadBlocks(&hsd1, sec, part_lba, 1U, SD_TEST_IO_TIMEOUT_MS) != HAL_OK) {
        return SD_FS_UNKNOWN;
    }
    return sd_fs_from_sector(sec);
}

int SD_Test_IntegrityOnce(uint32_t bytes, sd_integrity_t *out)
{
    sd_integrity_t local;

    if (out == NULL) {
        out = &local;
    }
    if (bytes == 0U) {
        bytes = SD_TEST_FILE_SIZE;
    }
    if (!SD_Test_MountReady()) {
        out->bytes = bytes;
        out->mounted = 0;
        out->wrote = 0;
        out->read_back = 0;
        out->identical = 0;
        out->write_crc = 0;
        out->read_crc = 0;
        out->fresult = -1;
        return 0;
    }
    return SD_Test_IntegrityRound(bytes, out);
}

/* Bytes per second from a byte count and a millisecond count, without
 * overflowing and without floating point. 0 when the transfer was too quick to
 * time - reporting a made-up rate would be worse than saying "too fast to
 * measure", which is what a 0 here means. */
static uint32_t sd_bps(uint32_t bytes, uint32_t ms)
{
    if (ms == 0U) {
        return 0U;
    }
    return (uint32_t)(((uint64_t)bytes * 1000ULL) / (uint64_t)ms);
}

int SD_Test_Speed(uint32_t bytes, sd_speed_t *out)
{
    static uint8_t buf[SD_TEST_CHUNK];
    sd_speed_t local;
    FRESULT  fr;
    FIL      fil;
    UINT     done = 0;
    uint32_t left, chunk, t0;
    int      ok = 1;

    if (out == NULL) {
        out = &local;
    }
    if (bytes == 0U) {
        bytes = SD_TEST_FILE_SIZE;
    }
    memset(out, 0, sizeof(*out));
    out->bytes = bytes;

    if (!SD_Test_MountReady()) {
        out->fresult = -1;
        return 0;
    }
    fr = f_mount(&sd_test_fs, sd_test_path, 1);
    out->fresult = (int)fr;
    if (fr != FR_OK) {
        printf("SDCARD_TEST: f_mount FAILED (FRESULT=%d)\r\n", (int)fr);
        return 0;
    }
    out->mounted = 1;

    /* The pattern is irrelevant - only the clock is. A constant fill also
     * keeps a card that compresses from being flattered by random data. */
    memset(buf, 0xA5, sizeof(buf));

    fr = f_open(&fil, SD_TEST_FILE_NAME, FA_CREATE_ALWAYS | FA_WRITE);
    if (fr != FR_OK) {
        out->fresult = (int)fr;
        ok = 0;
    } else {
        left = bytes;
        t0 = HAL_GetTick();
        while ((left > 0U) && ok) {
            chunk = (left > SD_TEST_CHUNK) ? SD_TEST_CHUNK : left;
            fr = f_write(&fil, buf, chunk, &done);
            if ((fr != FR_OK) || (done != chunk)) {
                out->fresult = (int)fr;
                ok = 0;
                break;
            }
            left -= chunk;
        }
        /* Inside the timed span on purpose: a write is not finished until it
         * has been flushed, and a card that buffers everything and pays for it
         * at close would otherwise measure as infinitely fast. */
        if (ok && (f_sync(&fil) != FR_OK)) {
            ok = 0;
        }
        out->write_ms = HAL_GetTick() - t0;
        f_close(&fil);
    }
    if (ok) {
        out->write_bps = sd_bps(bytes, out->write_ms);
    }

    if (ok) {
        fr = f_open(&fil, SD_TEST_FILE_NAME, FA_READ);
        if (fr != FR_OK) {
            out->fresult = (int)fr;
            ok = 0;
        } else {
            left = bytes;
            t0 = HAL_GetTick();
            while ((left > 0U) && ok) {
                chunk = (left > SD_TEST_CHUNK) ? SD_TEST_CHUNK : left;
                fr = f_read(&fil, buf, chunk, &done);
                if ((fr != FR_OK) || (done != chunk)) {
                    out->fresult = (int)fr;
                    ok = 0;
                    break;
                }
                left -= chunk;
            }
            out->read_ms = HAL_GetTick() - t0;
            f_close(&fil);
        }
    }
    if (ok) {
        out->read_bps = sd_bps(bytes, out->read_ms);
        printf("SDCARD_TEST: speed - %lu bytes: write %lu ms (%lu B/s), "
               "read %lu ms (%lu B/s)\r\n",
               (unsigned long)bytes, (unsigned long)out->write_ms,
               (unsigned long)out->write_bps, (unsigned long)out->read_ms,
               (unsigned long)out->read_bps);
    }
    return ok;
}

/* Enough rounds that a card with a bad block or a flaky bus has to show it,
 * while still finishing inside a station's takt time. At 4 KiB a round that
 * is 256 KiB written and 256 KiB read back, every byte verified twice - once
 * against the buffer, once against the CRC. */
#define SD_TEST_STRESS_PASSES 64U

int SD_Test_StressOnce(uint32_t bytes, uint32_t passes, sd_stress_t *out)
{
    sd_stress_t local;
    uint32_t stress_bytes;
    uint32_t start;
    uint32_t i;

    if (out == NULL) {
        out = &local;
    }
    /* 0 for either means "the built-in default". A plan that wants a different
     * shape of stress - fewer, larger rounds, say - says so; how much traffic
     * counts as proof is a production decision, not a firmware one. */
    stress_bytes = (bytes != 0U) ? bytes : SD_TEST_FILE_SIZE;
    if (passes == 0U) {
        passes = SD_TEST_STRESS_PASSES;
    }
    memset(out, 0, sizeof(*out));
    out->bytes_each = stress_bytes;
    out->passes     = passes;

    if (!SD_Test_MountReady()) {
        /* Nothing was attempted, so passes is 0 - not the 64 that were
         * planned. Reporting the plan would say "64 attempted, 0 passed" about
         * a card that was never touched, and the whole point of these numbers
         * is that repair can tell a dead card from a dead bus. */
        out->passes  = 0;
        out->fresult = -1;
        return 0;
    }
    printf("SDCARD_TEST: stress - %lu rounds of %lu bytes write/read/verify\r\n",
           (unsigned long)passes, (unsigned long)stress_bytes);

    start = HAL_GetTick();
    for (i = 0; i < passes; i++) {
        sd_integrity_t r;
        int round_ok = SD_Test_IntegrityRound(stress_bytes, &r);

        /* mounted comes from the round, not from the SD bring-up above: those
         * are two different questions, and reporting bring-up here while
         * sd.integrity reports f_mount under the same field name would make
         * one word mean two things across two targets. Seen on the board
         * 2026-09-08 with an exFAT card - bring-up succeeded, f_mount did
         * not, and the reply said mounted=1. */
        out->mounted = r.mounted;

        if (round_ok) {
            out->passed++;
            out->bytes_total += r.bytes;
        } else {
            out->fresult = r.fresult;
            if (out->first_bad_pass == 0U) {
                out->first_bad_pass = i + 1U;
            }
            /* A card that has stopped answering will fail every remaining
             * round the same way, and each one costs a FatFs timeout. One
             * failure is the answer; grinding through 63 more is not. */
            out->passes = i + 1U;
            break;
        }
    }
    out->elapsed_ms = HAL_GetTick() - start;

    return out->first_bad_pass == 0U;
}

void SD_Test_FileIntegrity(void)
{
    printf("SDCARD_TEST: file integrity test - write/read/verify %s (%u bytes)\r\n",
           SD_TEST_FILE_NAME, (unsigned)SD_TEST_FILE_SIZE);

    if (!SD_Test_MountReady()) {
        for (;;) { HAL_Delay(1000); }
    }

    for (;;) {
        sd_integrity_t r;

        if (SD_Test_IntegrityRound(SD_TEST_FILE_SIZE, &r)) {
            printf("SDCARD_TEST: PASS - %u bytes written and read back identical, "
                   "CRC32=0x%08lX\r\n",
                   (unsigned)r.bytes, (unsigned long)r.write_crc);
        } else if (!r.mounted) {
            HAL_Delay(1000);
            continue;
        }

        HAL_Delay(5000);
    }
}

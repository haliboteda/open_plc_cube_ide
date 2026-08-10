// sd_test.c
//
// Standalone microSD card bring-up test - see sd_test.h for the full
// picture (hardware, why the vendor files are copied in here, and what
// each of the two entry points proves).

// See stm32h7xx_hal_sd.c/stm32h7xx_ll_sdmmc.c in this same folder for why
// this is defined locally instead of touching Core/Inc/stm32h7xx_hal_conf.h.
#include "testinterface_hal_guard.h"   /* fires if this peripheral becomes real -- read it */
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

static uint32_t SD_Test_Crc32(const uint8_t *data, uint32_t len)
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

/* ---- Public entry point 2/2: write + read back + integrity check ----- */

void SD_Test_FileIntegrity(void)
{
    static uint8_t write_buf[SD_TEST_FILE_SIZE];
    static uint8_t read_buf[SD_TEST_FILE_SIZE];
    uint32_t rng_state = HAL_GetTick() | 1U;

    printf("SDCARD_TEST: file integrity test - write/read/verify %s (%u bytes)\r\n",
           SD_TEST_FILE_NAME, (unsigned)SD_TEST_FILE_SIZE);

    if (!SD_Test_Bringup()) {
        printf("SDCARD_TEST: FAIL reason=bringup\r\n");
        for (;;) { HAL_Delay(1000); }
    }

    if (!sd_test_driver_linked) {
        if (FATFS_LinkDriver(&sd_test_driver, sd_test_path) != 0) {
            printf("SDCARD_TEST: FAIL reason=fatfs_link_driver\r\n");
            for (;;) { HAL_Delay(1000); }
        }
        sd_test_driver_linked = 1;
    }

    for (;;) {
        FRESULT fr;
        FIL fil;
        UINT bytes_done;
        uint32_t i;
        uint32_t write_crc;
        uint32_t read_crc;
        int ok = 1;

        fr = f_mount(&sd_test_fs, sd_test_path, 1);
        if (fr != FR_OK) {
            printf("SDCARD_TEST: f_mount FAILED (FRESULT=%d) - retrying in 1s\r\n", (int)fr);
            HAL_Delay(1000);
            continue;
        }

        for (i = 0; i < SD_TEST_FILE_SIZE; i++) {
            write_buf[i] = (uint8_t)SD_Test_Rand(&rng_state);
        }
        write_crc = SD_Test_Crc32(write_buf, SD_TEST_FILE_SIZE);

        fr = f_open(&fil, SD_TEST_FILE_NAME, FA_CREATE_ALWAYS | FA_WRITE);
        if (fr != FR_OK) {
            printf("SDCARD_TEST: FAIL reason=f_open_write (FRESULT=%d)\r\n", (int)fr);
            ok = 0;
        } else {
            fr = f_write(&fil, write_buf, SD_TEST_FILE_SIZE, &bytes_done);
            f_close(&fil);
            if (fr != FR_OK || bytes_done != SD_TEST_FILE_SIZE) {
                printf("SDCARD_TEST: FAIL reason=f_write (FRESULT=%d, wrote %u/%u)\r\n",
                       (int)fr, (unsigned)bytes_done, (unsigned)SD_TEST_FILE_SIZE);
                ok = 0;
            }
        }

        if (ok) {
            fr = f_open(&fil, SD_TEST_FILE_NAME, FA_READ);
            if (fr != FR_OK) {
                printf("SDCARD_TEST: FAIL reason=f_open_read (FRESULT=%d)\r\n", (int)fr);
                ok = 0;
            } else {
                memset(read_buf, 0, SD_TEST_FILE_SIZE);
                fr = f_read(&fil, read_buf, SD_TEST_FILE_SIZE, &bytes_done);
                f_close(&fil);
                if (fr != FR_OK || bytes_done != SD_TEST_FILE_SIZE) {
                    printf("SDCARD_TEST: FAIL reason=f_read (FRESULT=%d, read %u/%u)\r\n",
                           (int)fr, (unsigned)bytes_done, (unsigned)SD_TEST_FILE_SIZE);
                    ok = 0;
                }
            }
        }

        if (ok) {
            read_crc = SD_Test_Crc32(read_buf, SD_TEST_FILE_SIZE);
            if (memcmp(write_buf, read_buf, SD_TEST_FILE_SIZE) != 0 || read_crc != write_crc) {
                printf("SDCARD_TEST: INTEGRITY FAIL - write_crc=0x%08lX read_crc=0x%08lX\r\n",
                       (unsigned long)write_crc, (unsigned long)read_crc);
                ok = 0;
            }
        }

        if (ok) {
            printf("SDCARD_TEST: PASS - %u bytes written and read back identical, CRC32=0x%08lX\r\n",
                   (unsigned)SD_TEST_FILE_SIZE, (unsigned long)write_crc);
        }

        HAL_Delay(5000);
    }
}

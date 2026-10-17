// porttool_calw.c
//
// pt.calw - see porttool_calw.h. Sector 15 into RAM, first 8 KiB replaced,
// erase, write back the words that are not blank, read the whole sector back. $PROD/docs/modules/M4/PORTTOOL-FLOW.md
// A.4 "pt.calw"; layout $PROD/docs/modules/M1/SECTOR-15.md.

#include "porttool.h"
#include "porttool_calw.h"
#include "porttool_cmd.h"

#include "main.h"

#include <stddef.h>
#include <stdio.h>
#include <string.h>

#if PORTTOOL_ENABLE

#define S15_BASE  ((uintptr_t)PORTTOOL_CALIB_BASE)
#define S15_SIZE  FLASH_SECTOR_SIZE
#define WORD_SIZE 32U   /* one H7 flash word, the smallest unit programmed */

/* The erase is by bank and sector number, the writes by address: both must
 * name the same 128 KiB. */
_Static_assert(CALIB_AREA_ADDR == FLASH_BANK2_BASE + FLASH_SECTOR_7 * FLASH_SECTOR_SIZE,
               "the calibration area must start sector 15 (bank 2, sector 7)");
_Static_assert(CALIB_AREA_SIZE % WORD_SIZE == 0U && CALIB_AREA_SIZE <= FLASH_SECTOR_SIZE,
               "the calibration area is whole flash words inside the sector");

/* The sector as it is to be: staged here, programmed from here, compared
 * against. Static: 128 KiB does not belong on the stack. */
static uint8_t s15_image[FLASH_SECTOR_SIZE] __attribute__((aligned(32)));

int PortTool_S15Program(uintptr_t dst, const uint8_t *src)
{
    /* The hard guard: nothing outside sector 15 is ever handed to the HAL. */
    if (dst < S15_BASE || dst - S15_BASE > S15_SIZE - WORD_SIZE
        || (dst - S15_BASE) % WORD_SIZE != 0U) {
        return 0;
    }
    return HAL_FLASH_Program(FLASH_TYPEPROGRAM_FLASHWORD, dst, (uintptr_t)src) == HAL_OK;
}

/* Bank 2 sector 7 and nothing else: the numbers are fixed here, not taken
 * from the command. */
static int s15_erase(void)
{
    FLASH_EraseInitTypeDef e;
    uint32_t bad = 0;

    memset(&e, 0, sizeof(e));
    e.TypeErase    = FLASH_TYPEERASE_SECTORS;
    e.Banks        = FLASH_BANK_2;
    e.Sector       = FLASH_SECTOR_7;
    e.NbSectors    = 1U;
    e.VoltageRange = FLASH_VOLTAGE_RANGE_3;
    return HAL_FLASHEx_Erase(&e, &bad) == HAL_OK;
}

static int word_is_blank(const uint8_t *p)
{
    for (uint32_t i = 0; i < WORD_SIZE; i++) {
        if (p[i] != 0xFFU) {
            return 0;
        }
    }
    return 1;
}

/* Why the record is refused, or NULL. Checks what the app checks before it
 * uses one (SECTOR-15.md), so nothing the app would ignore gets burned. */
static const char *record_refusal(const calib_area_t *rec)
{
    const volatile uint32_t *uid = (const volatile uint32_t *)UID_BASE;

    if (rec->magic != CALIB_MAGIC) { return "bad magic"; }
    if (rec->version != CALIB_VERSION) { return "unknown version"; }
    if (rec->channels != CALIB_CHANNELS) { return "wrong channel count"; }
    if (rec->crc32 != ~PortTool_Crc32Update(0xFFFFFFFFUL, (const uint8_t *)rec, offsetof(calib_area_t, crc32))) {
        return "crc does not match";
    }
    if (rec->uid[0] != uid[0] || rec->uid[1] != uid[1] || rec->uid[2] != uid[2]) {
        return "uid is not this board's";
    }
    /* Bank swap would put bank 2 sector 7 somewhere other than CALIB_AREA_ADDR. */
    if ((FLASH->OPTCR & FLASH_OPTCR_SWAP_BANK) != 0U) {
        return "flash banks are swapped";
    }
    return NULL;
}

void PortTool_CalWrite(const char *args)
{
    char hex[2U * sizeof(calib_area_t) + 2U];   /* one spare: too long shows */
    calib_area_t rec;
    uint8_t *raw = (uint8_t *)&rec;
    const char *rest = NULL;
    const char *why;
    uint32_t words = 0;
    uint32_t t0;

    PortCmd_Word(args != NULL ? args : "", hex, sizeof(hex), &rest);
    if (strlen(hex) != 2U * sizeof(calib_area_t) || (rest != NULL && *rest != '\0')) {
        printf("ERR calw refused: want %u hex digits\r\n", (unsigned)(2U * sizeof(calib_area_t)));
        return;
    }
    for (uint32_t i = 0; i < sizeof(calib_area_t); i++) {
        int hi = PortCmd_HexDigit(hex[2U * i]);
        int lo = PortCmd_HexDigit(hex[2U * i + 1U]);
        if (hi < 0 || lo < 0) {
            printf("ERR calw refused: not hex\r\n");
            return;
        }
        raw[i] = (uint8_t)((hi << 4) | lo);
    }
    why = record_refusal(&rec);
    if (why != NULL) {
        printf("ERR calw refused: %s\r\n", why);
        return;
    }

    t0 = HAL_GetTick();
    memcpy(s15_image, (const void *)S15_BASE, S15_SIZE);
    memset(s15_image, 0xFF, CALIB_AREA_SIZE);
    memcpy(s15_image, &rec, sizeof(rec));

    HAL_FLASH_Unlock();
    __HAL_FLASH_CLEAR_FLAG_BANK2(FLASH_FLAG_ALL_ERRORS_BANK2);

    if (!s15_erase()) {
        HAL_FLASH_Lock();
        printf("ERR calw failed: erase err=0x%08lX\r\n", (unsigned long)HAL_FLASH_GetError());
        return;
    }
    /* Blank words stay erased: the bootloader appends into them later. */
    for (uint32_t off = 0; off < S15_SIZE; off += WORD_SIZE) {
        if (word_is_blank(&s15_image[off])) {
            continue;
        }
        if (!PortTool_S15Program(S15_BASE + off, &s15_image[off])) {
            HAL_FLASH_Lock();
            printf("ERR calw failed: program at +0x%05lX err=0x%08lX\r\n",
                   (unsigned long)off, (unsigned long)HAL_FLASH_GetError());
            return;
        }
        words++;
    }
    HAL_FLASH_Lock();

    /* D-cache stays off in this image (main.c), so this reads the flash. */
    for (uint32_t off = 0; off < S15_SIZE; off += WORD_SIZE) {
        if (memcmp((const void *)(S15_BASE + off), &s15_image[off], WORD_SIZE) != 0) {
            printf("ERR calw failed: verify at +0x%05lX\r\n", (unsigned long)off);
            return;
        }
    }
    if (__HAL_FLASH_GET_FLAG_BANK2(FLASH_FLAG_SNECCERR_BANK2)
        || __HAL_FLASH_GET_FLAG_BANK2(FLASH_FLAG_DBECCERR_BANK2)) {
        __HAL_FLASH_CLEAR_FLAG_BANK2(FLASH_FLAG_SNECCERR_BANK2 | FLASH_FLAG_DBECCERR_BANK2);
        printf("ERR calw failed: verify saw an ECC error\r\n");
        return;
    }

    printf("OK calw words=%lu ms=%lu\r\n", (unsigned long)words,
           (unsigned long)(HAL_GetTick() - t0));
}

#endif /* PORTTOOL_ENABLE */

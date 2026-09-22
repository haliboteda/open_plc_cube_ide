/*
 * boot_selfupgrade.c -- see boot_selfupgrade.h.
 *
 * Everything between the erase and the last write runs from RAM with
 * interrupts off. Nothing in that stretch may touch flash: not a HAL call,
 * not printf, not memcpy (the compiler's version lives in flash), and not an
 * interrupt -- the vector table is in sector 0 too.
 *
 * The register sequence mirrors HAL_FLASH_Program() and FLASH_Erase_Sector()
 * in Drivers/STM32H7xx_HAL_Driver. Only the constants come from the HAL
 * headers; the code is copied rather than called because the HAL itself is in
 * the sector being erased.
 */

#include "boot_selfupgrade.h"
#include "owner_slot.h"
#include "IAP_config.h"
#include "stm32h7xx_hal.h"
#include <stdio.h>
#include <inttypes.h>

/* Sector 0 of bank 1: the bootloader and, in its top 8 KiB, the owner record
 * area. One erase takes both, which is why the owner records are carried. */
#define BOOT_SECTOR_BASE   ((uint32_t)FLASH_BASE)
#define BOOT_SECTOR_NUMBER 0U
#define BOOT_IMAGE_MAX     (OWNER_SLOT_BASE - BOOT_SECTOR_BASE)   /* 120 KiB */

/* Where the owner records wait out the erase. Past the 128 KiB a bootloader
 * image can reach, so the two never overlap. A full application image is far
 * larger than that, but one is never staged during a flashboot. */
#define OWNER_CARRY_BASE   (IAP_STAGE_BASE + (128U * 1024U))

_Static_assert(OWNER_CARRY_BASE + OWNER_SLOT_SIZE <= IAP_STAGE_BASE + IAP_STAGE_SIZE,
		"the owner carry buffer does not fit in the SDRAM staging region");
_Static_assert(BOOT_IMAGE_MAX == (120U * 1024U),
		"the bootloader region is not 120 KiB -- check OWNER_SLOT_BASE");

/* A bounded spin, not a timeout in milliseconds: SysTick cannot advance with
 * interrupts off, so HAL_GetTick() would stand still. The count only has to
 * be larger than the longest sector erase; falling through it is reported as
 * a failure either way. */
#define FLASH_SPIN_LIMIT   0x40000000UL

uint32_t boot_selfupgrade_max_size(void)
{
	return BOOT_IMAGE_MAX;
}

/* Wait for bank 1 to go idle. Returns false if it never does, or if the
 * operation set an error flag. */
__attribute__((section(".RamFunc"), noinline))
static bool ram_wait_bank1(void)
{
	uint32_t spins = FLASH_SPIN_LIMIT;

	while ((FLASH->SR1 & FLASH_SR_QW) != 0U) {
		if (--spins == 0U) {
			return false;
		}
	}
	if ((FLASH->SR1 & FLASH_FLAG_ALL_ERRORS_BANK1) != 0U) {
		FLASH->CCR1 = FLASH_FLAG_ALL_ERRORS_BANK1;
		return false;
	}
	if ((FLASH->SR1 & FLASH_SR_EOP) != 0U) {
		FLASH->CCR1 = FLASH_SR_EOP;
	}
	return true;
}

/* Program `len` bytes (a multiple of 32) at `dst`, then read them back.
 * Copies 32-bit words by hand: a memcpy call would jump into erased flash. */
__attribute__((section(".RamFunc"), noinline))
static bool ram_program(uint32_t dst, const uint32_t *src, uint32_t len)
{
	uint32_t off;

	for (off = 0U; off < len; off += 32U) {
		volatile uint32_t *d = (volatile uint32_t *)(dst + off);
		const uint32_t *s = &src[off / 4U];
		uint32_t i;

		SET_BIT(FLASH->CR1, FLASH_CR_PG);
		__ISB();
		__DSB();

		for (i = 0U; i < 8U; i++) {
			d[i] = s[i];
		}

		__ISB();
		__DSB();

		if (!ram_wait_bank1()) {
			CLEAR_BIT(FLASH->CR1, FLASH_CR_PG);
			return false;
		}
		CLEAR_BIT(FLASH->CR1, FLASH_CR_PG);

		for (i = 0U; i < 8U; i++) {
			if (d[i] != s[i]) {
				return false;
			}
		}
	}
	return true;
}

/*
 * The point of no return. Erases sector 0, writes the owner records back
 * first, then the new bootloader, and resets.
 *
 * Owner first is deliberate: a power cut between the two writes then leaves a
 * board that will not boot but is still owned, which an ST-Link reflash
 * recovers. The other order leaves it unowned, and whoever presses BOOT0
 * next takes it.
 */
__attribute__((section(".RamFunc"), noinline, noreturn))
static void ram_burn(uint32_t image_words, const uint32_t *image, const uint32_t *owner)
{
	(void)ram_wait_bank1();

	FLASH->CR1 &= ~(FLASH_CR_PSIZE | FLASH_CR_SNB);
	FLASH->CR1 |= (FLASH_CR_SER | FLASH_VOLTAGE_RANGE_3
			| (BOOT_SECTOR_NUMBER << FLASH_CR_SNB_Pos) | FLASH_CR_START);

	if (ram_wait_bank1()) {
		CLEAR_BIT(FLASH->CR1, FLASH_CR_SER);
		if (ram_program(OWNER_SLOT_BASE, owner, OWNER_SLOT_SIZE)) {
			(void)ram_program(BOOT_SECTOR_BASE, image, image_words * 4U);
		}
	}

	FLASH->CR1 |= FLASH_CR_LOCK;
	NVIC_SystemReset();

	for (;;) {
		/* NVIC_SystemReset() does not come back; this keeps the compiler
		 * from complaining about a noreturn function that falls through. */
	}
}

bool boot_selfupgrade_commit(uint32_t image_size)
{
	/* HAL_FLASH_Program always commits a whole 32-byte flash word, so the
	 * image is padded up with the erased value. */
	const uint32_t padded = (image_size + 31U) & ~31U;
	uint8_t *carry = (uint8_t *)OWNER_CARRY_BASE;
	const uint8_t *owner = (const uint8_t *)OWNER_SLOT_BASE;
	uint32_t i;

	if ((image_size == 0U) || (padded > BOOT_IMAGE_MAX)) {
		printf("flashboot: %" PRIu32 " bytes does not fit the %" PRIu32 "-byte bootloader region\r\n",
				image_size, (uint32_t)BOOT_IMAGE_MAX);
		return false;
	}

	for (i = image_size; i < padded; i++) {
		((uint8_t *)IAP_STAGE_BASE)[i] = 0xFFU;
	}

	/* Read the owner records out while flash is still readable.
	 *
	 * Compacted here, before interrupts go down, so the decisions are made by
	 * ordinary code that can still print and still read flash -- ram_burn()
	 * only ever programs the buffer it is handed. An erase is the one moment
	 * these slots can be reclaimed, and nothing else reclaims them.
	 *
	 * If compaction refuses, the area goes over exactly as it stands: that
	 * reclaims nothing but cannot lose ownership. */
	if (!owner_slot_compact(carry)) {
		for (i = 0U; i < OWNER_SLOT_SIZE; i++) {
			carry[i] = owner[i];
		}
	}

	HAL_FLASH_Unlock();
	if ((FLASH->CR1 & FLASH_CR_LOCK) != 0U) {
		printf("flashboot: bank 1 will not unlock, nothing was erased\r\n");
		return false;
	}
	FLASH->CCR1 = FLASH_FLAG_ALL_ERRORS_BANK1;

	printf("flashboot: erasing and rewriting sector 0 (%" PRIu32 " bytes). "
			"DO NOT CUT POWER.\r\n", padded);
	fflush(stdout);
	HAL_Delay(200);   /* let that line leave the UART before interrupts stop */

	/* The I-cache would otherwise keep serving instructions from a sector
	 * that no longer holds them. Interrupts go last, so nothing between here
	 * and the reset can vector into erased flash. */
	SCB_DisableICache();
	__disable_irq();

	ram_burn(padded / 4U, (const uint32_t *)IAP_STAGE_BASE, (const uint32_t *)carry);
}

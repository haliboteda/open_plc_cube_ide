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
#include "IAP_config.h"
#include "stm32h7xx_hal.h"
#include <stdio.h>
#include <inttypes.h>

/* Sector 0 of bank 1 holds the bootloader and nothing else: the root area is
 * in sector 15 (decision 72), so a flashboot carries nothing across. */
#define BOOT_SECTOR_BASE   ((uint32_t)FLASH_BASE)
#define BOOT_SECTOR_NUMBER 0U
#define BOOT_IMAGE_MAX     (128U * 1024U)

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
 * The point of no return. Erases sector 0, writes the new bootloader and
 * resets. A power cut in between leaves a board that will not boot but still
 * owns its root (sector 15); an ST-Link reflash recovers it.
 */
__attribute__((section(".RamFunc"), noinline, noreturn))
static void ram_burn(uint32_t image_words, const uint32_t *image)
{
	(void)ram_wait_bank1();

	FLASH->CR1 &= ~(FLASH_CR_PSIZE | FLASH_CR_SNB);
	FLASH->CR1 |= (FLASH_CR_SER | FLASH_VOLTAGE_RANGE_3
			| (BOOT_SECTOR_NUMBER << FLASH_CR_SNB_Pos) | FLASH_CR_START);

	if (ram_wait_bank1()) {
		CLEAR_BIT(FLASH->CR1, FLASH_CR_SER);
		(void)ram_program(BOOT_SECTOR_BASE, image, image_words * 4U);
	}

	FLASH->CR1 |= FLASH_CR_LOCK;
	NVIC_SystemReset();

	for (;;) {
		/* NVIC_SystemReset() does not come back; this keeps the compiler
		 * from complaining about a noreturn function that falls through. */
	}
}

/*
 * Everything from "the image is staged" to the point of no return; `what`
 * only names the operation in the log.
 *
 * Returns false only if the flash will not unlock. Otherwise it does not
 * return: ram_burn() resets the board.
 */
static bool arm_and_burn(uint32_t image_size, const char *what)
{
	HAL_FLASH_Unlock();
	if ((FLASH->CR1 & FLASH_CR_LOCK) != 0U) {
		printf("%s: bank 1 will not unlock, nothing was erased\r\n", what);
		return false;
	}
	FLASH->CCR1 = FLASH_FLAG_ALL_ERRORS_BANK1;

	printf("%s: erasing and rewriting sector 0 (%" PRIu32 " bytes). "
			"DO NOT CUT POWER.\r\n", what, image_size);
	fflush(stdout);
	HAL_Delay(200);   /* let that line leave the UART before interrupts stop */

	/* The I-cache would otherwise keep serving instructions from a sector
	 * that no longer holds them. Interrupts go last, so nothing between here
	 * and the reset can vector into erased flash. */
	SCB_DisableICache();
	__disable_irq();

	ram_burn(image_size / 4U, (const uint32_t *)IAP_STAGE_BASE);
}

bool boot_selfupgrade_commit(uint32_t image_size)
{
	/* HAL_FLASH_Program always commits a whole 32-byte flash word, so the
	 * image is padded up with the erased value. */
	const uint32_t padded = (image_size + 31U) & ~31U;
	uint32_t i;

	if ((image_size == 0U) || (padded > BOOT_IMAGE_MAX)) {
		printf("flashboot: %" PRIu32 " bytes does not fit the %" PRIu32 "-byte bootloader region\r\n",
				image_size, (uint32_t)BOOT_IMAGE_MAX);
		return false;
	}

	for (i = image_size; i < padded; i++) {
		((uint8_t *)IAP_STAGE_BASE)[i] = 0xFFU;
	}

	return arm_and_burn(padded, "flashboot");
}

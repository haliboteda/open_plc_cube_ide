/*
 * IAP_boot_handoff.c
 *
 * See IAP_boot_handoff.h. Mirrored into the Arduino core at
 * cores/arduino/stm32/IAP_boot_handoff.c -- the two copies differ only in the
 * include block below, same arrangement as iap_auth.c / iap_keyderive.c.
 */

#include "IAP_boot_handoff.h"
#include "main.h"
#include <stdio.h>

static bool                  s_taken;
static boot_req_t            s_taken_mode = BOOT_REQ_NONE;
static boot_handoff_status_t s_status     = BOOT_HANDOFF_OK;
static uint32_t              s_reset_rsr;
static bool                  s_rsr_latched;

static volatile boot_handoff_t *record(void)
{
	return (volatile boot_handoff_t *)BOOT_HANDOFF_ADDR;
}

static uint32_t expected_check(uint16_t version, uint32_t mode)
{
	return ~(BOOT_HANDOFF_MAGIC ^ (uint32_t)version ^ mode);
}

/*
 * Push our stores out to physical memory and drop any cached copy, so that the
 * read-back below really reads memory instead of the cache line we just wrote.
 *
 * Both maintenance calls are harmless no-ops when the data cache is disabled,
 * which is the point: this file stays correct no matter how the running image
 * has configured its caches, and it needs no MPU region. The bootloader can
 * therefore not care what a user's PLC application does with the D-cache.
 */
static void sync_to_memory(void)
{
	__DSB();
	SCB_CleanDCache_by_Addr((uint32_t *)BOOT_HANDOFF_ADDR, (int32_t)BOOT_HANDOFF_SIZE);
	SCB_InvalidateDCache_by_Addr((uint32_t *)BOOT_HANDOFF_ADDR, (int32_t)BOOT_HANDOFF_SIZE);
	__DSB();
	__ISB();
}

static void clear_record(void)
{
	volatile boot_handoff_t *h = record();

	h->magic   = 0U;
	h->version = 0U;
	h->size    = 0U;
	h->mode    = 0U;
	h->check   = 0U;
	sync_to_memory();
}

/*
 * Cover the whole reserved area, not just the fields this version defines.
 *
 * A power-on reset leaves SRAM4 and its ECC check bits random, and the two do
 * not agree, so the first access to any word here has to be a write -- reading
 * one first can raise an uncorrectable ECC error and take a BusFault. Only the
 * cold-boot path calls this, and after it every word in the region is safe to
 * read for the rest of the power cycle, including the bytes past the record
 * that a later version may start using. clear_record() stays scoped to the
 * record itself so that consuming a request never wipes a neighbour.
 */
static void init_reserved_area(void)
{
	volatile uint32_t *word = (volatile uint32_t *)BOOT_HANDOFF_ADDR;

	for (uint32_t i = 0U; i < (BOOT_HANDOFF_SIZE / sizeof(uint32_t)); i++) {
		word[i] = 0U;
	}
	sync_to_memory();
}

void boot_handoff_latch_reset_cause(void)
{
	if (s_rsr_latched) {
		return;
	}
	s_reset_rsr   = RCC->RSR;
	s_rsr_latched = true;
	__HAL_RCC_CLEAR_RESET_FLAGS();
}

bool boot_handoff_request(boot_req_t mode)
{
	volatile boot_handoff_t *h = record();
	const uint32_t chk = expected_check((uint16_t)BOOT_HANDOFF_VERSION, (uint32_t)mode);

	h->magic   = BOOT_HANDOFF_MAGIC;
	h->version = (uint16_t)BOOT_HANDOFF_VERSION;
	h->size    = (uint16_t)sizeof(boot_handoff_t);
	h->mode    = (uint32_t)mode;
	h->check   = chk;
	sync_to_memory();

	/* Read-back verification. This is the guard the old RTC-backup-register
	 * scheme lacked: a write that the hardware discarded looked identical to a
	 * write that succeeded, so the board reset with the old flag still in place
	 * and booted the wrong mode with nothing logged anywhere. */
	if ((h->magic != BOOT_HANDOFF_MAGIC) ||
	    (h->version != (uint16_t)BOOT_HANDOFF_VERSION) ||
	    (h->mode != (uint32_t)mode) ||
	    (h->check != chk)) {
		printf("boot_handoff: write did not take effect, not resetting\r\n");
		return false;
	}

	HAL_NVIC_SystemReset();
	for (;;) {
		/* not reached */
	}
}

boot_req_t boot_handoff_take(void)
{
	volatile boot_handoff_t *h = record();
	uint32_t magic;
	uint16_t version;
	uint32_t mode;
	uint32_t chk;
	bool cold;

	if (s_taken) {
		return s_taken_mode;
	}
	s_taken = true;

	/*
	 * On a cold boot we only ever write -- see init_reserved_area(). A genuine
	 * request can only have come from a warm reset, so that is also the only
	 * path that reads the record.
	 *
	 * The reset cause is normally latched much earlier, by the call in main().
	 * Doing it here as well keeps this function correct on its own: RCC->RSR
	 * survives until someone writes RMVF, and HAL_RCC_DeInit() does exactly
	 * that, so whoever reads it last would see nothing.
	 */
	boot_handoff_latch_reset_cause();
	cold = ((s_reset_rsr & RCC_RSR_PORRSTF) != 0U);

	if (cold) {
		init_reserved_area();
		s_status     = BOOT_HANDOFF_COLD_BOOT;
		s_taken_mode = BOOT_REQ_NONE;
		return s_taken_mode;
	}

	SCB_InvalidateDCache_by_Addr((uint32_t *)BOOT_HANDOFF_ADDR, (int32_t)BOOT_HANDOFF_SIZE);
	__DSB();

	magic   = h->magic;
	version = h->version;
	mode    = h->mode;
	chk     = h->check;

	if (magic == 0U) {
		/* The common case, and deliberately silent: nobody asked for anything.
		 * Reset button, watchdog, an application that reset itself -- all land
		 * here on every single boot, so reporting it would drown the log. */
		s_status     = BOOT_HANDOFF_OK;
		s_taken_mode = BOOT_REQ_NONE;
	} else if ((magic != BOOT_HANDOFF_MAGIC) || (chk != expected_check(version, mode))) {
		/* Only this file writes here, and it only ever stores 0 or MAGIC. Any
		 * third value means something wrote where it should not have (a stray
		 * pointer, a DMA, a linker script that gave the area away) or a bit
		 * flipped. Report it, but still boot the application: the signature
		 * check is the real gate, and pinning the board in the bootloader over
		 * one corrupted word would turn a glitch into an outage. */
		printf("boot_handoff: corrupt record (magic=%08lX ver=%u mode=%lu chk=%08lX)\r\n",
		       (unsigned long)magic, (unsigned)version,
		       (unsigned long)mode, (unsigned long)chk);
		s_status     = BOOT_HANDOFF_CORRUPT;
		s_taken_mode = BOOT_REQ_NONE;
	} else {
		/* Valid record. */
		if (version > (uint16_t)BOOT_HANDOFF_VERSION) {
			/* Written by a newer image than this bootloader understands. The
			 * header is frozen, so `mode` is still meaningful; we just cannot
			 * see whatever was appended after `check`. */
			printf("boot_handoff: record version %u newer than supported %u,"
			       " using frozen header fields only\r\n",
			       (unsigned)version, (unsigned)BOOT_HANDOFF_VERSION);
			s_status = BOOT_HANDOFF_UNKNOWN_VERSION;
		} else {
			s_status = BOOT_HANDOFF_OK;
		}

		switch (mode) {
		case (uint32_t)BOOT_REQ_CDC:
			s_taken_mode = BOOT_REQ_CDC;
			break;
		case (uint32_t)BOOT_REQ_ETH:
			s_taken_mode = BOOT_REQ_ETH;
			break;
		default:
			/* Somebody asked us to stay, but we cannot tell which channel they
			 * are on. Staying and serving everything is recoverable; jumping
			 * into the application would discard a request that explicitly told
			 * us not to. Do not guess a single channel. */
			printf("boot_handoff: unusable mode %lu, staying in bootloader on all channels\r\n",
			       (unsigned long)mode);
			s_status     = BOOT_HANDOFF_BAD_MODE;
			s_taken_mode = BOOT_REQ_ALL;
			break;
		}
	}

	/* Consume it either way: read once and it is gone, so no stale record can
	 * ever hold the board in the bootloader across reboots. */
	clear_record();
	return s_taken_mode;
}

boot_handoff_status_t boot_handoff_last_status(void)
{
	return s_status;
}

/*
 * owner_slot.c -- see owner_slot.h.
 *
 * Step 2 of 6: read and parse. Nothing here writes flash.
 */

#include "owner_slot.h"
#include "fw_pubkey.h"
#include <stdio.h>
#include <string.h>
#include <inttypes.h>

/* The record must be exactly five 32-byte flash words. If a field is ever
 * added or the compiler pads differently, this stops the build rather than
 * letting a board write records the next firmware cannot read. Same reasoning
 * as the journal record in bootloader_state.h. */
_Static_assert(sizeof(owner_record_t) == OWNER_RECORD_SIZE,
		"owner_record_t must be exactly 160 bytes (5 x 32-byte flash words)");
_Static_assert(OWNER_SLOT_MAX_RECORDS == 51U,
		"8K / 160 B should give 51 records; check OWNER_SLOT_SIZE");

static bool     s_scanned;
static bool     s_empty = true;
static uint32_t s_valid_count;
static uint32_t s_ignored_count;
static const owner_record_t *s_latest;   /* highest generation, structurally valid */

static const owner_record_t *record_at(uint32_t index)
{
	return (const owner_record_t *)(OWNER_SLOT_BASE + (index * OWNER_RECORD_SIZE));
}

/*
 * Structural validity only -- this says nothing about whether the record is
 * *authorised*. That is prev_sig's job and it is not checked yet (step 5).
 */
static bool record_is_structurally_valid(const owner_record_t *r)
{
	if (r->type != (uint8_t)OWNER_RECORD_TYPE) {
		return false;
	}
	if (r->slots != (uint8_t)OWNER_RECORD_SLOTS) {
		return false;
	}
	if (r->format_ver != (uint16_t)OWNER_FORMAT_VER) {
		/* A newer format is not corruption -- it is a record this firmware
		 * is too old to understand. Counted separately so the boot line can
		 * say which it is. */
		return false;
	}
	return true;
}

static bool record_is_erased(const owner_record_t *r)
{
	const uint8_t *p = (const uint8_t *)r;
	uint32_t i;

	for (i = 0U; i < OWNER_RECORD_SIZE; i++) {
		if (p[i] != 0xFFU) {
			return false;
		}
	}
	return true;
}

void owner_slot_init(void)
{
	uint32_t i;

	if (s_scanned) {
		return;
	}
	s_scanned = true;
	s_empty = true;
	s_valid_count = 0U;
	s_ignored_count = 0U;
	s_latest = NULL;

	/* Scan every slot rather than stopping at the first erased one. An append
	 * interrupted by a power cut can leave a half-written record with erased
	 * ones on both sides; stopping early would hide every record after it. */
	for (i = 0U; i < OWNER_SLOT_MAX_RECORDS; i++) {
		const owner_record_t *r = record_at(i);

		if (record_is_erased(r)) {
			continue;
		}
		s_empty = false;

		if (!record_is_structurally_valid(r)) {
			s_ignored_count++;
			continue;
		}
		s_valid_count++;

		/* Highest generation wins. Equal generations cannot be ordered, so
		 * the later slot is taken -- appends only ever move forward. */
		if (s_latest == NULL || r->generation >= s_latest->generation) {
			s_latest = r;
		}
	}
}

const uint8_t *owner_slot_root(void)
{
	owner_slot_init();

	/*
	 * ⚠️ Always the compiled-in root, for now.
	 *
	 * Returning s_latest->root_pubkey here would be the whole point of the
	 * feature -- and would also mean that anything able to write these 8K
	 * could appoint itself the trust root, because nothing verifies prev_sig
	 * yet (step 5). That is strictly worse than not having an owner slot, so
	 * the switch stays off until the check that makes it safe exists.
	 */
	return fw_public_key;
}

bool owner_slot_is_empty(void)
{
	owner_slot_init();
	return s_empty;
}

uint32_t owner_slot_record_count(void)
{
	owner_slot_init();
	return s_valid_count;
}

void owner_slot_report(void)
{
	owner_slot_init();

	if (s_empty) {
		printf("Owner slot: empty, using the built-in root key\r\n");
		return;
	}

	if (s_latest != NULL) {
		printf("Owner slot: %" PRIu32 " record(s), latest generation %" PRIu32 "%s\r\n",
				s_valid_count, s_latest->generation,
				((s_latest->flags & OWNER_FLAG_CLEARED) != 0UL) ? " (cleared)" : "");
	}
	if (s_ignored_count > 0U) {
		printf("Owner slot: %" PRIu32 " record(s) ignored - wrong format or corrupt\r\n",
				s_ignored_count);
	}

	/* Loud on purpose. A board carrying owner records that are not being
	 * honoured is a confusing state, and silence would let it look normal. */
	printf("** Owner records present but NOT in effect: signature checking is not "
			"implemented yet. Still using the built-in root. **\r\n");
}

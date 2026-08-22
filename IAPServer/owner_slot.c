/*
 * owner_slot.c -- see owner_slot.h.
 *
 * Step 2 of 6: read and parse. Nothing here writes flash.
 */

#include "owner_slot.h"
#include "fw_pubkey.h"
#include "fw_verify.h"
#include "sha256.h"
#include "usbd_cdc_flash.h"
#include <stdio.h>
#include <string.h>
#include <inttypes.h>

/*
 * SHA-256 of the 64 raw bytes (X||Y) of the root key published with this
 * project -- the one whose private half lives in IAPServer/keys, because
 * customers must be able to sign their own sketches with it.
 *
 * A fingerprint rather than a second copy of the key, so nobody can mistake it
 * for something that participates in verification. Nothing is verified against
 * it; it answers exactly one question, "is this board still trusting the key
 * everybody has".
 *
 * ⚠️ This is a CONSTANT, not something derived from fw_public_key at build
 * time. Deriving it would make the comparison trivially true for every build
 * and the warning would fire on customer boards that are perfectly safe --
 * which is the failure mode docs/design/OWNERSHIP.md spends a page warning about,
 * because a warning everybody learns to ignore protects nobody.
 *
 * ⚠️ If the project ever ships a DIFFERENT default key (rotate_keys.sh), that
 * new key is also public and this constant must be regenerated, or factory
 * boards silently stop warning. Case P6 in the test suite compares the two and
 * fails when they drift.
 *
 *   Regenerate:  parse IAPServer/keys/fw_pubkey.inc into 64 bytes, SHA-256 it.
 *                $TOOL/TestTool/tools/check-public-root.ps1 prints the value.
 */
static const uint8_t k_published_root_sha256[32] = {
	0xa3, 0xcb, 0xcb, 0xf7, 0x9f, 0xb6, 0x65, 0xdf,
	0x35, 0xcd, 0x14, 0xe1, 0x44, 0xda, 0x9b, 0xac,
	0x15, 0x6b, 0x84, 0xda, 0x86, 0x9b, 0x2c, 0x9f,
	0x10, 0x9e, 0x39, 0xde, 0xf8, 0xc3, 0x8b, 0xde,
};

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
static uint32_t s_torn_count;            /* body written, header never was */
static uint32_t s_unauthorised_count;    /* structurally fine, not entitled to apply */
static const owner_record_t *s_latest;   /* highest generation, structurally valid */
static const owner_record_t *s_effective; /* end of the trusted chain, or NULL */

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

static bool sig_is_absent(const owner_record_t *r)
{
	uint32_t i;

	for (i = 0U; i < sizeof(r->prev_sig); i++) {
		if (r->prev_sig[i] != 0U) {
			return false;
		}
	}
	return true;
}

/*
 * Decide which record's key is actually in force.
 *
 * Records are walked oldest first, never "highest generation wins". That
 * shortcut would be a hole: on a board already claimed by G1, anybody able to
 * append a record could write a second unsigned one with a higher generation
 * and take the board over. Authority has to come from the chain, not from
 * being last.
 *
 *   first record        may carry no signature -- that is the initial claim,
 *                       gated by physical presence when it was written (TOFU)
 *   cleared record      may carry no signature -- factory reset is gated the
 *                       same way, and requiring the current owner's signature
 *                       would brick a board whose owner lost the key (R3)
 *   anything else       must be signed by the root currently in force
 *
 * A record that fails stops the walk rather than being skipped. Skipping would
 * let an attacker invalidate one link and have the rest of the chain -- written
 * under a different owner -- silently apply.
 *
 * ⚠️ Signature verification is step 5 and does not exist yet, so for now any
 * record that would need one stops the walk. That is the safe direction: the
 * board keeps the root it already trusted.
 */
static void resolve_chain(void)
{
	const owner_record_t *current = NULL;
	uint32_t last_gen = 0U;
	bool first = true;
	bool prev_cleared = false;

	for (;;) {
		const owner_record_t *next = NULL;
		uint32_t i;

		/* Lowest generation strictly above the one just applied. 51 slots, so
		 * a linear pass per link is cheaper than sorting. */
		for (i = 0U; i < OWNER_SLOT_MAX_RECORDS; i++) {
			const owner_record_t *r = record_at(i);

			if (!record_is_structurally_valid(r)) {
				continue;
			}
			if (!first && (r->generation <= last_gen)) {
				continue;
			}
			if ((next == NULL) || (r->generation < next->generation)) {
				next = r;
			}
		}
		if (next == NULL) {
			break;
		}

		bool cleared = ((next->flags & OWNER_FLAG_CLEARED) != 0UL);

		if (sig_is_absent(next)) {
			/*
			 * Unsigned records are allowed in exactly three places, all of
			 * which are ones where a signature is either impossible or would
			 * defeat the purpose:
			 *
			 *   first          the initial claim -- there is no owner yet to
			 *                  sign it, so physical presence is the only gate
			 *                  there can be (TOFU)
			 *   cleared        factory reset -- requiring the current owner's
			 *                  signature would leave a board whose owner lost
			 *                  the key permanently unusable (R3)
			 *   prev_cleared   a fresh claim after a reset: the board is back
			 *                  in TOFU, so this is the "first" case again
			 *
			 * Anywhere else, an unsigned record is exactly what an attacker
			 * would write, and is refused.
			 */
			if (!first && !cleared && !prev_cleared) {
				s_unauthorised_count++;
				break;
			}
		} else {
			/* Must be signed by the root in force at this point in the chain --
			 * not the board's built-in key. Following the chain is the whole
			 * mechanism: each owner authorises the next. After a reset there is
			 * no owner, so the built-in root stands in and the board is once
			 * again as open as a factory board -- which is what a reset means. */
			uint8_t digest[SHA256_DIGEST_SIZE];
			const uint8_t *signer = ((current != NULL) && !prev_cleared)
					? current->root_pubkey : fw_public_key;

			sha256((const uint8_t *)next, OWNER_SIGNED_PREFIX_LEN, digest);
			if (!fw_verify_signature_with_key(signer, digest, next->prev_sig)) {
				s_unauthorised_count++;
				break;
			}
		}

		current = next;
		last_gen = next->generation;
		prev_cleared = cleared;
		first = false;
	}

	s_effective = current;
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
	s_torn_count = 0U;
	s_unauthorised_count = 0U;
	s_latest = NULL;
	s_effective = NULL;

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
			/* A record is written body-first, header last (see
			 * owner_slot_claim). So "header still erased, body is not" is the
			 * signature of a write interrupted by a power cut, not corruption
			 * -- worth telling apart, because one is expected and harmless and
			 * the other means something is wrong with the flash. */
			if (r->type == OWNER_RECORD_ERASED) {
				s_torn_count++;
			} else {
				s_ignored_count++;
			}
			continue;
		}
		s_valid_count++;

		if (s_latest == NULL || r->generation >= s_latest->generation) {
			s_latest = r;
		}
	}

	resolve_chain();
}

const uint8_t *owner_slot_root(void)
{
	owner_slot_init();

	/* A cleared record at the end of the chain is a factory reset: fall back to
	 * the built-in root and go back to accepting a fresh claim. */
	if ((s_effective != NULL) &&
			((s_effective->flags & OWNER_FLAG_CLEARED) == 0UL)) {
		return s_effective->root_pubkey;
	}
	return fw_public_key;
}

/*
 * Write one record into the first free slot.
 *
 * Body first, header last. A power cut in the middle of five flash words must
 * not leave something that reads as a valid record: writing the header word
 * last means the worst case is a record whose type is still 0xFF, which the
 * scanner rejects and counts as torn. Header first would leave a record
 * announcing itself as valid while its key was still half erased.
 */
static bool append_record(const owner_record_t *rec)
{
	uint8_t *base;
	uint32_t slot = OWNER_SLOT_MAX_RECORDS;
	uint32_t i;

	/* Torn records are stepped over, never reused: rewriting a slot whose body
	 * already holds bits would need an erase, and the only erasable unit here
	 * is the sector the bootloader itself is executing from. */
	for (i = 0U; i < OWNER_SLOT_MAX_RECORDS; i++) {
		if (record_is_erased(record_at(i))) {
			slot = i;
			break;
		}
	}
	if (slot == OWNER_SLOT_MAX_RECORDS) {
		printf("** owner record area is full **\r\n");
		return false;
	}

	base = (uint8_t *)(OWNER_SLOT_BASE + (slot * OWNER_RECORD_SIZE));

	if (Flash_If_Write((uint8_t *)rec + 32, base + 32, OWNER_RECORD_SIZE - 32U) != 0U) {
		printf("** FAILED writing the record body **\r\n");
		return false;
	}
	if (Flash_If_Write((uint8_t *)rec, base, 32U) != 0U) {
		printf("** FAILED writing the record header - the partial record will be "
				"ignored on the next boot **\r\n");
		return false;
	}
	return true;
}

uint32_t owner_slot_generation(void)
{
	owner_slot_init();
	return (s_effective != NULL) ? s_effective->generation : 0U;
}

bool owner_slot_set_owner(uint32_t generation, const uint8_t new_root[64],
		const uint8_t sig[64])
{
	owner_record_t rec;
	uint8_t digest[SHA256_DIGEST_SIZE];
	const uint8_t *current;

	owner_slot_init();

	if (s_effective == NULL) {
		printf("** setowner refused: board is unclaimed - use takeown **\r\n");
		return false;
	}
	if (generation != (s_effective->generation + 1U)) {
		printf("** setowner refused: generation must be %" PRIu32 ", got %" PRIu32
				" **\r\n", s_effective->generation + 1U, generation);
		return false;
	}

	memset(&rec, 0xFF, sizeof(rec));
	rec.type = (uint8_t)OWNER_RECORD_TYPE;
	rec.slots = (uint8_t)OWNER_RECORD_SLOTS;
	rec.format_ver = (uint16_t)OWNER_FORMAT_VER;
	rec.generation = generation;
	rec.flags = 0UL;
	memcpy(rec.root_pubkey, new_root, sizeof(rec.root_pubkey));
	memcpy(rec.prev_sig, sig, sizeof(rec.prev_sig));
	memset(rec.reserved, 0, sizeof(rec.reserved));

	/*
	 * Check before writing, not after. The scanner would reject a bad record on
	 * the next boot anyway, but it would still be sitting in flash consuming
	 * one of 51 slots that can never be reclaimed without erasing the
	 * bootloader. Refusing costs nothing; accepting is permanent.
	 */
	current = s_effective->root_pubkey;
	sha256((const uint8_t *)&rec, OWNER_SIGNED_PREFIX_LEN, digest);
	if (!fw_verify_signature_with_key(current, digest, rec.prev_sig)) {
		printf("** setowner refused: signature does not verify against the "
				"current owner **\r\n");
		return false;
	}

	if (!append_record(&rec)) {
		return false;
	}

	s_scanned = false;
	owner_slot_init();
	if ((s_effective == NULL) || (s_effective->generation != generation)) {
		printf("** setowner wrote a record but it did not take effect - "
				"see the boot log **\r\n");
		return false;
	}

	printf("** Owner changed to generation %" PRIu32 ". Firmware must now be "
			"signed by the new owner. **\r\n", generation);
	return true;
}

bool owner_slot_claim(const uint8_t root_pubkey[64], bool boot0_held)
{
	owner_record_t rec;

	owner_slot_init();

	/* Physical presence is the only thing standing between a factory board and
	 * whoever reaches it first. Refusing without it is the entire gate. */
	if (!boot0_held) {
		printf("** takeown refused: BOOT0 was not held during startup **\r\n");
		return false;
	}
	/* Claimable when nothing is in force, or when the last thing in force is a
	 * factory reset -- that is what a reset is for: the board goes back to
	 * accepting a fresh claim from whoever is standing in front of it. */
	if ((s_effective != NULL) &&
			((s_effective->flags & OWNER_FLAG_CLEARED) == 0UL)) {
		printf("** takeown refused: this board is already claimed. "
				"Changing owner needs the current owner's signature. **\r\n");
		return false;
	}

	memset(&rec, 0xFF, sizeof(rec));
	rec.type = (uint8_t)OWNER_RECORD_TYPE;
	rec.slots = (uint8_t)OWNER_RECORD_SLOTS;
	rec.format_ver = (uint16_t)OWNER_FORMAT_VER;
	/* Past everything already written, so the chain keeps its order after a
	 * reset-then-reclaim. Not always 1. */
	rec.generation = (s_effective != NULL) ? (s_effective->generation + 1U) : 1U;
	rec.flags = 0UL;
	memcpy(rec.root_pubkey, root_pubkey, sizeof(rec.root_pubkey));
	memset(rec.prev_sig, 0, sizeof(rec.prev_sig));   /* nothing to sign with yet */
	memset(rec.reserved, 0, sizeof(rec.reserved));

	if (!append_record(&rec)) {
		return false;
	}

	/* Re-read rather than assume: the scanner is what the next boot will use,
	 * so making it agree now is the only confirmation worth printing. */
	s_scanned = false;
	owner_slot_init();
	if (s_effective == NULL) {
		printf("** takeown wrote a record but it did not take effect - "
				"see the boot log **\r\n");
		return false;
	}

	printf("** Board claimed. It now trusts only firmware signed by that key. **\r\n"
			"** Reset to confirm: the published-root warning should be gone. **\r\n");
	return true;
}

bool owner_slot_factory_reset(bool physically_confirmed)
{
	owner_record_t rec;

	owner_slot_init();

	/* The gesture is the gate. This argument exists so the gate is visible at
	 * the call site rather than being an unstated property of who calls it. */
	if (!physically_confirmed) {
		printf("** factory reset refused: no physical confirmation **\r\n");
		return false;
	}

	/* Nothing in force, or already reset: writing another cleared record would
	 * change nothing and burn one of 51 slots that cannot be reclaimed without
	 * erasing the bootloader. */
	if (s_effective == NULL) {
		printf("** Factory reset: board was already unclaimed, nothing to do **\r\n");
		return true;
	}
	if ((s_effective->flags & OWNER_FLAG_CLEARED) != 0UL) {
		printf("** Factory reset: board was already reset, nothing to do **\r\n");
		return true;
	}

	memset(&rec, 0xFF, sizeof(rec));
	rec.type = (uint8_t)OWNER_RECORD_TYPE;
	rec.slots = (uint8_t)OWNER_RECORD_SLOTS;
	rec.format_ver = (uint16_t)OWNER_FORMAT_VER;
	rec.generation = s_effective->generation + 1U;
	rec.flags = OWNER_FLAG_CLEARED;
	/* No key, and no signature.
	 *
	 * Unsigned is not an oversight. Requiring the current owner's signature
	 * would mean a customer who lost their private key could never use the
	 * board again, and the only remaining route would be ST-Link -- which the
	 * customer typically does not have. docs/design/OWNERSHIP.md takes that trade
	 * deliberately: whoever can physically reach the board can reset it and
	 * take it over. What that buys is that nobody can do it remotely. */
	memset(rec.root_pubkey, 0, sizeof(rec.root_pubkey));
	memset(rec.prev_sig, 0, sizeof(rec.prev_sig));
	memset(rec.reserved, 0, sizeof(rec.reserved));

	if (!append_record(&rec)) {
		return false;
	}

	s_scanned = false;
	owner_slot_init();
	if ((s_effective == NULL) ||
			((s_effective->flags & OWNER_FLAG_CLEARED) == 0UL)) {
		printf("** factory reset wrote a record but it did not take effect - "
				"see the boot log **\r\n");
		return false;
	}

	printf("** FACTORY RESET DONE at generation %" PRIu32 ". Back to the built-in "
			"root; the board can be claimed again. **\r\n", rec.generation);
	return true;
}

bool owner_slot_root_is_public(void)
{
	uint8_t digest[SHA256_DIGEST_SIZE];

	/* Hash whatever root is actually in force, not fw_public_key directly, so
	 * this keeps telling the truth once step 5 lets an owner record take
	 * effect: a board that has been claimed must stop warning. */
	sha256(owner_slot_root(), 64U, digest);

	return memcmp(digest, k_published_root_sha256, sizeof(digest)) == 0;
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

/*
 * The warning that makes the owner slot worth having.
 *
 * Printed when the root in force is the published one -- meaning its private
 * half is in everybody's hands, so anybody can sign firmware this board will
 * accept. That is the honest description of a factory board, and
 * docs/design/OWNERSHIP.md is explicit that it is a consequence of the product being
 * open, not a defect to be hidden.
 *
 * Deliberately NOT keyed on the slot being empty: a customer who built the
 * firmware with their own key has an empty slot and a safe board. Warning them
 * on every boot would train every reader to skip the line, and the one board
 * that genuinely needed the warning would be skipped with it.
 */
static void report_root_trust(void)
{
	if (!owner_slot_root_is_public()) {
		return;
	}
	printf("** This board trusts the PUBLISHED root key: anyone can sign firmware "
			"it will run. **\r\n"
			"** Claim it (see docs/design/OWNERSHIP.md) to bind it to a key of your own. **\r\n");
}

void owner_slot_report(void)
{
	owner_slot_init();

	if (s_empty) {
		printf("Owner slot: empty, using the built-in root key\r\n");
		report_root_trust();
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
	if (s_torn_count > 0U) {
		/* Expected after a power cut during a claim, and harmless: the header
		 * is written last precisely so this is what a torn write looks like. */
		printf("Owner slot: %" PRIu32 " partial record(s) from an interrupted write, "
				"ignored\r\n", s_torn_count);
	}

	if (s_effective != NULL) {
		if ((s_effective->flags & OWNER_FLAG_CLEARED) != 0UL) {
			printf("Owner slot: last record is a factory reset - back to the "
					"built-in root\r\n");
		} else {
			printf("Owner slot: claimed at generation %" PRIu32 " - "
					"firmware must be signed by that owner\r\n",
					s_effective->generation);
		}
	}

	/* Loud on purpose. Records that exist but are not being honoured are a
	 * confusing state, and silence would let it look normal. */
	if (s_unauthorised_count > 0U) {
		/* Not an error the board can fix, and not necessarily an attack either:
		 * a record signed by an owner further down a chain that was broken
		 * earlier lands here too. Either way the board keeps the last root it
		 * could actually authorise, and says how many it would not. */
		printf("** %" PRIu32 " owner record(s) NOT in effect: not signed by the "
				"owner in force at that point in the chain. The board is using "
				"the last root it could verify. **\r\n", s_unauthorised_count);
	}

	report_root_trust();
}

/*
 * owner_slot.c -- see owner_slot.h.
 */

#include "owner_slot.h"
#include "bootloader_state.h"
#include "fw_verify.h"
#include "iap_keyderive.h"
#include "sha256.h"
#include "usbd_cdc_flash.h"
#include <stdio.h>
#include <string.h>
#include <inttypes.h>

/* Both records must be a whole number of 32-byte flash words, and the two
 * segments must tile the area exactly. If a field is ever added or the
 * compiler pads differently, this stops the build rather than letting a board
 * write records the next firmware cannot read. Same reasoning as the metadata
 * record in bootloader_state.h. */
_Static_assert(sizeof(owner_record_t) == OWNER_RECORD_SIZE,
		"owner_record_t must be exactly 160 bytes (5 x 32-byte flash words)");
_Static_assert(sizeof(owner_revoke_rec_t) == OWNER_REVOKE_REC_SIZE,
		"owner_revoke_rec_t must be exactly 32 bytes (1 flash word)");
_Static_assert(OWNER_SEG_O_SIZE + OWNER_SEG_R_SIZE == OWNER_SLOT_SIZE,
		"the 'O' and 'R' segments must tile the 8K area exactly");

static bool     s_scanned;
static bool     s_empty = true;
static uint32_t s_valid_count;
static uint32_t s_ignored_count;
static uint32_t s_torn_count;            /* body written, header never was */
static uint32_t s_unauthorised_count;    /* structurally fine, not entitled to apply */
static const owner_record_t *s_latest;   /* highest generation, structurally valid */
static const owner_record_t *s_effective; /* end of the trusted chain, or NULL */

/* Every structurally valid 'R' record written for this board. Revocation is
 * cumulative, so all of them matter, not just the last. */
static const owner_revoke_rec_t *s_revoke_records[OWNER_REVOKE_MAX_RECORDS];
static uint32_t s_revoke_count;
static uint32_t s_revoke_used;   /* 'R' words that are not erased, valid or not */

/* Too large for the stack; only one reclaim runs at a time. */
static owner_carry_t s_carry;

static const owner_record_t *record_at(uint32_t index)
{
	return (const owner_record_t *)(OWNER_SEG_O_BASE + (index * OWNER_RECORD_SIZE));
}

static const owner_revoke_rec_t *revoke_at(uint32_t index)
{
	return (const owner_revoke_rec_t *)(OWNER_SEG_R_BASE
			+ (index * OWNER_REVOKE_REC_SIZE));
}

/*
 * Structural validity only -- this says nothing about whether the record is
 * *authorised* (prev_sig, checked in resolve_chain()) or *for this board*
 * (uid, also checked in resolve_chain()).
 *
 * A different format_ver is not corruption -- it is a record this firmware is
 * too old (or too new) to understand -- and there is no fallback to an
 * earlier version, so it is counted separately and the boot line says which.
 */
static bool record_is_structurally_valid(const owner_record_t *r)
{
	return (r->type == (uint8_t)OWNER_RECORD_TYPE) &&
			(r->format_ver == (uint16_t)OWNER_FORMAT_VER);
}

static bool revoke_is_structurally_valid(const owner_revoke_rec_t *r)
{
	return (r->type == (uint8_t)OWNER_RECORD_TYPE_REVOKE) &&
			(r->format_ver == (uint16_t)OWNER_FORMAT_VER);
}

static bool bytes_are_erased(const uint8_t *p, uint32_t len)
{
	uint32_t i;

	for (i = 0U; i < len; i++) {
		if (p[i] != 0xFFU) {
			return false;
		}
	}
	return true;
}

static bool record_is_erased(const owner_record_t *r)
{
	return bytes_are_erased((const uint8_t *)r, OWNER_RECORD_SIZE);
}

static bool revoke_is_erased(const owner_revoke_rec_t *r)
{
	return bytes_are_erased((const uint8_t *)r, OWNER_REVOKE_REC_SIZE);
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

static bool effective_is_root(void)
{
	return (s_effective != NULL) && ((s_effective->flags & OWNER_FLAG_CLEARED) == 0UL);
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
 *   first record        may carry no signature -- the initial claim, or the
 *                       one record a reclaim writes back (TOFU)
 *   cleared record      may carry no signature -- factory reset is gated by
 *                       the BOOT0 gesture, and requiring the current owner's
 *                       signature would brick a board whose owner lost the
 *                       key (R3)
 *   after a cleared one may carry no signature -- the board has no root
 *                       again, so this is a first claim
 *   anything else       must be signed by the root currently in force
 *
 * A signed record with no root in force before it has nothing to verify
 * against and stops the walk: no root is compiled in (decision 72).
 *
 * A record that fails stops the walk rather than being skipped. Skipping would
 * let an attacker invalidate one link and have the rest of the chain -- written
 * under a different owner -- silently apply.
 *
 * Every non-cleared record must also carry THIS board's uid, checked before
 * the signature/TOFU branch and applying to both: uid is the only thing
 * standing between "this board's first claim" and "somebody else's first
 * claim, copied here". Cleared records carry uid = 0 and are exempt.
 */
static void resolve_chain(void)
{
	const owner_record_t *current = NULL;
	uint32_t last_gen = 0U;
	uint32_t i;
	bool first = true;
	bool prev_cleared = false;
	uint8_t my_uid[IAP_MACHINE_ID_SIZE];

	iap_keyderive_get_machine_id(my_uid);

	for (;;) {
		const owner_record_t *next = NULL;

		/* Lowest generation strictly above the one just applied. 32 slots, so
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

		if (!cleared && (memcmp(next->uid, my_uid, sizeof(my_uid)) != 0)) {
			/* Not for this board. Same failure bucket as an unauthorised
			 * signature: it stops the walk. */
			s_unauthorised_count++;
			break;
		}

		if (sig_is_absent(next)) {
			if (!first && !cleared && !prev_cleared) {
				s_unauthorised_count++;
				break;
			}
		} else {
			uint8_t digest[SHA256_DIGEST_SIZE];

			if ((current == NULL) || prev_cleared) {
				s_unauthorised_count++;
				break;
			}
			sha256((const uint8_t *)next, OWNER_SIGNED_PREFIX_LEN, digest);
			if (!fw_verify_signature_with_key(current->root_pubkey, digest,
					next->prev_sig)) {
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

	/*
	 * Revocations are a table, not a step in the ownership history: collected
	 * in their own pass, structure and uid only.
	 *
	 * No signature check and no generation here, deliberately (decision 59).
	 * The gate is on the way IN -- owner_slot_revoke() verifies the current
	 * owner's signature before a single byte is written.
	 */
	s_revoke_count = 0U;
	for (i = 0U; i < OWNER_REVOKE_MAX_RECORDS; i++) {
		const owner_revoke_rec_t *r = revoke_at(i);

		if (!revoke_is_structurally_valid(r)) {
			continue;
		}
		if (memcmp(r->uid, my_uid, sizeof(my_uid)) != 0) {
			continue;   /* written for a different board */
		}
		s_revoke_records[s_revoke_count++] = r;
	}
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
	s_revoke_used = 0U;

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
			/* Written body-first, header last (append_record), so "header
			 * still erased, body is not" is an interrupted write, not
			 * corruption. */
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

	for (i = 0U; i < OWNER_REVOKE_MAX_RECORDS; i++) {
		if (!revoke_is_erased(revoke_at(i))) {
			s_empty = false;
			s_revoke_used++;
		}
	}

	resolve_chain();
}

void owner_slot_rescan(void)
{
	s_scanned = false;
	owner_slot_init();
}

const uint8_t *owner_slot_root(void)
{
	owner_slot_init();
	return effective_is_root() ? s_effective->root_pubkey : NULL;
}

static int32_t first_free_record(void)
{
	uint32_t i;

	/* Torn records are stepped over, never reused: rewriting a slot whose body
	 * already holds bits needs an erase, and that only happens in a reclaim. */
	for (i = 0U; i < OWNER_SLOT_MAX_RECORDS; i++) {
		if (record_is_erased(record_at(i))) {
			return (int32_t)i;
		}
	}
	return -1;
}

/* Programs one 'O' record at `base`. Body first, header last: a power cut in
 * the middle of five flash words then leaves a record whose type is still
 * 0xFF, which the scanner rejects as torn. */
static bool program_record(const owner_record_t *rec, uint8_t *base)
{
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

/*
 * Write one 'O' record into the first free slot of the 'O' segment. A full
 * segment is reclaimed first, which leaves one record in it: the compacted
 * record in force. `rec` is built by the caller before this runs and does not
 * depend on the history the reclaim discards.
 */
static bool append_record(const owner_record_t *rec)
{
	int32_t slot = first_free_record();

	if (slot < 0) {
		printf("Owner record area is full - reclaiming sector 15\r\n");
		owner_slot_build_carry(&s_carry);
		if (!bootloader_state_reclaim(&s_carry)) {
			return false;
		}
		owner_slot_rescan();
		slot = first_free_record();
		if (slot < 0) {
			printf("** owner record area still full after the reclaim **\r\n");
			return false;
		}
	}

	return program_record(rec,
			(uint8_t *)(OWNER_SEG_O_BASE + ((uint32_t)slot * OWNER_RECORD_SIZE)));
}

/*
 * Write one 'R' record into the first free slot of the 'R' segment.
 *
 * One flash word, so there is no body/header order to get right: either the
 * word programs or the scanner rejects what is left.
 */
static bool append_revoke_record(const owner_revoke_rec_t *rec)
{
	uint32_t slot = OWNER_REVOKE_MAX_RECORDS;
	uint32_t i;

	for (i = 0U; i < OWNER_REVOKE_MAX_RECORDS; i++) {
		if (revoke_is_erased(revoke_at(i))) {
			slot = i;
			break;
		}
	}
	if (slot == OWNER_REVOKE_MAX_RECORDS) {
		printf("** revocation area is full (%" PRIu32 " records). Only "
				"setowner --wipe empties it. **\r\n",
				(uint32_t)OWNER_REVOKE_MAX_RECORDS);
		return false;
	}

	if (Flash_If_Write((uint8_t *)rec,
			(uint8_t *)(OWNER_SEG_R_BASE + (slot * OWNER_REVOKE_REC_SIZE)),
			OWNER_REVOKE_REC_SIZE) != 0U) {
		printf("** FAILED writing the revocation record **\r\n");
		return false;
	}
	return true;
}

uint32_t owner_slot_generation(void)
{
	owner_slot_init();
	return (s_effective != NULL) ? s_effective->generation : 0U;
}

/*
 * The handover record itself: built and verified, but not written anywhere.
 * Shared by the append path and the wipe path so the two cannot come to
 * different conclusions about what a valid handover looks like.
 *
 * Checked before writing: a record that would not apply must never occupy a
 * slot.
 */
static bool build_handover_record(uint32_t generation, const uint8_t new_root[64],
		const uint8_t sig[64], owner_record_t *rec)
{
	uint8_t digest[SHA256_DIGEST_SIZE];

	owner_slot_init();

	if (!effective_is_root()) {
		printf("** setowner refused: board has no root - the first upload claims it **\r\n");
		return false;
	}
	if (generation != (s_effective->generation + 1U)) {
		printf("** setowner refused: generation must be %" PRIu32 ", got %" PRIu32
				" **\r\n", s_effective->generation + 1U, generation);
		return false;
	}

	memset(rec, 0xFF, sizeof(*rec));
	rec->type = (uint8_t)OWNER_RECORD_TYPE;
	rec->reserved0 = 0U;
	rec->format_ver = (uint16_t)OWNER_FORMAT_VER;
	rec->generation = generation;
	rec->flags = 0UL;
	memcpy(rec->root_pubkey, new_root, sizeof(rec->root_pubkey));
	iap_keyderive_get_machine_id(rec->uid);
	memcpy(rec->prev_sig, sig, sizeof(rec->prev_sig));
	memset(rec->reserved, 0, sizeof(rec->reserved));

	sha256((const uint8_t *)rec, OWNER_SIGNED_PREFIX_LEN, digest);
	if (!fw_verify_signature_with_key(s_effective->root_pubkey, digest,
			rec->prev_sig)) {
		printf("** setowner refused: signature does not verify against the "
				"current owner **\r\n");
		return false;
	}
	return true;
}

bool owner_slot_build_wipe_carry(uint32_t generation, const uint8_t new_root[64],
		const uint8_t sig[64], owner_carry_t *out)
{
	owner_record_t rec;

	if (!build_handover_record(generation, new_root, sig, &rec)) {
		return false;
	}

	/* Unsigned: the wipe discards the links that authorised it, so a stored
	 * signature would have nothing to verify against on the next boot. The
	 * signature was the authorisation to wipe, checked above. */
	memset(rec.prev_sig, 0, sizeof(rec.prev_sig));

	memset(out, 0, sizeof(*out));
	out->has_owner = 1U;
	out->owner = rec;
	out->revoke_count = 0U;

	printf("** owner area will be rewritten with one unsigned record at "
			"generation %" PRIu32 ": %" PRIu32 " owner record(s) and %" PRIu32
			" revocation(s) dropped **\r\n",
			generation, s_valid_count, s_revoke_used);
	return true;
}

bool owner_slot_set_owner(uint32_t generation, const uint8_t new_root[64],
		const uint8_t sig[64])
{
	owner_record_t rec;

	if (!build_handover_record(generation, new_root, sig, &rec)) {
		return false;
	}

	if (!append_record(&rec)) {
		return false;
	}

	owner_slot_rescan();
	if ((s_effective == NULL) || (s_effective->generation != generation)) {
		printf("** setowner wrote a record but it did not take effect - "
				"see the boot log **\r\n");
		return false;
	}

	printf("** Owner changed to generation %" PRIu32 ". Firmware must now be "
			"signed by the new owner. **\r\n", generation);
	return true;
}

bool owner_slot_claim(const uint8_t root_pubkey[64])
{
	owner_record_t rec;

	owner_slot_init();

	/* The only condition: no root in force. That covers a factory board and
	 * one whose last record is a factory reset (decision 72). */
	if (effective_is_root()) {
		printf("** takeown refused: this board already has a root. "
				"Changing owner needs the current owner's signature. **\r\n");
		return false;
	}

	memset(&rec, 0xFF, sizeof(rec));
	rec.type = (uint8_t)OWNER_RECORD_TYPE;
	rec.reserved0 = 0U;
	rec.format_ver = (uint16_t)OWNER_FORMAT_VER;
	/* Past everything already written, so the chain keeps its order after a
	 * reset-then-reclaim. Not always 1. */
	rec.generation = (s_effective != NULL) ? (s_effective->generation + 1U) : 1U;
	rec.flags = 0UL;
	memcpy(rec.root_pubkey, root_pubkey, sizeof(rec.root_pubkey));
	iap_keyderive_get_machine_id(rec.uid);
	memset(rec.prev_sig, 0, sizeof(rec.prev_sig));   /* nothing to sign with yet */
	memset(rec.reserved, 0, sizeof(rec.reserved));

	if (!append_record(&rec)) {
		return false;
	}

	/* Re-read rather than assume: the scanner is what the next boot will use. */
	owner_slot_rescan();
	if (!effective_is_root()) {
		printf("** takeown wrote a record but it did not take effect - "
				"see the boot log **\r\n");
		return false;
	}

	printf("** Board claimed. It now trusts only firmware signed by that key. **\r\n");
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

	/* No root in force: another cleared record would change nothing and burn
	 * a slot. */
	if (!effective_is_root()) {
		printf("** Factory reset: board has no root, nothing to do **\r\n");
		return true;
	}

	memset(&rec, 0xFF, sizeof(rec));
	rec.type = (uint8_t)OWNER_RECORD_TYPE;
	rec.reserved0 = 0U;
	rec.format_ver = (uint16_t)OWNER_FORMAT_VER;
	rec.generation = s_effective->generation + 1U;
	rec.flags = OWNER_FLAG_CLEARED;
	/* No key and no signature: requiring the current owner's signature would
	 * leave a customer who lost their key with a board only ST-Link could
	 * rescue. Whoever can reach the button can reset the board; nobody can do
	 * it remotely (R3). */
	memset(rec.root_pubkey, 0, sizeof(rec.root_pubkey));
	memset(rec.uid, 0, sizeof(rec.uid));   /* cleared record asserts nothing about which board */
	memset(rec.prev_sig, 0, sizeof(rec.prev_sig));
	memset(rec.reserved, 0, sizeof(rec.reserved));

	if (!append_record(&rec)) {
		return false;
	}

	owner_slot_rescan();
	if (effective_is_root()) {
		printf("** factory reset wrote a record but it did not take effect - "
				"see the boot log **\r\n");
		return false;
	}

	printf("** FACTORY RESET DONE at generation %" PRIu32 ". The board has no root; "
			"the next upload claims it. **\r\n", rec.generation);
	return true;
}

bool owner_slot_revoke(const uint8_t leaf_prefix[OWNER_REVOKE_PREFIX_LEN],
		const uint8_t sig[64], bool *already)
{
	owner_revoke_rec_t rec;
	uint8_t digest[SHA256_DIGEST_SIZE];
	uint8_t probe[64];

	owner_slot_init();

	if (already != NULL) {
		*already = false;
	}

	if (!effective_is_root()) {
		printf("** revoke refused: board has no root **\r\n");
		return false;
	}

	/* Idempotent: naming a leaf that is already revoked writes nothing, which
	 * is also what makes a replayed request harmless (decision I-D1). */
	memset(probe, 0, sizeof(probe));
	memcpy(probe, leaf_prefix, OWNER_REVOKE_PREFIX_LEN);
	if (owner_slot_is_revoked(probe)) {
		printf("** already revoked - nothing written **\r\n");
		if (already != NULL) {
			*already = true;
		}
		return true;
	}

	memset(&rec, 0xFF, sizeof(rec));
	rec.type = (uint8_t)OWNER_RECORD_TYPE_REVOKE;
	rec.reserved0 = 0U;
	rec.format_ver = (uint16_t)OWNER_FORMAT_VER;
	iap_keyderive_get_machine_id(rec.uid);
	memcpy(rec.leaf_prefix, leaf_prefix, OWNER_REVOKE_PREFIX_LEN);

	/* The signature covers the whole record as it will be written. */
	sha256((const uint8_t *)&rec, OWNER_REVOKE_SIGNED_LEN, digest);
	if (!fw_verify_signature_with_key(s_effective->root_pubkey, digest, sig)) {
		printf("** revoke refused: signature does not verify against the "
				"current owner **\r\n");
		return false;
	}

	if (!append_revoke_record(&rec)) {
		return false;
	}

	owner_slot_rescan();
	if (!owner_slot_is_revoked(probe)) {
		printf("** revoke wrote a record but it did not take effect - "
				"see the boot log **\r\n");
		return false;
	}

	printf("** Revocation recorded. **\r\n");
	return true;
}

bool owner_slot_is_revoked(const uint8_t leaf_pubkey[64])
{
	const uint8_t *root;
	uint32_t i;

	owner_slot_init();
	root = owner_slot_root();
	if (root == NULL) {
		return false;   /* nothing verifies without a root anyway */
	}

	for (i = 0U; i < s_revoke_count; i++) {
		const uint8_t *entry = s_revoke_records[i]->leaf_prefix;

		/* R4: the root in force can never revoke itself. */
		if (memcmp(entry, root, OWNER_REVOKE_PREFIX_LEN) == 0) {
			continue;
		}
		if (memcmp(entry, leaf_pubkey, OWNER_REVOKE_PREFIX_LEN) == 0) {
			return true;
		}
	}
	return false;
}

void owner_slot_build_carry(owner_carry_t *out)
{
	const uint8_t *root;
	uint32_t i, j;

	owner_slot_init();
	root = owner_slot_root();

	memset(out, 0, sizeof(*out));
	if (root == NULL) {
		printf("** owner area compacted to nothing: the board has no root **\r\n");
		return;
	}

	out->has_owner = 1U;
	out->owner = *s_effective;
	memset(out->owner.prev_sig, 0, sizeof(out->owner.prev_sig));

	for (i = 0U; i < s_revoke_count; i++) {
		const owner_revoke_rec_t *r = s_revoke_records[i];
		bool duplicate = false;

		/* R4 ignores these on every boot; carrying one forward spends a slot. */
		if (memcmp(r->leaf_prefix, root, OWNER_REVOKE_PREFIX_LEN) == 0) {
			continue;
		}
		for (j = 0U; j < out->revoke_count; j++) {
			if (memcmp(out->revoke[j].leaf_prefix, r->leaf_prefix,
					OWNER_REVOKE_PREFIX_LEN) == 0) {
				duplicate = true;
				break;
			}
		}
		if (!duplicate) {
			out->revoke[out->revoke_count++] = *r;
		}
	}

	printf("** owner area compacted: 1 of %" PRIu32 " owner record(s) and %" PRIu32
			" of %" PRIu32 " revocation(s) kept **\r\n",
			s_valid_count, out->revoke_count, s_revoke_used);
}

bool owner_slot_write_carry(const owner_carry_t *carry)
{
	uint32_t i;

	if (carry->revoke_count > OWNER_REVOKE_MAX_RECORDS) {
		return false;
	}
	for (i = 0U; i < carry->revoke_count; i++) {
		if (Flash_If_Write((uint8_t *)&carry->revoke[i],
				(uint8_t *)(OWNER_SEG_R_BASE + (i * OWNER_REVOKE_REC_SIZE)),
				OWNER_REVOKE_REC_SIZE) != 0U) {
			printf("** FAILED writing a revocation back **\r\n");
			return false;
		}
	}
	if (carry->has_owner != 0U) {
		return program_record(&carry->owner, (uint8_t *)OWNER_SEG_O_BASE);
	}
	return true;
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
		printf("Owner slot: empty - no root, the next upload claims this board\r\n");
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
		printf("Owner slot: %" PRIu32 " partial record(s) from an interrupted write, "
				"ignored\r\n", s_torn_count);
	}

	if (effective_is_root()) {
		printf("Owner slot: claimed at generation %" PRIu32 " - "
				"firmware must be signed by that owner\r\n",
				s_effective->generation);
	} else if (s_effective != NULL) {
		printf("Owner slot: last record is a factory reset - no root, the next "
				"upload claims this board\r\n");
	} else {
		printf("Owner slot: no valid record - no root, the next upload claims "
				"this board\r\n");
	}

	/* Loud on purpose: records that exist but are not honoured would otherwise
	 * look normal. */
	if (s_unauthorised_count > 0U) {
		printf("** %" PRIu32 " owner record(s) NOT in effect: not signed by the "
				"owner in force at that point in the chain. The board is using "
				"the last root it could verify. **\r\n", s_unauthorised_count);
	}

	{
		uint32_t o_used = s_valid_count + s_ignored_count + s_torn_count;
		uint32_t o_free = (o_used < OWNER_SLOT_MAX_RECORDS)
				? (OWNER_SLOT_MAX_RECORDS - o_used) : 0U;
		uint32_t r_free = (s_revoke_used < OWNER_REVOKE_MAX_RECORDS)
				? (OWNER_REVOKE_MAX_RECORDS - s_revoke_used) : 0U;
		uint32_t revoked_names = 0U;
		uint32_t ri;
		bool self_named = false;
		const uint8_t *root = owner_slot_root();

		for (ri = 0U; ri < s_revoke_count; ri++) {
			if ((root != NULL) && (memcmp(s_revoke_records[ri]->leaf_prefix, root,
					OWNER_REVOKE_PREFIX_LEN) == 0)) {
				self_named = true;
				continue;
			}
			revoked_names++;
		}

		printf("Owner slot: %" PRIu32 "/%" PRIu32 " owner slot(s) free, "
				"%" PRIu32 "/%" PRIu32 " revoke slot(s) free, "
				"%" PRIu32 " leaf(s) revoked\r\n",
				o_free, (uint32_t)OWNER_SLOT_MAX_RECORDS,
				r_free, (uint32_t)OWNER_REVOKE_MAX_RECORDS,
				revoked_names);

		/* Owner slots reclaim themselves; revocation slots only come back
		 * through setowner --wipe, so warn early enough to act. */
		if (r_free <= OWNER_REVOKE_LOW_WATER) {
			printf("** Only %" PRIu32 " revocation slot(s) left. Changing the "
					"root with setowner --wipe retires every leaf that root "
					"issued and empties these slots. **\r\n", r_free);
		}

		if (self_named) {
			printf("** A revocation names the root currently in force - ignored "
					"(a root can never revoke itself, R4). **\r\n");
		}
	}
}

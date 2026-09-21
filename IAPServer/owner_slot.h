/*
 * owner_slot.h -- which public key this board trusts as its signing root.
 *
 * Requirement C10. Design: $PROD/docs/modules/M2-ownership.md.
 *
 * An append-only record area in the top 8K of the bootloader's own flash
 * sector, reserved by STM32H743IIKX_FLASH.ld (FLASH LENGTH is 120K, not the
 * 128K of the sector, so the linker cannot place anything here). Empty means
 * the board falls back to the root compiled into fw_pubkey.c.
 *
 * Why this area and not the state sector: a reclaim there erases the whole
 * sector. Copying owner records out and back would open a window the width of
 * an erase, and losing them there would silently return the board to the
 * factory root -- the one failure this must not have.
 *
 * Fully implemented: init/claim/set_owner/factory_reset all write real
 * records, and owner_slot_root() walks the verified chain (resolve_chain() in
 * the .c file) rather than trusting the compiled-in root unconditionally.
 * $PROD/docs/modules/M2-ownership.md is the design source; this header only states the
 * on-flash layout.
 *
 * v3 (2026-09-20) adds a second record type, 'R' (revoke), living in the same
 * append-only area and the same generation sequence as 'O' records -- see
 * OWNER_RECORD_TYPE_REVOKE below. This is a hard cut, not a migration: `slots`
 * is gone (nothing ever used it; variable-length records were never built),
 * so a v2 record is structurally invalid to v3 firmware and vice versa.
 */

#ifndef IAPSERVER_OWNER_SLOT_H_
#define IAPSERVER_OWNER_SLOT_H_

#include <stdint.h>
#include <stdbool.h>

/* Top 8K of the bootloader sector. Must agree with FLASH LENGTH in
 * STM32H743IIKX_FLASH.ld: that script grants the linker 0x08000000..0x0801DFFF
 * and this area starts where it stops. */
#define OWNER_SLOT_BASE        0x0801E000UL
#define OWNER_SLOT_SIZE        (8U * 1024U)

/* 160 bytes = 5 x 32-byte flash words. The H7 programs a 256-bit word at a
 * time, so a record that is not a whole number of them cannot be appended
 * without a read-modify-write of a neighbour. */
#define OWNER_RECORD_SIZE      160U
#define OWNER_SLOT_MAX_RECORDS (OWNER_SLOT_SIZE / OWNER_RECORD_SIZE)   /* 51 */

#define OWNER_RECORD_TYPE         'O'
#define OWNER_RECORD_TYPE_REVOKE  'R'
#define OWNER_RECORD_ERASED       0xFFU
#define OWNER_FORMAT_VER          3U

/* flags (meaningful for 'O' records only; 'R' always writes 0) */
#define OWNER_FLAG_CLEARED     0x00000001UL   /* factory reset: fall back to R0 */

/* An 'R' record names up to this many revoked leaves. Revocation today has one
 * caller (owner_slot_revoke()) that fills one slot per call; the other three
 * exist so a future caller can batch without a format change. Unused slots are
 * all-zero and skipped by owner_slot_is_revoked(). */
#define OWNER_REVOKE_SLOTS       4U
/* A leaf is named by the first 16 bytes of its 64-byte public key -- not the
 * whole key. Full P-256 points are effectively random, so 128 bits of prefix
 * make a collision (revoking the wrong leaf, or failing to revoke the right
 * one) astronomically unlikely, and it lets one record hold four names instead
 * of one. See $PROD/maps/owner-revoke-and-boot-upgrade/issues/OWN-01-revoke-by-serial-or-by-pubkey.md. */
#define OWNER_REVOKE_PREFIX_LEN  16U

/*
 * Layout is fixed by $PROD/docs/modules/M2-ownership.md and locked by a _Static_assert in the
 * .c file. format_ver exists from the first version on purpose, so a later
 * format change is an upgrade rather than a breaking migration.
 *
 * v2 (2026-09-04): added `uid`, binding a record to the one board it was
 * issued for -- without it, the raw bytes of one board's record area could be
 * copied onto another board's and verify just as well, since nothing in the
 * signed prefix said which board it was for. `uid` sits between the union
 * below and `prev_sig`, inside OWNER_SIGNED_PREFIX_LEN, so it is covered by
 * the same signature as everything else -- a field the signature does not
 * cover is not actually bound to anything.
 *
 * v3 (2026-09-20): dropped `slots` (nothing ever read it -- variable-length
 * records were never built) in favour of a `reserved0` byte that keeps every
 * other field at the same offset, and gave the payload two interpretations:
 * `root_pubkey` for an 'O' record, `revoked` for an 'R' one. Only one is ever
 * live in a given record, decided by `type`; the union just means an 'R'
 * record does not need a second struct to hold the same 64 bytes.
 *
 * No compatibility with earlier versions: nothing in the field carries an
 * older record forward, so OWNER_FORMAT_VER is a hard cut, not a migration.
 * record_is_structurally_valid() rejects format_ver != 3 outright.
 */
typedef struct {
	uint8_t  type;             /*   0  'O', 'R', or 0xFF when erased           */
	uint8_t  reserved0;        /*   1  was `slots`; always 0 now                */
	uint16_t format_ver;       /*   2  3                                        */
	uint32_t generation;       /*   4  monotonic; walked oldest-first, see below */
	uint32_t flags;            /*   8  bit0 = cleared ('O' only)                */
	union {
		uint8_t root_pubkey[64];                             /* 'O' */
		uint8_t revoked[OWNER_REVOKE_SLOTS][OWNER_REVOKE_PREFIX_LEN]; /* 'R' */
	};                         /*  12  64 bytes either way; all zero when cleared */
	uint8_t  uid[12];          /*  76  this board's HAL_GetUIDw0/1/2(), big-endian
	                            *      UIDW2||UIDW1||UIDW0. Zero for a cleared
	                            *      record (there is no "this board" claim
	                            *      left once cleared).                     */
	uint8_t  prev_sig[64];     /*  88  previous root's signature over bytes 0..87
	                            *      all zero for the first claim and for a
	                            *      cleared 'O' record -- those are gated by a
	                            *      physical action, not by a signature. An
	                            *      'R' record is never exempt (see below)    */
	uint8_t  reserved[8];      /* 152                                          */
} owner_record_t;

/* Scan the area. Read-only; safe to call before anything else is up. */
void owner_slot_init(void);

/*
 * Append a record claiming this board for `root_pubkey`.
 *
 * Gated on BOOT0 having been held through this boot's startup window -- the
 * caller passes that in rather than reading the pin again, because by the time
 * a command arrives the operator has long since let go.
 *
 * ⚠️ The first claim carries no signature and cannot: there is no owner yet to
 * sign it. It is trust-on-first-use, gated by physical presence, and whoever
 * gets there first wins. $PROD/docs/modules/M2-ownership.md states this plainly -- a factory
 * board's security ceiling is "anyone who can press the button", and that only
 * closes once the board is claimed.
 *
 * Returns false and changes nothing if the board is already claimed, if BOOT0
 * was not held, or if the write fails.
 */
bool owner_slot_claim(const uint8_t root_pubkey[64], bool boot0_held);

/* The bytes a change-of-owner signature covers: everything in the record
 * before prev_sig itself -- type, slots, format_ver, generation, flags, the
 * incoming public key, and (v2) the board's own uid. Signing the generation is
 * what stops a captured record from being replayed into a later slot; signing
 * uid is what stops the whole record being replayed onto a different board. */
#define OWNER_SIGNED_PREFIX_LEN 88U

/*
 * Append a record handing the board to `new_root`.
 *
 * `sig` must be the CURRENT owner's signature over SHA-256 of the first
 * OWNER_SIGNED_PREFIX_LEN bytes of the record being written, and `generation`
 * must be exactly one past the record in force -- both sides therefore agree
 * bit for bit on what was signed.
 *
 * No physical gate: the signature IS the authorisation, and changing owner
 * remotely is a case the design means to support. That is also why this is a
 * different entry point from owner_slot_claim(), which has no signature to
 * check and so can only be gated by presence.
 *
 * Verified before anything is written -- a record that would not apply should
 * never reach the flash.
 */
bool owner_slot_set_owner(uint32_t generation, const uint8_t new_root[64],
		const uint8_t sig[64]);

/* Generation of the record currently in force, 0 when the board is unclaimed.
 * The host needs it to build the next record's signed prefix. */
uint32_t owner_slot_generation(void);

/*
 * Append a cleared record: the board goes back to the built-in root and can be
 * claimed again.
 *
 * Carries no signature, and that is the trade $PROD/docs/modules/M2-ownership.md makes on
 * purpose. Requiring the current owner's signature would leave a customer who
 * lost their private key with a board only ST-Link could rescue -- and the
 * customer is exactly who does not have one. The cost is that anybody who can
 * physically reach the board can reset it and take it over; what it buys is
 * that nobody can do it remotely, which is the attack that matters (R3).
 *
 * `physically_confirmed` is the caller asserting the operator performed the
 * gesture. Passing it makes the gate visible here rather than being an
 * unstated property of the call site.
 */
bool owner_slot_factory_reset(bool physically_confirmed);

/*
 * Append a record revoking up to OWNER_REVOKE_SLOTS leaves. `revoked` is
 * exactly the 64 bytes the record will carry -- an unused slot must be all
 * zero, and the caller decides how many of the four are filled. Like
 * owner_slot_set_owner(), `generation` must be exactly one past the record in
 * force and `sig` must verify over the signed prefix of the record as it will
 * actually be written: the caller has to know the board's uid in advance to
 * produce that signature, the same requirement set_owner already has.
 *
 * No physical gate, for the same reason set_owner has none: the signature IS
 * the authorisation, and a departed colleague's key needs revoking without
 * anyone standing at the board.
 *
 * There is no unsigned case for 'R', unlike 'O': revocation has no equivalent
 * of "the first claim, TOFU" -- a board with nothing to revoke from has no
 * business writing a revocation, and resolve_chain() enforces that requiring
 * a signature.
 */
bool owner_slot_revoke(const uint8_t revoked[OWNER_REVOKE_SLOTS][OWNER_REVOKE_PREFIX_LEN],
		const uint8_t sig[64], bool *already);

/*
 * Has this leaf been revoked by an 'R' record written for this board?
 * Compares only the first OWNER_REVOKE_PREFIX_LEN bytes of `leaf_pubkey`, the
 * same slice a revocation names.
 *
 * R4: the root currently in force can never revoke itself -- an entry that
 * happens to equal owner_slot_root()'s own prefix is skipped, not honoured.
 * This is deliberately a per-entry skip, not a reason to reject the whole
 * record: one record holds four names, and a self-naming mistake (or a
 * captured record replayed after the board changed hands) must not cost the
 * other three their effect.
 *
 * Not something callers need to remember on their own -- iap_cert_verify()
 * and iap_cert_verify_image() (iap_cert.h) both take the answer as a
 * parameter, so a call site that forgets to ask does not compile.
 */
bool owner_slot_is_revoked(const uint8_t leaf_pubkey[64]);

/*
 * The root to verify firmware against: the last verified link in the on-flash
 * chain (resolve_chain(), .c file), or the compiled-in fw_public_key when the
 * area is empty or the chain resolves to nothing valid.
 */
const uint8_t *owner_slot_root(void);

/* True when the area holds no record at all (a factory board). */
bool owner_slot_is_empty(void);

/* Number of structurally valid records found. */
uint32_t owner_slot_record_count(void);

/* One boot line describing what was found. */
void owner_slot_report(void);

/*
 * True when the root this board verifies firmware against is the one published
 * with the project -- the key whose PRIVATE half is in the repository, because
 * customers have to be able to sign their own sketches.
 *
 * ⚠️ The question is NOT "is the owner slot empty". A customer who compiled the
 * firmware with their own key has an empty slot and a perfectly safe board;
 * warning them every boot would teach everyone to ignore the line, and then it
 * protects nobody. See $PROD/docs/modules/M2-ownership.md.
 */
bool owner_slot_root_is_public(void);

#endif /* IAPSERVER_OWNER_SLOT_H_ */

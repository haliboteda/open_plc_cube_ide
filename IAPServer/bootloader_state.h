/*
 * bootloader_state.h
 *
 * Bootloader-owned Flash state in sector 15 (IAP_STATE_SECTOR_ADDR): the
 * calibration area, the root area (owner_slot.h), append-only firmware
 * metadata and a completion marker. App updates never touch it.
 *
 * Nothing is erased except by a reclaim, and a reclaim stages what it must
 * not lose in backup SRAM first (bkp_stash.h). Layout, the three reclaim
 * triggers and what a power cut does at each step:
 * $PROD/docs/modules/M1/SECTOR-15.md.
 */

#ifndef IAPSERVER_BOOTLOADER_STATE_H_
#define IAPSERVER_BOOTLOADER_STATE_H_

#include <stdint.h>
#include <stdbool.h>
#include "iap_cert.h"
#include "owner_slot.h"

#ifdef __cplusplus
extern "C" {
#endif

/* One STM32H7 flash word: the smallest thing that can be programmed, and a word
 * may only be programmed once. One metadata record needs seven of them (the
 * signature and certificate alone are 192 bytes). */
#define IAP_META_SLOT_SIZE 32U
#define IAP_METADATA_SLOTS    7U

/*
 * Firmware metadata payload: 4+64+128+20 = 216 bytes, which with the 8-byte
 * record header fills IAP_METADATA_SLOTS slots exactly (224 = 7x32).
 *
 * `cert` is the certificate whose leaf key produced `signature` -- stored
 * whole, not just the leaf pubkey, because re-verification at every boot
 * (server_decide()) has to re-check root_sig too. That is what makes a
 * setowner handover -- or a revocation -- retroactively invalidate the
 * currently-installed firmware: the stored cert's root_sig only verifies
 * against the root that signed it, owner_slot_root() changes the moment
 * ownership changes, and owner_slot_is_revoked() is checked fresh every time
 * against the leaf this cert names. A cached "last known good leaf_pubkey"
 * would silently defeat both -- do not add one.
 *
 * 2026-09-20: dropped `sha256` (never read anywhere -- the signature already
 * binds the hash, so a stored copy of the hash added no security and nothing
 * ever compared it) and `cert` shrank from 132 to 128 bytes when its `serial`
 * field went away (see iap_cert.h). One update costs 7 slots, so 3583/7 = 511
 * updates before a reclaim.
 */
typedef struct {
	uint32_t   app_size;
	uint8_t    signature[64];   /* by the cert's leaf key, not necessarily the root */
	iap_cert_t cert;
	uint8_t    reserved[20];
} iap_fw_metadata_t;

/* Idempotent. Finishes a reclaim a power cut interrupted, marks a factory
 * sector, then scans the metadata. */
void bootloader_state_init(void);

/* True if sha256_selftest() passed during bootloader_state_init(). If this
 * is false, no verification result the crypto layer produces can be
 * trusted -- callers should treat every signature/HMAC check as failed. */
bool bootloader_state_crypto_selftest_passed(void);

/* Latest firmware metadata. Returns false if none exists yet (fresh/blank
 * device that has never been through an authenticated update). */
bool bootloader_state_get_metadata(iap_fw_metadata_t *out);

/* Appends a new metadata record after a successful, signature-verified
 * update. A full area is reclaimed with the new record as its metadata.
 * `cert` is stored whole (see the comment on iap_fw_metadata_t above) -- it is the certificate
 * whose leaf key produced `signature`.
 *
 * No hash parameter: `iap_fw_metadata_t` used to store one, but nothing ever
 * read it back (the signature already binds the hash it was computed over,
 * so a second stored copy checked against nothing was dead weight). Removed
 * 2026-09-20 along with the field. */
void bootloader_state_save_metadata(uint32_t app_size, const uint8_t signature[64],
                                     const iap_cert_t *cert);

/*
 * Rewrite sector 15 with `carry` as the root area, keeping calibration and the
 * latest metadata record (SECTOR-15.md, steps 1-6). For a full 'O' segment
 * and for `setowner --wipe`. Returns false if a write failed; the backup copy
 * then stays and the next boot finishes the job.
 */
bool bootloader_state_reclaim(const owner_carry_t *carry);

/* Logs a rejected authentication attempt with a count since boot. Log only,
 * never Flash: an unauthenticated caller must not be able to reach Flash at
 * all, or a flood of rejected commands would wear the state sector. */
void bootloader_state_note_auth_fail(uint32_t peer_ip);

/* SHA-256 over `size` bytes of the (memory-mapped) app region starting at
 * app_base, read directly from Flash -- no RAM staging of the whole image. */
void bootloader_state_hash_app(uint32_t app_base, uint32_t size, uint8_t out_sha256[32]);

/* Records the outcome of server_decide()'s boot-time signature check so
 * other modules (e.g. the UDP discovery reply) can report it without
 * recomputing the hash/signature check themselves. Defaults to false. */
void bootloader_state_set_app_valid(bool valid);
bool bootloader_state_app_is_valid(void);

#ifdef __cplusplus
}
#endif

#endif /* IAPSERVER_BOOTLOADER_STATE_H_ */

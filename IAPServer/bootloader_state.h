/*
 * bootloader_state.h
 *
 * Bootloader-owned Flash state: firmware metadata (size/hash/signature) and
 * a tamper-chained event log, both living in the reserved tail sector
 * (IAP_STATE_SECTOR_ADDR, see usbd_cdc_flash.h) that app updates can never
 * touch.
 *
 * Physical layout: the whole 128K sector is one append-only journal of 32-byte
 * slots (4096 of them). Records are never overwritten in place -- every
 * successful update *appends* fresh metadata, and the "current" firmware is
 * simply the most recent metadata record. NOR Flash forces this: the smallest
 * erase is the whole sector, so overwriting metadata in place would wipe the
 * event log along with it on every single update.
 *
 * When the journal fills up it is NOT erased on the spot. Erasing takes
 * hundreds of milliseconds, and losing power inside that window would take the
 * current metadata with it -- turning a healthy board into one that no longer
 * trusts its own application. Instead:
 *
 *   - a full journal simply stops accepting log entries (they are counted, and
 *     the state is reported at boot and in the identity string) -- history is
 *     kept, nothing is silently thrown away;
 *   - the erase happens only inside bootloader_state_save_metadata(), i.e. at
 *     the one moment the old metadata has already been made worthless by the
 *     update that just overwrote the application it described. Losing power
 *     there leaves the board needing a re-upload, which was already true from
 *     the moment the application region was erased. The reclaim therefore adds
 *     no failure mode that the update itself did not already have.
 */

#ifndef IAPSERVER_BOOTLOADER_STATE_H_
#define IAPSERVER_BOOTLOADER_STATE_H_

#include <stdint.h>
#include <stdbool.h>

#ifdef __cplusplus
extern "C" {
#endif

/* One STM32H7 flash word: the smallest thing that can be programmed, and a word
 * may only be programmed once. A log entry is exactly one of these, so the
 * journal holds as many events as the hardware can possibly fit. Metadata needs
 * four (the SHA-256 and the signature alone are 96 bytes). */
#define IAP_JOURNAL_SLOT_SIZE 32U
#define IAP_METADATA_SLOTS    4U

/* Firmware metadata payload: 4+4+32+64+16 = 120 bytes, which with the 8-byte
 * record header fills IAP_METADATA_SLOTS slots exactly. */
typedef struct {
	uint32_t app_size;
	uint32_t fw_version;
	uint8_t  sha256[32];
	uint8_t  signature[64];
	uint8_t  reserved[16];
} iap_fw_metadata_t;

/* The log record itself is iap_log_rec_t in bootloader_state.c. */
typedef enum {
	IAP_EVT_UPDATE_OK          = 1,
	IAP_EVT_CRC_FAIL           = 2,
	IAP_EVT_SIG_FAIL           = 3,
	IAP_EVT_AUTH_FAIL          = 4,
	IAP_EVT_OVERFLOW_ABORT     = 5,
	IAP_EVT_BOOT_VERIFY_FAIL   = 6,
	IAP_EVT_JOURNAL_RECLAIMED  = 7,
	/* Image verified, but copying it out of the staging buffer into the app
	 * region failed. Distinct from SIG_FAIL on purpose: the image was good, the
	 * flash write was not, and the two need completely different diagnosis. */
	IAP_EVT_FLASH_WRITE_FAIL   = 8
} bootloader_event_type_t;

void bootloader_state_init(void);

/* True if sha256_selftest() passed during bootloader_state_init(). If this
 * is false, no verification result the crypto layer produces can be
 * trusted -- callers should treat every signature/HMAC check as failed. */
bool bootloader_state_crypto_selftest_passed(void);

/* Latest firmware metadata. Returns false if none exists yet (fresh/blank
 * device that has never been through an authenticated update). */
bool bootloader_state_get_metadata(iap_fw_metadata_t *out);

/* Appends a new metadata record after a successful, signature-verified
 * update. Does not erase/overwrite the previous record. */
void bootloader_state_save_metadata(uint32_t app_size, uint32_t fw_version,
                                     const uint8_t sha256[32], const uint8_t signature[64]);

/* Appends a tamper-chained log entry. Each entry's stored hash covers the
 * previous entry's full raw bytes, so deleting/altering a past entry breaks
 * the chain for everything after it.
 *
 * tick_ms: HAL_GetTick() at the time of the event -- relative to this boot
 * only (there is no synchronized wall-clock source in the bootloader), but
 * still useful to see how far apart repeated events in one session were.
 * auth_counter: iap_auth_get_counter() at the time of the event -- ties the
 * entry to a specific challenge/response attempt, not just "an auth
 * failure happened at some point". Pass 0 for events with no meaningful
 * association (e.g. none issued yet). */
void bootloader_state_log_event(bootloader_event_type_t event, uint32_t method, uint32_t peer_ip,
                                 uint32_t tick_ms, uint32_t auth_counter);

/* Event of the most recent log record, or 0 when the journal holds none. Lets a
 * caller skip appending a repeat of an event that would otherwise be written on
 * every single boot. */
uint32_t bootloader_state_last_log_event(void);

/* True once the journal has no room left for further log entries. Cleared by
 * the next successful update, which reclaims the sector. */
bool bootloader_state_journal_full(void);

/* How many log entries have been dropped because the journal was full. */
uint32_t bootloader_state_dropped_events(void);

#define IAP_AUTH_FAIL_LOG_SIZE 32U

typedef struct {
	uint32_t method;
	uint32_t peer_ip;
	uint32_t tick_ms;
} iap_auth_fail_entry_t;

/* Records a rejected authentication attempt. Deliberately RAM-only: an
 * unauthenticated caller must not be able to reach Flash at all, or a flood of
 * rejected commands would wear the state sector and force reclaims. Lost on
 * power-off, which matches how comparable devices treat this log. */
void bootloader_state_note_auth_fail(uint32_t method, uint32_t peer_ip, uint32_t tick_ms);

/* Total rejections since boot; may exceed IAP_AUTH_FAIL_LOG_SIZE. */
uint32_t bootloader_state_auth_fail_count(void);

/* Most recent rejections, oldest first. Writes the entry count to out_count. */
const iap_auth_fail_entry_t *bootloader_state_auth_fail_log(uint32_t *out_count);

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

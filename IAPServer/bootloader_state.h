/*
 * bootloader_state.h
 *
 * Bootloader-owned Flash state: firmware metadata (size/hash/signature) and
 * a tamper-chained event log, both living in the reserved tail sector
 * (IAP_STATE_SECTOR_ADDR, see usbd_cdc_flash.h) that app updates can never
 * touch.
 *
 * Physical layout: the whole 128K sector is one append-only journal of
 * fixed-size 128-byte slots. Metadata is never overwritten in place -- every
 * successful update *appends* a fresh metadata record, and the "current"
 * firmware is simply the most recent metadata record in the journal. This
 * is required by how NOR Flash actually works: the minimum erase unit is
 * the whole sector, so if metadata and log shared one sector but metadata
 * were overwritten in place, every single firmware update would force an
 * erase that wipes the entire event log too. Append-only avoids that: erase
 * (a "reclaim") only happens when the journal fills up (~1000+ records),
 * and a reclaim preserves the current metadata by re-appending it first.
 */

#ifndef IAPSERVER_BOOTLOADER_STATE_H_
#define IAPSERVER_BOOTLOADER_STATE_H_

#include <stdint.h>
#include <stdbool.h>

#ifdef __cplusplus
extern "C" {
#endif

#define IAP_JOURNAL_SLOT_SIZE 128U

/* Firmware metadata payload (type == IAP_RECORD_METADATA).
 * app_size + sha256 + signature + fw_version + reserved = 4+4+32+64+16 = 120
 * bytes, exactly filling a journal slot's payload. */
typedef struct {
	uint32_t app_size;
	uint32_t fw_version;
	uint8_t  sha256[32];
	uint8_t  signature[64];
	uint8_t  reserved[16];
} iap_fw_metadata_t;

/* Note: iap_log_payload_t (bootloader_state.c) carries tick_ms and
 * auth_counter alongside event/method/peer_ip/prev_hash, taken out of what
 * used to be pure reserved padding -- total payload size is unchanged. */
typedef enum {
	IAP_EVT_UPDATE_OK          = 1,
	IAP_EVT_CRC_FAIL           = 2,
	IAP_EVT_SIG_FAIL           = 3,
	IAP_EVT_AUTH_FAIL          = 4,
	IAP_EVT_OVERFLOW_ABORT     = 5,
	IAP_EVT_BOOT_VERIFY_FAIL   = 6,
	IAP_EVT_JOURNAL_RECLAIMED  = 7
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

/* SHA-256 over `size` bytes of the (memory-mapped) app region starting at
 * app_base, read directly from Flash -- no RAM staging of the whole image. */
void bootloader_state_hash_app(uint32_t app_base, uint32_t size, uint8_t out_sha256[32]);

/* Records the outcome of server_init()'s boot-time signature check so
 * other modules (e.g. the UDP discovery reply) can report it without
 * recomputing the hash/signature check themselves. Defaults to false. */
void bootloader_state_set_app_valid(bool valid);
bool bootloader_state_app_is_valid(void);

#ifdef __cplusplus
}
#endif

#endif /* IAPSERVER_BOOTLOADER_STATE_H_ */

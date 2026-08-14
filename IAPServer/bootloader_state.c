/*
 * bootloader_state.c
 *
 * See bootloader_state.h for the on-Flash layout rationale (append-only
 * journal shared by firmware metadata and the tamper-chained event log).
 */

#include "bootloader_state.h"
#include "usbd_cdc_flash.h"
#include "sha256.h"
#include <string.h>
#include <stdio.h>
#include <inttypes.h>

extern uint16_t Flash_If_Write(uint8_t *DataAddress, uint8_t *FlashAddress, uint32_t Len);
extern uint16_t Flash_If_Erase(uint32_t Add, uint32_t NbSectors);

#define IAP_JOURNAL_BASE        IAP_STATE_SECTOR_ADDR
#define IAP_JOURNAL_REGION_SIZE (128U * 1024U)
#define IAP_JOURNAL_SLOT_COUNT  (IAP_JOURNAL_REGION_SIZE / IAP_JOURNAL_SLOT_SIZE)

/* Erased Flash reads as 0xFF, so that is "no record here". The others are
 * one byte each: a record header must fit alongside its payload in a single
 * 32-byte flash word. */
#define IAP_REC_BLANK    0xFFU
#define IAP_REC_METADATA 0x4DU /* 'M' */
#define IAP_REC_LOG      0x4CU /* 'L' */

/* Exactly one flash word. peer_ip/tick_ms/auth_counter say who and when;
 * detail carries whatever the event needs (the reclaim record puts the number
 * of discarded slots there). prev_hash is the first 12 bytes of SHA-256 over
 * the previous log record's raw 32 bytes -- truncated because 96 bits is more
 * than enough to make an edited or deleted entry detectable, and the space
 * buys the rest of the fields. */
typedef struct {
	uint8_t  type;      /* IAP_REC_LOG */
	uint8_t  slots;     /* 1 */
	uint8_t  event;
	uint8_t  method;
	uint32_t peer_ip;
	uint32_t tick_ms;
	uint32_t auth_counter;
	uint32_t detail;
	uint8_t  prev_hash[12];
} iap_log_rec_t;

/* Four flash words: 8-byte header + the 120-byte payload. */
typedef struct {
	uint8_t  type;      /* IAP_REC_METADATA */
	uint8_t  slots;     /* IAP_METADATA_SLOTS */
	uint16_t reserved0;
	uint32_t reserved1;
	iap_fw_metadata_t meta;
} iap_meta_rec_t;

/* These sizes are the on-Flash format. A stray padding byte would shift every
 * field of every record already written, so fail the build instead. */
_Static_assert(sizeof(iap_log_rec_t) == IAP_JOURNAL_SLOT_SIZE,
		"log record must be exactly one flash word");
_Static_assert(sizeof(iap_meta_rec_t) == (IAP_METADATA_SLOTS * IAP_JOURNAL_SLOT_SIZE),
		"metadata record must fill its slots exactly");

static uint32_t s_next_free_slot;
static uint32_t s_last_metadata_slot;
static uint32_t s_dropped_events;
static bool     s_format_unknown;
static uint8_t  s_last_log_hash[12];
static bool     s_have_log_hash;
static uint32_t s_last_log_event;   /* 0 = no log record in the journal yet */
static bool     s_crypto_selftest_ok;
static bool     s_app_valid;

static iap_auth_fail_entry_t s_auth_fail[IAP_AUTH_FAIL_LOG_SIZE];
static uint32_t s_auth_fail_total;

static const void *slot_ptr(uint32_t index)
{
	return (const void *)(IAP_JOURNAL_BASE + index * IAP_JOURNAL_SLOT_SIZE);
}

static uint8_t slot_type(uint32_t index)
{
	return *(const volatile uint8_t *)slot_ptr(index);
}

static uint8_t slot_count_of(uint32_t index)
{
	return *((const volatile uint8_t *)slot_ptr(index) + 1U);
}

/* Slots still free. Records are appended, never split. */
static uint32_t journal_room(void)
{
	return (s_next_free_slot >= IAP_JOURNAL_SLOT_COUNT)
			? 0U : (IAP_JOURNAL_SLOT_COUNT - s_next_free_slot);
}

static void journal_write(const void *record, uint32_t slots);
static void journal_reclaim(void);
static void journal_log(uint8_t event, uint32_t method, uint32_t peer_ip,
		uint32_t tick_ms, uint32_t auth_counter, uint32_t detail);

void bootloader_state_init(void)
{
	uint32_t i;

	s_crypto_selftest_ok = sha256_selftest();
	if (!s_crypto_selftest_ok) {
		printf("** CRYPTO SELFTEST FAILED - firmware verification cannot be trusted! **\r\n");
	}

	s_next_free_slot = IAP_JOURNAL_SLOT_COUNT; /* assume full unless a blank slot is found below */
	s_last_metadata_slot = 0xFFFFFFFFU;
	s_have_log_hash = false;
	s_last_log_event = 0U;
	s_format_unknown = false;

	for (i = 0; i < IAP_JOURNAL_SLOT_COUNT; ) {
		const uint8_t type = slot_type(i);
		const uint8_t slots = slot_count_of(i);

		if (type == IAP_REC_BLANK) {
			s_next_free_slot = i;
			break;
		}
		if ((type == IAP_REC_LOG) && (slots == 1U)) {
			const iap_log_rec_t *rec = (const iap_log_rec_t *)slot_ptr(i);
			uint8_t digest[32];

			s_last_log_event = rec->event;
			sha256((const uint8_t *)rec, IAP_JOURNAL_SLOT_SIZE, digest);
			memcpy(s_last_log_hash, digest, sizeof(s_last_log_hash));
			s_have_log_hash = true;
			i += 1U;
			continue;
		}
		if ((type == IAP_REC_METADATA) && (slots == IAP_METADATA_SLOTS)) {
			s_last_metadata_slot = i;
			i += IAP_METADATA_SLOTS;
			continue;
		}

		/* Not blank and not a record this build understands -- most likely a
		 * journal written by an older layout. Stop here rather than guess a
		 * length and walk off into the middle of somebody else's record. The
		 * sector then stays read-only until an operator erases it, which loses
		 * only the log: the next update rewrites the metadata anyway. */
		printf("** State sector holds an unrecognised record at slot %" PRIu32
				" - logging is disabled until %08" PRIX32 " is erased **\r\n",
				i, (uint32_t)IAP_JOURNAL_BASE);
		s_format_unknown = true;
		break;
	}

	printf("Bootloader state: %" PRIu32 "/%" PRIu32 " journal slots used, metadata %s\r\n",
			s_next_free_slot, (uint32_t)IAP_JOURNAL_SLOT_COUNT,
			(s_last_metadata_slot == 0xFFFFFFFFU) ? "absent" : "present");
	if (bootloader_state_journal_full()) {
		printf("** Journal full - new events are not being recorded. "
				"The next successful update reclaims the sector. **\r\n");
	}
}

bool bootloader_state_journal_full(void)
{
	return s_format_unknown || (journal_room() == 0U);
}

uint32_t bootloader_state_dropped_events(void)
{
	return s_dropped_events;
}

bool bootloader_state_crypto_selftest_passed(void)
{
	return s_crypto_selftest_ok;
}

bool bootloader_state_get_metadata(iap_fw_metadata_t *out)
{
	if (s_last_metadata_slot == 0xFFFFFFFFU) {
		return false;
	}
	memcpy(out, &((const iap_meta_rec_t *)slot_ptr(s_last_metadata_slot))->meta, sizeof(*out));
	return true;
}

void bootloader_state_save_metadata(uint32_t app_size, uint32_t fw_version,
                                     const uint8_t sha256_digest[32], const uint8_t signature[64])
{
	iap_meta_rec_t rec;

	/* The only place that ever erases. Safe precisely here: the application
	 * this metadata will describe has just been written, so whatever the old
	 * record said is already untrue. */
	if ((journal_room() < IAP_METADATA_SLOTS) || s_format_unknown) {
		journal_reclaim();
	}

	memset(&rec, 0, sizeof(rec));
	rec.type = IAP_REC_METADATA;
	rec.slots = (uint8_t)IAP_METADATA_SLOTS;
	rec.meta.app_size = app_size;
	rec.meta.fw_version = fw_version;
	memcpy(rec.meta.sha256, sha256_digest, 32U);
	memcpy(rec.meta.signature, signature, 64U);

	journal_write(&rec, IAP_METADATA_SLOTS);
	s_last_metadata_slot = s_next_free_slot - IAP_METADATA_SLOTS;
}

void bootloader_state_log_event(bootloader_event_type_t event, uint32_t method, uint32_t peer_ip,
                                 uint32_t tick_ms, uint32_t auth_counter)
{
	journal_log((uint8_t)event, method, peer_ip, tick_ms, auth_counter, 0U);
}

/* Never erases: a full journal drops the entry and says so. Erasing to make
 * room here would risk the current metadata for the sake of a log line. */
static void journal_log(uint8_t event, uint32_t method, uint32_t peer_ip,
		uint32_t tick_ms, uint32_t auth_counter, uint32_t detail)
{
	iap_log_rec_t rec;
	uint8_t digest[32];

	if (bootloader_state_journal_full()) {
		s_dropped_events++;
		printf("Journal full - event %u not recorded (%" PRIu32 " dropped so far)\r\n",
				(unsigned)event, s_dropped_events);
		return;
	}

	memset(&rec, 0, sizeof(rec));
	rec.type = IAP_REC_LOG;
	rec.slots = 1U;
	rec.event = event;
	rec.method = (uint8_t)method;
	rec.peer_ip = peer_ip;
	rec.tick_ms = tick_ms;
	rec.auth_counter = auth_counter;
	rec.detail = detail;
	if (s_have_log_hash) {
		memcpy(rec.prev_hash, s_last_log_hash, sizeof(rec.prev_hash));
	}

	journal_write(&rec, 1U);

	sha256((const uint8_t *)slot_ptr(s_next_free_slot - 1U), IAP_JOURNAL_SLOT_SIZE, digest);
	memcpy(s_last_log_hash, digest, sizeof(s_last_log_hash));
	s_have_log_hash = true;
	s_last_log_event = event;
}

uint32_t bootloader_state_last_log_event(void)
{
	return s_last_log_event;
}

void bootloader_state_note_auth_fail(uint32_t method, uint32_t peer_ip, uint32_t tick_ms)
{
	iap_auth_fail_entry_t *slot = &s_auth_fail[s_auth_fail_total % IAP_AUTH_FAIL_LOG_SIZE];

	slot->method = method;
	slot->peer_ip = peer_ip;
	slot->tick_ms = tick_ms;
	s_auth_fail_total++;

	printf("Auth rejected (attempt %" PRIu32 " this boot, peer %08" PRIX32 ")\r\n",
			s_auth_fail_total, peer_ip);
}

uint32_t bootloader_state_auth_fail_count(void)
{
	return s_auth_fail_total;
}

const iap_auth_fail_entry_t *bootloader_state_auth_fail_log(uint32_t *out_count)
{
	*out_count = (s_auth_fail_total < IAP_AUTH_FAIL_LOG_SIZE)
			? s_auth_fail_total : IAP_AUTH_FAIL_LOG_SIZE;
	return s_auth_fail;
}

void bootloader_state_hash_app(uint32_t app_base, uint32_t size, uint8_t out_sha256[32])
{
	sha256((const uint8_t *)app_base, size, out_sha256);
}

void bootloader_state_set_app_valid(bool valid)
{
	s_app_valid = valid;
}

bool bootloader_state_app_is_valid(void)
{
	return s_app_valid;
}

static void journal_write(const void *record, uint32_t slots)
{
	const uint32_t addr = IAP_JOURNAL_BASE + (s_next_free_slot * IAP_JOURNAL_SLOT_SIZE);

	(void)Flash_If_Write((uint8_t *)record, (uint8_t *)addr, slots * IAP_JOURNAL_SLOT_SIZE);
	s_next_free_slot += slots;
}

/* Called from bootloader_state_save_metadata() and nowhere else -- see the
 * header for why that one call site is the only safe moment to erase. The old
 * metadata is deliberately not carried over: the caller is about to write the
 * record that replaces it. */
static void journal_reclaim(void)
{
	const uint32_t discarded = s_next_free_slot;

	printf("Reclaiming state sector (%" PRIu32 " slots discarded)\r\n", discarded);

	(void)Flash_If_Erase(IAP_JOURNAL_BASE, RESERVED_TAIL_SECTORS);

	s_next_free_slot = 0U;
	s_last_metadata_slot = 0xFFFFFFFFU;
	s_have_log_hash = false;
	s_format_unknown = false;
	s_dropped_events = 0U;

	journal_log((uint8_t)IAP_EVT_JOURNAL_RECLAIMED, 0U, 0U, 0U, 0U, discarded);
}

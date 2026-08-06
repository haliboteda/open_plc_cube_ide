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

#define IAP_RECORD_BLANK    0xFFFFFFFFU /* erased Flash reads as this */
#define IAP_RECORD_METADATA 0x4D455441U /* 'META' */
#define IAP_RECORD_LOG      0x4C4F4721U

/* Log payload (type == IAP_RECORD_LOG). 4+4+4+32+4+4+68 = 120 bytes. */
typedef struct {
	uint32_t event;
	uint32_t method;
	uint32_t peer_ip;
	uint8_t  prev_hash[32];
	uint32_t tick_ms;
	uint32_t auth_counter;
	uint8_t  reserved[68];
} iap_log_payload_t;

typedef struct {
	uint32_t type;
	uint32_t seq;
	uint8_t  payload[IAP_JOURNAL_SLOT_SIZE - 8U];
} iap_journal_slot_t;

static uint32_t s_next_free_slot;
static uint32_t s_next_seq;
static uint32_t s_last_metadata_slot;
static uint8_t  s_last_log_hash[32];
static bool     s_have_log_hash;
static bool     s_crypto_selftest_ok;
static bool     s_app_valid;

static const iap_journal_slot_t *slot_ptr(uint32_t index)
{
	return (const iap_journal_slot_t *)(IAP_JOURNAL_BASE + index * IAP_JOURNAL_SLOT_SIZE);
}

static void journal_append(uint32_t type, const uint8_t *payload, uint32_t payload_len);
static void journal_reclaim(void);

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
	s_next_seq = 1U;

	for (i = 0; i < IAP_JOURNAL_SLOT_COUNT; i++) {
		const iap_journal_slot_t *slot = slot_ptr(i);

		if (slot->type == IAP_RECORD_BLANK) {
			s_next_free_slot = i;
			break;
		}
		if (slot->seq >= s_next_seq) {
			s_next_seq = slot->seq + 1U;
		}
		if (slot->type == IAP_RECORD_METADATA) {
			s_last_metadata_slot = i;
		} else if (slot->type == IAP_RECORD_LOG) {
			sha256((const uint8_t *)slot, IAP_JOURNAL_SLOT_SIZE, s_last_log_hash);
			s_have_log_hash = true;
		}
	}

	printf("Bootloader state: %" PRIu32 "/%" PRIu32 " journal slots used, metadata %s\r\n",
			s_next_free_slot, (uint32_t)IAP_JOURNAL_SLOT_COUNT,
			(s_last_metadata_slot == 0xFFFFFFFFU) ? "absent" : "present");
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
	memcpy(out, slot_ptr(s_last_metadata_slot)->payload, sizeof(*out));
	return true;
}

void bootloader_state_save_metadata(uint32_t app_size, uint32_t fw_version,
                                     const uint8_t sha256_digest[32], const uint8_t signature[64])
{
	iap_fw_metadata_t meta;

	memset(&meta, 0, sizeof(meta));
	meta.app_size = app_size;
	meta.fw_version = fw_version;
	memcpy(meta.sha256, sha256_digest, 32U);
	memcpy(meta.signature, signature, 64U);

	journal_append(IAP_RECORD_METADATA, (const uint8_t *)&meta, sizeof(meta));
	s_last_metadata_slot = s_next_free_slot - 1U;
}

void bootloader_state_log_event(bootloader_event_type_t event, uint32_t method, uint32_t peer_ip,
                                 uint32_t tick_ms, uint32_t auth_counter)
{
	iap_log_payload_t evt;

	memset(&evt, 0, sizeof(evt));
	evt.event = (uint32_t)event;
	evt.method = method;
	evt.peer_ip = peer_ip;
	evt.tick_ms = tick_ms;
	evt.auth_counter = auth_counter;
	if (s_have_log_hash) {
		memcpy(evt.prev_hash, s_last_log_hash, 32U);
	}

	journal_append(IAP_RECORD_LOG, (const uint8_t *)&evt, sizeof(evt));
	sha256((const uint8_t *)slot_ptr(s_next_free_slot - 1U), IAP_JOURNAL_SLOT_SIZE, s_last_log_hash);
	s_have_log_hash = true;
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

static void journal_append(uint32_t type, const uint8_t *payload, uint32_t payload_len)
{
	iap_journal_slot_t slot;
	uint32_t addr;

	if (s_next_free_slot >= IAP_JOURNAL_SLOT_COUNT) {
		journal_reclaim();
	}

	memset(&slot, 0, sizeof(slot));
	slot.type = type;
	slot.seq = s_next_seq++;
	if (payload_len > sizeof(slot.payload)) {
		payload_len = (uint32_t)sizeof(slot.payload); /* defensive; callers always pass a fixed struct that fits exactly */
	}
	memcpy(slot.payload, payload, payload_len);

	addr = IAP_JOURNAL_BASE + (s_next_free_slot * IAP_JOURNAL_SLOT_SIZE);
	(void)Flash_If_Write((uint8_t *)&slot, (uint8_t *)addr, sizeof(slot));
	s_next_free_slot++;
}

static void journal_reclaim(void)
{
	iap_fw_metadata_t saved_meta;
	bool had_meta;

	printf("Bootloader state journal full - reclaiming (erasing state sector, metadata preserved)\r\n");

	had_meta = bootloader_state_get_metadata(&saved_meta);

	(void)Flash_If_Erase(IAP_JOURNAL_BASE, RESERVED_TAIL_SECTORS);

	s_next_free_slot = 0U;
	s_have_log_hash = false;
	s_last_metadata_slot = 0xFFFFFFFFU;
	/* s_next_seq keeps counting up across the reclaim so seq numbers stay
	 * globally monotonic even though the old slots are now gone. */

	if (had_meta) {
		journal_append(IAP_RECORD_METADATA, (const uint8_t *)&saved_meta, sizeof(saved_meta));
		s_last_metadata_slot = s_next_free_slot - 1U;
	}

	{
		iap_log_payload_t evt;
		memset(&evt, 0, sizeof(evt));
		evt.event = (uint32_t)IAP_EVT_JOURNAL_RECLAIMED;
		journal_append(IAP_RECORD_LOG, (const uint8_t *)&evt, sizeof(evt));
		sha256((const uint8_t *)slot_ptr(s_next_free_slot - 1U), IAP_JOURNAL_SLOT_SIZE, s_last_log_hash);
		s_have_log_hash = true;
	}
}

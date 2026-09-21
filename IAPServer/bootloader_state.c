/*
 * bootloader_state.c
 *
 * See bootloader_state.h for the on-Flash layout rationale. Sector 15 holds
 * two things with opposite needs: calibration data that must be over-writable,
 * and firmware metadata that is only ever appended. Splitting them is what
 * keeps 547 of every 548 upgrades from touching the calibration area at all.
 *
 * Layout and the reasoning behind it: $PROD/docs/modules/M1/SECTOR-15.md and
 * DECISIONS.md #61.
 *
 * NOTE: the IAP_JOURNAL_* names predate the split and now refer to the
 * metadata area only. Renaming them touches the host test scripts too, so it
 * is tracked separately in $PROD/work/TODO.md.
 */

#include "bootloader_state.h"
#include "usbd_cdc_flash.h"
#include "IAP_config.h"   /* IAP_STAGE_BASE: staging for the calibration carry-over */
#include "sha256.h"
#include <string.h>
#include <stdio.h>
#include <inttypes.h>

extern uint16_t Flash_If_Write(uint8_t *DataAddress, uint8_t *FlashAddress, uint32_t Len);
extern uint16_t Flash_If_Erase(uint32_t Add, uint32_t NbSectors);

/* Calibration data: fixed address at the very start of the sector, written by
 * the production fixture over JLINK. The firmware has no command to write it
 * (DECISIONS.md #45). Nothing here reads it either -- it only has to survive
 * the erase below. */
#define IAP_CALIB_BASE          IAP_STATE_SECTOR_ADDR
#define IAP_CALIB_SIZE          (8U * 1024U)

#define IAP_JOURNAL_BASE        (IAP_STATE_SECTOR_ADDR + IAP_CALIB_SIZE)
#define IAP_JOURNAL_REGION_SIZE ((128U * 1024U) - IAP_CALIB_SIZE)
#define IAP_JOURNAL_SLOT_COUNT  (IAP_JOURNAL_REGION_SIZE / IAP_JOURNAL_SLOT_SIZE)

/* Erased Flash reads as 0xFF, so that is "no record here". 'M' is now the only
 * record type -- the eight event types and their 'L' records were removed
 * because they supported no requirement and had no reader (see
 * $PROD/docs/modules/M1-firmware-upgrade.md). */
#define IAP_REC_BLANK    0xFFU
#define IAP_REC_METADATA 0x4DU /* 'M' */

/* Seven flash words: 8-byte header + the 216-byte payload. */
typedef struct {
	uint8_t  type;      /* IAP_REC_METADATA */
	uint8_t  slots;     /* IAP_METADATA_SLOTS */
	uint16_t reserved0;
	uint32_t reserved1;
	iap_fw_metadata_t meta;
} iap_meta_rec_t;

/* These sizes are the on-Flash format. A stray padding byte would shift every
 * field of every record already written, so fail the build instead. */
_Static_assert(sizeof(iap_meta_rec_t) == (IAP_METADATA_SLOTS * IAP_JOURNAL_SLOT_SIZE),
		"metadata record must fill its slots exactly");

static uint32_t s_next_free_slot;
static uint32_t s_last_metadata_slot;
static bool     s_format_unknown;
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
static bool calib_area_is_blank(void);
static bool metadata_area_full(void);
void bootloader_state_init(void)
{
	uint32_t i;

	s_crypto_selftest_ok = sha256_selftest();
	if (!s_crypto_selftest_ok) {
		printf("** CRYPTO SELFTEST FAILED - firmware verification cannot be trusted! **\r\n");
	}

	s_next_free_slot = IAP_JOURNAL_SLOT_COUNT; /* assume full unless a blank slot is found below */
	s_last_metadata_slot = 0xFFFFFFFFU;
	s_format_unknown = false;

	for (i = 0; i < IAP_JOURNAL_SLOT_COUNT; ) {
		const uint8_t type = slot_type(i);
		const uint8_t slots = slot_count_of(i);

		if (type == IAP_REC_BLANK) {
			s_next_free_slot = i;
			break;
		}
		if ((type == IAP_REC_METADATA) && (slots == IAP_METADATA_SLOTS)) {
			s_last_metadata_slot = i;
			i += IAP_METADATA_SLOTS;
			continue;
		}

		/* Not blank and not a record this build understands -- most likely a
		 * layout written by an older build. Stop here rather than guess a
		 * length and walk off into the middle of somebody else's record. The
		 * area then stays read-only until the next successful update reclaims
		 * it, which loses nothing: that update rewrites the metadata anyway. */
		printf("** State sector holds an unrecognised record at slot %" PRIu32
				" - it stays read-only until the next successful update erases %08" PRIX32 " **\r\n",
				i, (uint32_t)IAP_JOURNAL_BASE);
		s_format_unknown = true;
		break;
	}

	printf("Bootloader state: %" PRIu32 "/%" PRIu32 " metadata slots used, metadata %s\r\n",
			s_next_free_slot, (uint32_t)IAP_JOURNAL_SLOT_COUNT,
			(s_last_metadata_slot == 0xFFFFFFFFU) ? "absent" : "present");
	if (metadata_area_full()) {
		printf("** Metadata area full - the next successful update reclaims it. **\r\n");
	}
}

static bool metadata_area_full(void)
{
	return s_format_unknown || (journal_room() == 0U);
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

void bootloader_state_save_metadata(uint32_t app_size, const uint8_t signature[64],
                                     const iap_cert_t *cert)
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
	memcpy(rec.meta.signature, signature, 64U);
	memcpy(&rec.meta.cert, cert, sizeof(rec.meta.cert));

	journal_write(&rec, IAP_METADATA_SLOTS);
	s_last_metadata_slot = s_next_free_slot - IAP_METADATA_SLOTS;
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

	printf("Reclaiming metadata area (%" PRIu32 " slots discarded)\r\n", discarded);

	/* The erase granularity is the whole 128 KiB sector, so this takes the
	 * calibration area at the front of it along too. Carry it across when it
	 * holds anything: losing metadata costs one re-upload, losing calibration
	 * means a trip back to the production line -- a reflash cannot restore it.
	 *
	 * Today the area is always blank, because the calibration feature is not
	 * implemented (that code is the user's to write, DECISIONS.md #45/#61), so
	 * the common path is a plain erase. The check is here so that the day it
	 * does hold something, a reclaim does not silently destroy it. */
	if (calib_area_is_blank()) {
		(void)Flash_If_Erase(IAP_STATE_SECTOR_ADDR, RESERVED_TAIL_SECTORS);
	} else {
		/* Staged in SDRAM: a reclaim can only happen inside
		 * bootloader_state_save_metadata(), by which point the new image is
		 * already committed to flash and the staging buffer is free. */
		uint8_t *carry = (uint8_t *)IAP_STAGE_BASE;

		memcpy(carry, (const void *)IAP_CALIB_BASE, IAP_CALIB_SIZE);
		(void)Flash_If_Erase(IAP_STATE_SECTOR_ADDR, RESERVED_TAIL_SECTORS);
		(void)Flash_If_Write(carry, (uint8_t *)IAP_CALIB_BASE, IAP_CALIB_SIZE);
	}

	s_next_free_slot = 0U;
	s_last_metadata_slot = 0xFFFFFFFFU;
	s_format_unknown = false;
}

/* True when nothing has ever been written to the calibration area. Erased NOR
 * flash reads as 0xFF, so an all-0xFF area has nothing worth preserving. */
static bool calib_area_is_blank(void)
{
	const uint8_t *cal = (const uint8_t *)IAP_CALIB_BASE;
	uint32_t i;

	for (i = 0U; i < IAP_CALIB_SIZE; i++) {
		if (cal[i] != 0xFFU) {
			return false;
		}
	}
	return true;
}

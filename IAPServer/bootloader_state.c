/*
 * bootloader_state.c
 *
 * Sector 15, the bootloader's own state: calibration, the root area and
 * firmware metadata, plus a completion marker. Layout, the reclaim and what a
 * power cut does at each step: $PROD/docs/modules/M1/SECTOR-15.md.
 */

#include "bootloader_state.h"
#include "bkp_stash.h"
#include "owner_slot.h"
#include "usbd_cdc_flash.h"
#include "sha256.h"
#include "calib_area.h"
#include <string.h>
#include <stdio.h>
#include <inttypes.h>

extern uint16_t Flash_If_Erase(uint32_t Add, uint32_t NbSectors);

#define IAP_SECTOR_SIZE      (128U * 1024U)

/* Calibration data: fixed address at the start of the sector, written by the
 * production fixture over JLINK (DECISIONS.md #61). Nothing here reads it; it
 * only has to survive a reclaim. Format: calib_area.h. */
#define IAP_CALIB_BASE       IAP_STATE_SECTOR_ADDR
#define IAP_CALIB_SIZE       CALIB_AREA_SIZE
/* Firmware addresses only; the host harness (T2-34) maps the sector to RAM. */
#ifndef BOOTLOADER_STATE_HOST_TEST
_Static_assert(IAP_CALIB_BASE == CALIB_AREA_ADDR, "calibration area moved without calib_area.h");
#endif

#define IAP_ROOT_AREA_BASE   (IAP_CALIB_BASE + IAP_CALIB_SIZE)
#ifndef BOOTLOADER_STATE_HOST_TEST
_Static_assert(IAP_ROOT_AREA_BASE == OWNER_SLOT_BASE, "root area moved without owner_slot.h");
#endif

#define IAP_MARKER_ADDR      (IAP_STATE_SECTOR_ADDR + IAP_SECTOR_SIZE - IAP_META_SLOT_SIZE)
#define IAP_META_BASE        (IAP_ROOT_AREA_BASE + OWNER_SLOT_SIZE)
#define IAP_META_REGION_SIZE (IAP_MARKER_ADDR - IAP_META_BASE)
#define IAP_META_SLOT_COUNT  (IAP_META_REGION_SIZE / IAP_META_SLOT_SIZE)

/* Erased Flash reads as 0xFF, so that is "no record here". 'M' is the only
 * metadata record type. */
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
_Static_assert(sizeof(iap_meta_rec_t) == (IAP_METADATA_SLOTS * IAP_META_SLOT_SIZE),
		"metadata record must fill its slots exactly");

/* Written last by a reclaim: its presence says the sector was rewritten in
 * full under this layout. */
#define IAP_MARKER_MAGIC   0x4C353153UL   /* "S15L" */
#define IAP_MARKER_LAYOUT  1U
typedef struct {
	uint32_t magic;
	uint32_t layout;
	uint8_t  zero[24];
} iap_marker_t;
_Static_assert(sizeof(iap_marker_t) == IAP_META_SLOT_SIZE, "marker is one flash word");

/* What a reclaim carries across the erase, and the backup-SRAM copy of it. */
typedef struct {
	owner_carry_t  carry;
	uint32_t       has_meta;
	iap_meta_rec_t meta;
} s15_stash_t;
_Static_assert(sizeof(s15_stash_t) <= BKP_STASH_MAX_PAYLOAD, "reclaim copy does not fit backup SRAM");

static bool     s_inited;
static uint32_t s_next_free_slot;
static uint32_t s_last_metadata_slot;
static bool     s_format_unknown;
static bool     s_crypto_selftest_ok;
static bool     s_app_valid;

static uint32_t s_auth_fail_total;

/* Too large for the stack; only one reclaim runs at a time. RAM rather than
 * SDRAM: a reclaim can run at boot, before the FMC is up. */
static s15_stash_t s_stash;
static uint8_t     s_calib[IAP_CALIB_SIZE];

static const void *slot_ptr(uint32_t index)
{
	return (const void *)(IAP_META_BASE + index * IAP_META_SLOT_SIZE);
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
static uint32_t meta_room(void)
{
	return (s_next_free_slot >= IAP_META_SLOT_COUNT)
			? 0U : (IAP_META_SLOT_COUNT - s_next_free_slot);
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

static bool marker_is_valid(void)
{
	const iap_marker_t *m = (const iap_marker_t *)IAP_MARKER_ADDR;

	return (m->magic == IAP_MARKER_MAGIC) && (m->layout == IAP_MARKER_LAYOUT);
}

static bool write_marker(void)
{
	iap_marker_t m;

	memset(&m, 0, sizeof(m));
	m.magic = IAP_MARKER_MAGIC;
	m.layout = IAP_MARKER_LAYOUT;
	return Flash_If_Write((uint8_t *)&m, (uint8_t *)IAP_MARKER_ADDR, sizeof(m)) == 0U;
}

static void meta_scan(void)
{
	uint32_t i;

	s_next_free_slot = IAP_META_SLOT_COUNT; /* assume full unless a blank slot is found below */
	s_last_metadata_slot = 0xFFFFFFFFU;
	s_format_unknown = false;

	for (i = 0; i < IAP_META_SLOT_COUNT; ) {
		const uint8_t type = slot_type(i);
		const uint8_t slots = slot_count_of(i);

		if (type == IAP_REC_BLANK) {
			s_next_free_slot = i;
			break;
		}
		if ((type == IAP_REC_METADATA) && (slots == IAP_METADATA_SLOTS)
				&& ((i + IAP_METADATA_SLOTS) <= IAP_META_SLOT_COUNT)) {
			s_last_metadata_slot = i;
			i += IAP_METADATA_SLOTS;
			continue;
		}

		/* Not a record this build understands. Stop rather than guess a
		 * length; the next reclaim clears it. */
		printf("** State sector holds an unrecognised record at slot %" PRIu32
				" - the next reclaim clears it **\r\n", i);
		s_format_unknown = true;
		break;
	}
}

/*
 * Steps 1-6 of a reclaim (SECTOR-15.md): calibration into RAM, `st` into
 * backup SRAM, erase, write back calibration / root area / metadata, marker,
 * clear the backup copy. At boot `st` already is the backup copy, so step 2
 * is skipped.
 *
 * A write failure leaves the backup copy in place, so the next boot finishes
 * the job. Returns false then.
 */
static bool s15_rewrite(const s15_stash_t *st, bool stash_it)
{
	bool ok = true;

	memcpy(s_calib, (const void *)IAP_CALIB_BASE, IAP_CALIB_SIZE);

	if (stash_it && !bkp_stash_save(st, sizeof(*st))) {
		/* Proceed: refusing would stop every later upload and handover. A cut
		 * in the next second then costs the root, like a factory reset.
		 * See SECTOR-15.md "回收时掉电". */
		printf("** Backup SRAM unavailable - reclaiming with no safety copy **\r\n");
	}

	printf("Reclaiming sector 15 (%" PRIu32 " metadata slots, root area compacted). "
			"DO NOT CUT POWER.\r\n", s_next_free_slot);

	if (Flash_If_Erase(IAP_STATE_SECTOR_ADDR, RESERVED_TAIL_SECTORS) != 0U) {
		printf("** Sector 15 erase FAILED **\r\n");
		ok = false;
	}
	if (ok && !bytes_are_erased(s_calib, IAP_CALIB_SIZE)) {
		ok = (Flash_If_Write(s_calib, (uint8_t *)IAP_CALIB_BASE, IAP_CALIB_SIZE) == 0U);
	}
	if (ok) {
		ok = owner_slot_write_carry(&st->carry);
	}
	if (ok && (st->has_meta != 0U)) {
		ok = (Flash_If_Write((uint8_t *)&st->meta, (uint8_t *)IAP_META_BASE,
				sizeof(st->meta)) == 0U);
	}
	if (ok) {
		ok = write_marker();
	}
	if (ok) {
		bkp_stash_clear();
	} else {
		printf("** Sector 15 rewrite FAILED - the next boot retries from the backup copy **\r\n");
	}

	meta_scan();
	owner_slot_rescan();
	return ok;
}

/*
 * Boot-time check (SECTOR-15.md "开机判断"). A valid backup copy always wins:
 * it is cleared only after a rewrite has fully completed.
 */
static void s15_recover(void)
{
	if (!bkp_stash_enable()) {
		printf("** Backup SRAM regulator not ready - reclaims run without a safety copy **\r\n");
	}

	if (bkp_stash_load(&s_stash, sizeof(s_stash))) {
		printf("Sector 15: finishing a reclaim cut short by a power loss, from the "
				"backup-SRAM copy\r\n");
		(void)s15_rewrite(&s_stash, false);
		return;
	}
	bkp_stash_clear();   /* a copy that does not verify is never used */
	if (marker_is_valid()) {
		return;
	}
	if (bytes_are_erased((const uint8_t *)IAP_ROOT_AREA_BASE,
			IAP_SECTOR_SIZE - IAP_CALIB_SIZE)) {
		/* A factory board: nothing but calibration. */
		if (!write_marker()) {
			printf("** Sector 15: writing the layout marker FAILED **\r\n");
		}
		return;
	}

	printf("** Sector 15 is not in this layout, or a reclaim was cut short with no "
			"backup copy - rebuilt with calibration only. The board has no root "
			"and no firmware metadata. **\r\n");
	memset(&s_stash, 0, sizeof(s_stash));
	(void)s15_rewrite(&s_stash, false);
}

void bootloader_state_init(void)
{
	if (s_inited) {
		return;
	}
	s_inited = true;

	s_crypto_selftest_ok = sha256_selftest();
	if (!s_crypto_selftest_ok) {
		printf("** CRYPTO SELFTEST FAILED - firmware verification cannot be trusted! **\r\n");
	}

	meta_scan();
	s15_recover();

	printf("Bootloader state: %" PRIu32 "/%" PRIu32 " metadata slots used, metadata %s\r\n",
			s_next_free_slot, (uint32_t)IAP_META_SLOT_COUNT,
			(s_last_metadata_slot == 0xFFFFFFFFU) ? "absent" : "present");
	if (s_format_unknown || (meta_room() < IAP_METADATA_SLOTS)) {
		printf("** Metadata area full - the next successful update reclaims sector 15. **\r\n");
	}
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

static void build_meta_rec(iap_meta_rec_t *rec, uint32_t app_size,
		const uint8_t signature[64], const iap_cert_t *cert)
{
	memset(rec, 0, sizeof(*rec));
	rec->type = IAP_REC_METADATA;
	rec->slots = (uint8_t)IAP_METADATA_SLOTS;
	rec->meta.app_size = app_size;
	memcpy(rec->meta.signature, signature, 64U);
	memcpy(&rec->meta.cert, cert, sizeof(rec->meta.cert));
}

void bootloader_state_save_metadata(uint32_t app_size, const uint8_t signature[64],
                                     const iap_cert_t *cert)
{
	iap_meta_rec_t rec;
	const uintptr_t addr = IAP_META_BASE + (s_next_free_slot * IAP_META_SLOT_SIZE);

	build_meta_rec(&rec, app_size, signature, cert);

	/* Full: the new record goes in as part of the reclaim, so the backup copy
	 * holds the metadata of the image just written, not the stale one. */
	if ((meta_room() < IAP_METADATA_SLOTS) || s_format_unknown) {
		memset(&s_stash, 0, sizeof(s_stash));
		owner_slot_build_carry(&s_stash.carry);
		s_stash.has_meta = 1U;
		s_stash.meta = rec;
		(void)s15_rewrite(&s_stash, true);
		return;
	}

	(void)Flash_If_Write((uint8_t *)&rec, (uint8_t *)addr, sizeof(rec));
	s_last_metadata_slot = s_next_free_slot;
	s_next_free_slot += IAP_METADATA_SLOTS;
}

bool bootloader_state_reclaim(const owner_carry_t *carry)
{
	bootloader_state_init();   /* a factory reset can get here before server_decide() */
	memset(&s_stash, 0, sizeof(s_stash));
	s_stash.carry = *carry;
	if (s_last_metadata_slot != 0xFFFFFFFFU) {
		s_stash.has_meta = 1U;
		memcpy(&s_stash.meta, slot_ptr(s_last_metadata_slot), sizeof(s_stash.meta));
	}
	return s15_rewrite(&s_stash, true);
}

void bootloader_state_note_auth_fail(uint32_t peer_ip)
{
	s_auth_fail_total++;

	printf("Auth rejected (attempt %" PRIu32 " this boot, peer %08" PRIX32 ")\r\n",
			s_auth_fail_total, peer_ip);
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

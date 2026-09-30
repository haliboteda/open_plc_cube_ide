/*
 * bkp_stash.c -- see bkp_stash.h.
 */

#include "bkp_stash.h"
#include "iap_keyderive.h"
#include "sha256.h"
#include "stm32h7xx_hal.h"
#include <string.h>

#define BKP_STASH_MAGIC  0x48535453UL   /* "STSH" */

typedef struct {
	uint32_t magic;
	uint32_t len;
	uint8_t  uid[IAP_MACHINE_ID_SIZE];
	uint8_t  reserved[12];
	uint8_t  digest[SHA256_DIGEST_SIZE];   /* over uid || payload */
} bkp_stash_hdr_t;

_Static_assert(sizeof(bkp_stash_hdr_t) == 64U, "stash header must be 64 bytes");

#define STASH_HDR     ((volatile bkp_stash_hdr_t *)D3_BKPSRAM_BASE)
#define STASH_PAYLOAD ((uint8_t *)(D3_BKPSRAM_BASE + sizeof(bkp_stash_hdr_t)))

static bool s_enabled;

static void digest_of(const uint8_t uid[IAP_MACHINE_ID_SIZE], const uint8_t *payload,
		uint32_t len, uint8_t out[SHA256_DIGEST_SIZE])
{
	sha256_ctx_t ctx;

	sha256_init(&ctx);
	sha256_update(&ctx, uid, IAP_MACHINE_ID_SIZE);
	sha256_update(&ctx, payload, len);
	sha256_final(&ctx, out);
}

bool bkp_stash_enable(void)
{
	if (s_enabled) {
		return true;
	}
	__HAL_RCC_BKPRAM_CLK_ENABLE();
	HAL_PWR_EnableBkUpAccess();
	/* Waits for BRRDY: only then is what gets written kept on VBAT. */
	s_enabled = (HAL_PWREx_EnableBkUpReg() == HAL_OK);
	return s_enabled;
}

bool bkp_stash_save(const void *payload, uint32_t len)
{
	bkp_stash_hdr_t hdr;

	if (!bkp_stash_enable() || (len > BKP_STASH_MAX_PAYLOAD)) {
		return false;
	}
	memset(&hdr, 0, sizeof(hdr));
	hdr.magic = BKP_STASH_MAGIC;
	hdr.len = len;
	iap_keyderive_get_machine_id(hdr.uid);
	digest_of(hdr.uid, (const uint8_t *)payload, len, hdr.digest);

	/* Payload before header: a stash cut short reads as absent, not as a
	 * truncated copy with a valid header. */
	STASH_HDR->magic = 0U;
	memcpy(STASH_PAYLOAD, payload, len);
	memcpy((void *)((uint8_t *)STASH_HDR + 4U), (const uint8_t *)&hdr + 4U,
			sizeof(hdr) - 4U);
	__DSB();
	STASH_HDR->magic = BKP_STASH_MAGIC;
	__DSB();

	return (memcmp(STASH_PAYLOAD, payload, len) == 0);
}

bool bkp_stash_load(void *out, uint32_t len)
{
	bkp_stash_hdr_t hdr;
	uint8_t my_uid[IAP_MACHINE_ID_SIZE];
	uint8_t digest[SHA256_DIGEST_SIZE];

	if (!bkp_stash_enable()) {
		return false;
	}
	memcpy(&hdr, (const void *)STASH_HDR, sizeof(hdr));
	if ((hdr.magic != BKP_STASH_MAGIC) || (hdr.len != len)) {
		return false;
	}
	iap_keyderive_get_machine_id(my_uid);
	if (memcmp(hdr.uid, my_uid, sizeof(my_uid)) != 0) {
		return false;
	}
	digest_of(hdr.uid, STASH_PAYLOAD, len, digest);
	if (memcmp(digest, hdr.digest, sizeof(digest)) != 0) {
		return false;
	}
	memcpy(out, STASH_PAYLOAD, len);
	return true;
}

void bkp_stash_clear(void)
{
	if (bkp_stash_enable()) {
		STASH_HDR->magic = 0U;
		__DSB();
	}
}

/*
 * iap_auth.c
 *
 * See iap_auth.h. No hardware RNG is enabled on this board (HAL_RNG is not
 * configured), so the nonce is built from a monotonic counter persisted in
 * an RTC backup register (survives reset/power-cycle as long as VBAT is
 * maintained) plus the device UID and the current tick count.
 *
 * Note this counter stays in the backup register even though the boot-mode
 * request moved out to SRAM4 (IAP_boot_handoff.h): the two want opposite
 * lifetimes. A boot request must NOT survive a power cycle, whereas this counter
 * must, or nonces would repeat across power cycles and defeat the replay check. Uniqueness -- not
 * unpredictability -- is what defeats replay here: the HMAC key is what an
 * attacker actually needs and never gets from observing nonces.
 */

#include "iap_auth.h"
#include "iap_keyderive.h"
#include "sha256.h"
#include "bootloader_state.h"
#include "rtc.h"
#include "main.h"
#include <string.h>
#include <stdio.h>
#include <inttypes.h>

#ifndef IAP_AUTH_COUNTER_BKP_REG
#define IAP_AUTH_COUNTER_BKP_REG RTC_BKP_DR1
#endif

/* Second register holds a fixed witness value. It can only read back correctly
 * if the backup domain survived, which is what tells us the VBAT cell is doing
 * its job -- nonce uniqueness across power cycles depends on it.
 *
 * DR3, not DR2: the application image uses DR2 for its own nonce counter
 * (core libraries/OpenPLC_IAP/src/iap_auth.c). "The two never run at the same
 * time" is not the same as "they cannot collide" -- a backup register exists
 * precisely to carry state across that handover, so taking turns on one is a
 * collision. It broke both directions: the application's counter overwrote this
 * witness, making every boot report the backup domain as lost, and this
 * witness reset the application's counter to a fixed value, so the application
 * reissued the same nonce numbers after every visit to the bootloader.
 *
 * Backup register allocation is shared state across three repositories with no
 * shared build -- see the table in docs/ARCHITECTURE.md before claiming one. */
#ifndef IAP_VBAT_WITNESS_BKP_REG
#define IAP_VBAT_WITNESS_BKP_REG RTC_BKP_DR3
#endif
#define IAP_VBAT_WITNESS_VALUE 0x56424154U /* 'VBAT' */

static uint8_t  s_nonce[IAP_AUTH_NONCE_SIZE];
static bool     s_nonce_pending;
static uint32_t s_nonce_issue_tick;

static uint32_t next_counter(void)
{
	uint32_t v = HAL_RTCEx_BKUPRead(&hrtc, IAP_AUTH_COUNTER_BKP_REG) + 1U;
	HAL_PWR_EnableBkUpAccess();
	HAL_RTCEx_BKUPWrite(&hrtc, IAP_AUTH_COUNTER_BKP_REG, v);
	HAL_PWR_DisableBkUpAccess();
	return v;
}

static bool constant_time_eq(const uint8_t *a, const uint8_t *b, uint32_t len)
{
	uint8_t diff = 0;
	uint32_t i;
	for (i = 0; i < len; i++) {
		diff |= a[i] ^ b[i];
	}
	return diff == 0U;
}

void iap_auth_issue_challenge(char *out_hex)
{
	uint32_t counter = next_counter();
	uint32_t uid0 = HAL_GetUIDw0();
	uint32_t tick = HAL_GetTick();
	uint32_t i;

	memcpy(s_nonce, &counter, 4U);
	memcpy(s_nonce + 4, &uid0, 4U);
	memcpy(s_nonce + 8, &tick, 4U);
	memset(s_nonce + 12, 0, 4U);

	s_nonce_pending = true;
	s_nonce_issue_tick = tick;

	for (i = 0; i < IAP_AUTH_NONCE_SIZE; i++) {
		sprintf(out_hex + i * 2U, "%02x", s_nonce[i]);
	}
	out_hex[IAP_AUTH_NONCE_SIZE * 2U] = '\0';
}

bool iap_auth_verify_and_consume(const uint8_t *msg, uint32_t msg_len, const uint8_t hmac[IAP_AUTH_HMAC_SIZE])
{
	uint8_t buf[IAP_AUTH_NONCE_SIZE + 256U];
	uint8_t calc[IAP_AUTH_HMAC_SIZE];
	uint8_t device_key[IAP_DEVICE_KEY_SIZE];

	/* If the self-test run at boot (bootloader_state_init) found the
	 * SHA-256/HMAC implementation broken, no result it produces can be
	 * trusted -- mirror the same precondition server_decide() already
	 * applies to the boot-time signature check, rather than computing an
	 * HMAC with a known-unreliable primitive and trusting the answer. */
	if (!bootloader_state_crypto_selftest_passed()) {
		return false;
	}

	if (!s_nonce_pending) {
		return false;
	}
	s_nonce_pending = false; /* one-shot: consumed whether this check passes or not */

	if ((HAL_GetTick() - s_nonce_issue_tick) > IAP_AUTH_NONCE_TTL_MS) {
		printf("Auth nonce expired\r\n");
		return false;
	}
	if (msg_len > sizeof(buf) - IAP_AUTH_NONCE_SIZE) {
		return false;
	}

	memcpy(buf, s_nonce, IAP_AUTH_NONCE_SIZE);
	memcpy(buf + IAP_AUTH_NONCE_SIZE, msg, msg_len);

	iap_keyderive_get_device_key(device_key);
	hmac_sha256(device_key, sizeof(device_key), buf, IAP_AUTH_NONCE_SIZE + msg_len, calc);

	return constant_time_eq(calc, hmac, IAP_AUTH_HMAC_SIZE);
}

uint32_t iap_auth_get_counter(void)
{
	return HAL_RTCEx_BKUPRead(&hrtc, IAP_AUTH_COUNTER_BKP_REG);
}

void iap_auth_report_backup_domain(void)
{
	uint32_t witness = HAL_RTCEx_BKUPRead(&hrtc, IAP_VBAT_WITNESS_BKP_REG);
	uint32_t counter = HAL_RTCEx_BKUPRead(&hrtc, IAP_AUTH_COUNTER_BKP_REG);

	if (witness == IAP_VBAT_WITNESS_VALUE) {
		printf("Backup domain retained, nonce counter = %" PRIu32 "\r\n", counter);
		return;
	}

	/* Losing the domain zeroes every register in it, so a counter that is still
	 * counting means the witness read is wrong rather than the domain gone.
	 * Report what was actually read: claiming replay protection is weakened when
	 * it is not costs more than saying nothing. */
	if (counter != 0U) {
		printf("** Backup domain witness missing, but nonce counter = %" PRIu32 ". **\r\n"
				"** Domain contents survived; treating the witness read as unreliable. **\r\n",
				counter);
	} else {
		printf("** Backup domain was lost - RTC battery absent or empty. **\r\n"
				"** Nonce counter is zero; replay protection is weakened. **\r\n");
	}

	HAL_PWR_EnableBkUpAccess();
	HAL_RTCEx_BKUPWrite(&hrtc, IAP_VBAT_WITNESS_BKP_REG, IAP_VBAT_WITNESS_VALUE);
	HAL_PWR_DisableBkUpAccess();
}

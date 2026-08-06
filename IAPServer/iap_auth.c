/*
 * iap_auth.c
 *
 * See iap_auth.h. No hardware RNG is enabled on this board (HAL_RNG is not
 * configured), so the nonce is built from a monotonic counter persisted in
 * an RTC backup register (survives reset/power-cycle as long as VBAT is
 * maintained, same mechanism the bootloader already uses for MAGIC_BKP_REG)
 * plus the device UID and the current tick count. Uniqueness -- not
 * unpredictability -- is what defeats replay here: the HMAC key is what an
 * attacker actually needs and never gets from observing nonces.
 */

#include "iap_auth.h"
#include "sha256.h"
#include "bootloader_state.h"
#include "rtc.h"
#include "main.h"
#include <string.h>
#include <stdio.h>

#ifndef IAP_AUTH_COUNTER_BKP_REG
#define IAP_AUTH_COUNTER_BKP_REG RTC_BKP_DR1
#endif

/* *** PLACEHOLDER TEST-ONLY KEY. See IAPServer/keys/README.md. Every device
 * built from this source tree shares this exact key until it is replaced
 * with a real per-device secret from your own provisioning process. *** */
static const uint8_t iap_auth_key[32] = {
	0x49, 0x41, 0x50, 0x2d, 0x54, 0x45, 0x53, 0x54,
	0x2d, 0x4b, 0x45, 0x59, 0x2d, 0x44, 0x4f, 0x2d,
	0x4e, 0x4f, 0x54, 0x2d, 0x55, 0x53, 0x45, 0x2d,
	0x49, 0x4e, 0x2d, 0x50, 0x52, 0x4f, 0x44, 0x21
};

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

	/* If the self-test run at boot (bootloader_state_init) found the
	 * SHA-256/HMAC implementation broken, no result it produces can be
	 * trusted -- mirror the same precondition server_init() already
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

	hmac_sha256(iap_auth_key, sizeof(iap_auth_key), buf, IAP_AUTH_NONCE_SIZE + msg_len, calc);

	return constant_time_eq(calc, hmac, IAP_AUTH_HMAC_SIZE);
}

uint32_t iap_auth_get_counter(void)
{
	return HAL_RTCEx_BKUPRead(&hrtc, IAP_AUTH_COUNTER_BKP_REG);
}

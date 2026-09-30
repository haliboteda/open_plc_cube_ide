/*
 * iap_auth.c
 *
 * See iap_auth.h. The nonce is 16 bytes straight from the RNG peripheral, so
 * none of it has to survive a power cycle. It used to be a counter kept in an
 * RTC backup register; that counter is gone, along with everything it needed to
 * stay alive -- see $PROD/docs/tables/DECISIONS.md, decision 66.
 *
 * Uniqueness -- not unpredictability -- is what defeats replay here: the leaf
 * private key is what an attacker actually needs, and observing nonces never
 * yields it.
 */

#include "iap_auth.h"
#include "iap_cert.h"
#include "owner_slot.h"
#include "fw_verify.h"
#include "sha256.h"
#include "bootloader_state.h"
#include "rtc.h"
#include "rng.h"
#include "main.h"
#include <string.h>
#include <stdio.h>
#include <inttypes.h>

/* A fixed witness value. It can only read back correctly if the backup domain
 * survived, which is what tells us the VBAT cell is doing its job. The nonce no
 * longer depends on that (decision 66), but the RTC's own timekeeping does.
 *
 * Backup register allocation is shared state across three repositories with no
 * shared build -- see the table in $PROD/docs/repo/ARCHITECTURE.md before claiming one. */
#ifndef IAP_VBAT_WITNESS_BKP_REG
#define IAP_VBAT_WITNESS_BKP_REG RTC_BKP_DR3
#endif
#define IAP_VBAT_WITNESS_VALUE 0x56424154U /* 'VBAT' */

static uint8_t  s_nonce[IAP_AUTH_NONCE_SIZE];
static bool     s_nonce_pending;
static uint32_t s_nonce_issue_tick;

/* Fills words[] from the RNG. False means the peripheral did not deliver, and
 * the caller must then refuse to issue a challenge rather than use whatever the
 * data register held -- that is usually zero, which would hand out the same
 * nonce every time.
 *
 * Deliberately different from the Arduino-side copy: the two reach different RNG
 * handles. Only iap_auth_issue_challenge() is compared across the repositories. */
static bool rng_words(uint32_t *words, uint32_t n)
{
	uint32_t i;

	for (i = 0; i < n; i++) {
		if (HAL_RNG_GenerateRandomNumber(&hrng, &words[i]) != HAL_OK) {
			printf("RNG failed (error 0x%08" PRIX32 "), refusing to issue a challenge\r\n",
					HAL_RNG_GetError(&hrng));
			return false;
		}
	}
	return true;
}

bool iap_auth_issue_challenge(char *out_hex)
{
	uint32_t words[IAP_AUTH_NONCE_SIZE / 4U];
	uint32_t i;

	/* Dropped before the RNG is asked: a failed attempt must not leave the
	 * previous nonce accepting answers. */
	s_nonce_pending = false;

	if (!rng_words(words, IAP_AUTH_NONCE_SIZE / 4U)) {
		return false;
	}
	memcpy(s_nonce, words, IAP_AUTH_NONCE_SIZE);

	s_nonce_pending = true;
	s_nonce_issue_tick = HAL_GetTick();

	for (i = 0; i < IAP_AUTH_NONCE_SIZE; i++) {
		sprintf(out_hex + i * 2U, "%02x", s_nonce[i]);
	}
	out_hex[IAP_AUTH_NONCE_SIZE * 2U] = '\0';
	return true;
}

bool iap_auth_verify_and_consume(const uint8_t *msg, uint32_t msg_len,
		const iap_cert_t *cert, const uint8_t nonce_sig[64])
{
	uint8_t buf[IAP_AUTH_NONCE_SIZE + 256U];
	uint8_t digest[SHA256_DIGEST_SIZE];

	/* If the self-test run at boot (bootloader_state_init) found the
	 * SHA-256 implementation broken, no result it produces can be trusted --
	 * mirror the same precondition server_decide() already applies to the
	 * boot-time signature check, rather than computing a digest with a
	 * known-unreliable primitive and trusting the answer. */
	if (!bootloader_state_crypto_selftest_passed()) {
		printf("Auth rejected: crypto self-test failed at boot, not trusting the check\r\n");
		return false;
	}

	if (!s_nonce_pending) {
		printf("Auth rejected: no nonce pending, call authchallenge first\r\n");
		return false;
	}
	s_nonce_pending = false; /* one-shot: consumed whether this check passes or not */

	if ((HAL_GetTick() - s_nonce_issue_tick) > IAP_AUTH_NONCE_TTL_MS) {
		printf("Auth nonce expired\r\n");
		return false;
	}
	if (msg_len > sizeof(buf) - IAP_AUTH_NONCE_SIZE) {
		printf("Auth rejected: message too long (%" PRIu32 " bytes)\r\n", msg_len);
		return false;
	}

	/* Not folded into the signed message: the certificate isn't secret or
	 * session-specific (its own validity is anchored by root_sig, not by
	 * nonce freshness), so it is checked as its own independent step.
	 *
	 * Revocation is checked here too, not only at upload time -- a revoked
	 * colleague should not be able to open a session at all, whether or not
	 * they follow it with an upload. */
	if (owner_slot_root() == NULL) {
		printf("Auth rejected: this board has no root - claim it first\r\n");
		return false;
	}
	if (!iap_cert_verify(cert, owner_slot_root(),
			owner_slot_is_revoked(cert->leaf_pubkey))) {
		printf("Auth rejected: certificate does not verify against the trusted "
				"root, or the leaf has been revoked\r\n");
		return false;
	}

	memcpy(buf, s_nonce, IAP_AUTH_NONCE_SIZE);
	memcpy(buf + IAP_AUTH_NONCE_SIZE, msg, msg_len);
	sha256(buf, IAP_AUTH_NONCE_SIZE + msg_len, digest);

	if (!fw_verify_signature_with_key(cert->leaf_pubkey, digest, nonce_sig)) {
		printf("Auth rejected: signature does not match the challenge\r\n");
		return false;
	}
	return true;
}

void iap_auth_report_backup_domain(void)
{
	uint32_t witness = HAL_RTCEx_BKUPRead(&hrtc, IAP_VBAT_WITNESS_BKP_REG);

	if (witness == IAP_VBAT_WITNESS_VALUE) {
		printf("Backup domain retained\r\n");
	} else {
		/* Do not name a cause here. A mismatched RTCSEL between bootloader and
		 * application wipes this domain with a healthy battery in place, and
		 * blaming the battery cost a full day of debugging (2026-09-18).
		 * See $PROD/docs/tables/DECISIONS.md, decision 57.
		 *
		 * Says nothing about replay protection: since decision 66 the nonce does
		 * not come from this domain. What is lost is the RTC's time. */
		printf("** Backup domain was lost - VBAT supply or RTC clock source changed. **\r\n"
				"** The RTC has restarted from a fixed time. **\r\n");
	}

	HAL_PWR_EnableBkUpAccess();
	HAL_RTCEx_BKUPWrite(&hrtc, IAP_VBAT_WITNESS_BKP_REG, IAP_VBAT_WITNESS_VALUE);
	HAL_PWR_DisableBkUpAccess();
}

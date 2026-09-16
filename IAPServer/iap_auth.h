/*
 * iap_auth.h
 *
 * ECDSA challenge-response for the network IAP commands that can change
 * device state (flash a new image, force a reboot into bootloader mode).
 * Without this, anyone who can reach the TCP/UDP port could issue those
 * commands with no proof of authorization at all.
 *
 * The board keeps no secret at all for this -- only public keys and
 * certificates. Why that shape was chosen: $PROD/docs/security/OWNERSHIP.md.
 *
 * Protocol: client requests a challenge, device replies with a nonce that
 * can only ever be used once and expires after IAP_AUTH_NONCE_TTL_MS; client
 * presents a certificate (see iap_cert.h) and signs sha256(nonce || msg) with
 * the certificate's leaf private key, where `msg` is the exact command it
 * wants authorized. The device checks the certificate is vouched for by the
 * root it currently trusts, then checks the signature against the
 * certificate's leaf public key -- both steps, not just one; a valid
 * certificate says nothing about who signed *this* nonce, and a valid
 * signature under some leaf key says nothing if that leaf was never actually
 * certified.
 */

#ifndef IAPSERVER_IAP_AUTH_H_
#define IAPSERVER_IAP_AUTH_H_

#include <stdint.h>
#include <stdbool.h>
#include "iap_cert.h"

#ifdef __cplusplus
extern "C" {
#endif

#define IAP_AUTH_NONCE_SIZE   16U
#define IAP_AUTH_NONCE_TTL_MS 30000U

/* Issues a fresh, never-repeating nonce and hex-encodes it into out_hex
 * (caller must provide at least IAP_AUTH_NONCE_SIZE*2 + 1 bytes). */
void iap_auth_issue_challenge(char *out_hex);

/*
 * Checks nonce_sig against the most recently issued nonce, under the leaf key
 * named by `cert` -- once `cert` itself has been checked against the root
 * this board currently trusts. The nonce is consumed (one-shot) regardless of
 * the result, so a captured (nonce, signature) pair can never be replayed.
 *
 * `cert` is not itself secret or session-specific -- only nonce_sig needs
 * freshness -- so it is verified independently of `msg`/the nonce, not folded
 * into the signed message.
 */
bool iap_auth_verify_and_consume(const uint8_t *msg, uint32_t msg_len,
		const iap_cert_t *cert, const uint8_t nonce_sig[64]);

/* Current value of the persistent challenge counter (incremented once per
 * iap_auth_issue_challenge() call, survives reset). Exposed only so callers
 * can attach "which challenge attempt this event corresponds to" to audit
 * log entries -- it is not secret and is not part of any security check. */
uint32_t iap_auth_get_counter(void);

/* Reports at boot whether the VBAT-backed domain survived the last power-off.
 * Call once, after MX_RTC_Init(). */
void iap_auth_report_backup_domain(void);

#ifdef __cplusplus
}
#endif

#endif /* IAPSERVER_IAP_AUTH_H_ */

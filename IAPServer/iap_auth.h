/*
 * iap_auth.h
 *
 * HMAC-SHA256 challenge-response for the network IAP commands that can
 * change device state (flash a new image, force a reboot into bootloader
 * mode). Without this, anyone who can reach the TCP/UDP port could issue
 * those commands with no proof of authorization at all.
 *
 * The key itself is per-device: see iap_keyderive.h. It is derived from a
 * fixed shared password mixed with this device's machine ID (UID), so every
 * device authenticates with a different effective key even though the same
 * password is embedded in every firmware image. See IAPServer/keys/README.md
 * for provisioning notes.
 *
 * Protocol: client requests a challenge, device replies with a nonce that
 * can only ever be used once and expires after IAP_AUTH_NONCE_TTL_MS; client
 * proves it holds the device key by sending HMAC-SHA256(key, nonce || msg)
 * where `msg` is the exact command it wants authorized.
 */

#ifndef IAPSERVER_IAP_AUTH_H_
#define IAPSERVER_IAP_AUTH_H_

#include <stdint.h>
#include <stdbool.h>

#ifdef __cplusplus
extern "C" {
#endif

#define IAP_AUTH_NONCE_SIZE   16U
#define IAP_AUTH_HMAC_SIZE    32U
#define IAP_AUTH_NONCE_TTL_MS 30000U

/* Issues a fresh, never-repeating nonce and hex-encodes it into out_hex
 * (caller must provide at least IAP_AUTH_NONCE_SIZE*2 + 1 bytes). */
void iap_auth_issue_challenge(char *out_hex);

/* Checks hmac against HMAC-SHA256(key, nonce || msg) for the most recently
 * issued nonce. The nonce is consumed (one-shot) regardless of the result,
 * so a captured (nonce, hmac) pair can never be replayed. */
bool iap_auth_verify_and_consume(const uint8_t *msg, uint32_t msg_len, const uint8_t hmac[IAP_AUTH_HMAC_SIZE]);

/* Current value of the persistent challenge counter (incremented once per
 * iap_auth_issue_challenge() call, survives reset). Exposed only so callers
 * can attach "which challenge attempt this event corresponds to" to audit
 * log entries -- it is not secret and is not part of any security check. */
uint32_t iap_auth_get_counter(void);

#ifdef __cplusplus
}
#endif

#endif /* IAPSERVER_IAP_AUTH_H_ */

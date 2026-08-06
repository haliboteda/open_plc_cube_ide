/*
 * fw_verify.h
 *
 * ECDSA (secp256r1) signature verification for boot-time firmware
 * integrity checking. Thin wrapper around the vendored micro-ecc library
 * (IAPServer/uecc/) + the embedded public key (fw_pubkey.h).
 */

#ifndef IAPSERVER_FW_VERIFY_H_
#define IAPSERVER_FW_VERIFY_H_

#include <stdint.h>
#include <stdbool.h>

#ifdef __cplusplus
extern "C" {
#endif

#define FW_PUBLIC_KEY_SIZE 64U /* secp256r1 uncompressed point, X||Y, no 0x04 prefix */
#define FW_SIGNATURE_SIZE  64U /* secp256r1 signature, r||s */

/* Verifies an ECDSA signature over a SHA-256 hash using the embedded
 * release public key (see fw_pubkey.c). Returns true only if the
 * signature is valid for that exact hash. */
bool fw_verify_signature(const uint8_t hash[32], const uint8_t signature[FW_SIGNATURE_SIZE]);

#ifdef __cplusplus
}
#endif

#endif /* IAPSERVER_FW_VERIFY_H_ */

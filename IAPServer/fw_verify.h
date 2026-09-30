/*
 * fw_verify.h
 *
 * ECDSA (secp256r1) signature verification for boot-time firmware
 * integrity checking. Thin wrapper around the vendored micro-ecc library
 * (IAPServer/uecc/).
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

/*
 * Verify against the root this board currently trusts (owner_slot_root()).
 * Always false on a board with no root.
 */
bool fw_verify_signature(const uint8_t hash[32], const uint8_t signature[FW_SIGNATURE_SIZE]);

/*
 * Verify against a specific key. For checking a chain link against the root
 * that came before it, where "the root this board trusts" is the question
 * being answered rather than the input.
 */
bool fw_verify_signature_with_key(const uint8_t pubkey[64],
		const uint8_t hash[32], const uint8_t signature[FW_SIGNATURE_SIZE]);

#ifdef __cplusplus
}
#endif

#endif /* IAPSERVER_FW_VERIFY_H_ */

/*
 * fw_verify.c
 *
 * See fw_verify.h. Uses the vendored micro-ecc (uECC_verify).
 */

#include "fw_verify.h"
#include "owner_slot.h"
#include "uecc/uECC.h"
#include <stddef.h>

bool fw_verify_signature_with_key(const uint8_t pubkey[64],
		const uint8_t hash[32], const uint8_t signature[FW_SIGNATURE_SIZE])
{
	return uECC_verify(pubkey, hash, 32U, signature, uECC_secp256r1()) == 1;
}

bool fw_verify_signature(const uint8_t hash[32], const uint8_t signature[FW_SIGNATURE_SIZE])
{
	/* A board with no root verifies nothing (decision 72). */
	const uint8_t *root = owner_slot_root();

	return (root != NULL) && fw_verify_signature_with_key(root, hash, signature);
}

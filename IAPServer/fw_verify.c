/*
 * fw_verify.c
 *
 * See fw_verify.h. Uses the vendored micro-ecc (uECC_verify) with the
 * embedded release public key from fw_pubkey.c.
 */

#include "fw_verify.h"
#include "fw_pubkey.h"
#include "uecc/uECC.h"

bool fw_verify_signature(const uint8_t hash[32], const uint8_t signature[FW_SIGNATURE_SIZE])
{
	return uECC_verify(fw_public_key, hash, 32U, signature, uECC_secp256r1()) == 1;
}

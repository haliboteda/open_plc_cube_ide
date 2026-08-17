/*
 * fw_verify.c
 *
 * See fw_verify.h. Uses the vendored micro-ecc (uECC_verify).
 */

#include "fw_verify.h"
#include "fw_pubkey.h"
#include "owner_slot.h"
#include "uecc/uECC.h"

bool fw_verify_signature_with_key(const uint8_t pubkey[64],
		const uint8_t hash[32], const uint8_t signature[FW_SIGNATURE_SIZE])
{
	return uECC_verify(pubkey, hash, 32U, signature, uECC_secp256r1()) == 1;
}

bool fw_verify_signature(const uint8_t hash[32], const uint8_t signature[FW_SIGNATURE_SIZE])
{
	/*
	 * owner_slot_root(), not fw_public_key.
	 *
	 * On a claimed board those differ, and verifying against the built-in key
	 * would make claiming pure theatre: the boot log would announce "firmware
	 * must be signed by that owner" while the board went on running anything
	 * signed with the published key -- the key everybody has. Requirement C10
	 * is about what actually runs, not about what the log says.
	 *
	 * Caught by the M1 step 4 acceptance: after claiming, an application signed
	 * with the OLD key still booted. Nothing else would have noticed, because
	 * every other symptom looked correct.
	 *
	 * On an unclaimed board owner_slot_root() returns fw_public_key, so this is
	 * the previous behaviour unchanged.
	 */
	return fw_verify_signature_with_key(owner_slot_root(), hash, signature);
}

/*
 * iap_cert.c -- see iap_cert.h.
 */

#include "iap_cert.h"
#include "fw_verify.h"
#include "sha256.h"

bool iap_cert_verify(const iap_cert_t *cert, const uint8_t root[64])
{
	uint8_t digest[SHA256_DIGEST_SIZE];

	sha256((const uint8_t *)cert, IAP_CERT_SIGNED_LEN, digest);
	return fw_verify_signature_with_key(root, digest, cert->root_sig);
}

bool iap_cert_verify_image(const uint8_t hash[32], const uint8_t signature[64],
		const iap_cert_t *cert, const uint8_t root[64])
{
	if (!iap_cert_verify(cert, root)) {
		return false;
	}
	return fw_verify_signature_with_key(cert->leaf_pubkey, hash, signature);
}

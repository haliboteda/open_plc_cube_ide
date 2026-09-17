/*
 * iap_cert.h -- leaf certificates: "this key is authorised to sign firmware
 * (and session-auth challenges) on behalf of a trusted root".
 *
 * Requirement C11. Design: $PROD/docs/modules/M2-ownership.md.
 *
 * A board no longer asks "was this signed by the root directly" -- it asks
 * "was this signed by a leaf whose certificate the root vouches for". Simple
 * mode (one key doing everything) is not a special case of that question: the
 * tool self-signs a certificate whose leaf_pubkey equals the root's own
 * pubkey, and it goes through exactly the same two-step check as a delegated
 * one. There is no self-signed branch in this file, on purpose -- a branch
 * nobody exercises in the common case is a branch that quietly stops working.
 *
 * Deliberately independent of owner_slot.h: this module takes the trusted
 * root as a plain argument rather than calling owner_slot_root() itself, so
 * the exact same code can be compiled, unmodified, into a context that has no
 * owner-slot flash to read -- see open_plc_arduino's owner_root_ro.c, which
 * gets the root a different way and hands it to the same functions here.
 */

#ifndef IAPSERVER_IAP_CERT_H_
#define IAPSERVER_IAP_CERT_H_

#include <stdint.h>
#include <stdbool.h>

#ifdef __cplusplus
extern "C" {
#endif

/* leaf_pubkey[64] || serial(4, little-endian) || root_sig[64]. No parser --
 * three fields at fixed offsets, same style as owner_record_t. */
#define IAP_CERT_SIZE       132U
#define IAP_CERT_SIGNED_LEN  68U   /* leaf_pubkey||serial, covered by root_sig */

typedef struct {
	uint8_t  leaf_pubkey[64]; /*  0  secp256r1 X||Y */
	uint32_t serial;          /* 64  little-endian; tool-assigned, from a local
	                            *     counter kept next to the root private key.
	                            *     Only meaningful once C12 (revocation) exists
	                            *     -- it is what a revocation list would name. */
	uint8_t  root_sig[64];    /* 68  root's signature over sha256(bytes[0,68)) */
} iap_cert_t;

_Static_assert(sizeof(iap_cert_t) == IAP_CERT_SIZE,
		"iap_cert_t must be exactly 132 bytes (64 + 4 + 64, no padding)");

/*
 * Is this certificate actually vouched for by `root`?
 *
 * Checks root_sig over sha256(cert bytes [0, IAP_CERT_SIGNED_LEN)) using
 * `root`. Says nothing about the image the certificate will go on to
 * authorise -- that is iap_cert_verify_image()'s job, a separate step,
 * because a caller that only needs to know "is this leaf currently
 * authorised" (session auth) should not also have to supply an image hash.
 */
bool iap_cert_verify(const iap_cert_t *cert, const uint8_t root[64]);

/*
 * The two-step check a firmware image needs: the certificate is vouched for
 * by `root`, AND `signature` verifies over `hash` under the certificate's
 * leaf_pubkey. Both must hold -- a valid certificate says nothing about who
 * signed a *particular* image, and a valid image signature under some leaf
 * key says nothing if that leaf was never actually certified.
 */
bool iap_cert_verify_image(const uint8_t hash[32], const uint8_t signature[64],
		const iap_cert_t *cert, const uint8_t root[64]);

#ifdef __cplusplus
}
#endif

#endif /* IAPSERVER_IAP_CERT_H_ */

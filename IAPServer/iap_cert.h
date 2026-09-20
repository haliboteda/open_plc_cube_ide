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
 *
 * Revocation (2026-09-20) works the same way: `leaf_is_revoked` is a plain
 * bool the caller supplies, computed however that caller's environment
 * answers "has this leaf been revoked" (owner_slot_is_revoked() in the
 * bootloader; open_plc_arduino's mirror of the same on-flash records
 * elsewhere). This file never asks the question itself, on purpose -- adding
 * the parameter rather than calling out to owner_slot.h keeps the
 * independence above intact, AND makes a call site that forgets to check
 * revocation a compile error instead of a silent gap.
 */

#ifndef IAPSERVER_IAP_CERT_H_
#define IAPSERVER_IAP_CERT_H_

#include <stdint.h>
#include <stdbool.h>

#ifdef __cplusplus
extern "C" {
#endif

/* leaf_pubkey[64] || root_sig[64]. No parser -- two fields at fixed offsets,
 * same style as owner_record_t.
 *
 * 2026-09-20: dropped the `serial` field this used to carry. It existed so a
 * revocation list would have something to name, but revocation ended up
 * naming leaves by their public key instead (see
 * $PROD/maps/owner-revoke-and-boot-upgrade/issues/OWN-01-revoke-by-serial-or-by-pubkey.md)
 * -- a scheme that needs no board-assigned number, and works even if the tool
 * that issued a certificate is never seen again. `serial` never did anything
 * a verifier checked; it was signed and stored, never compared. */
#define IAP_CERT_SIZE        128U
#define IAP_CERT_SIGNED_LEN   64U   /* leaf_pubkey, covered by root_sig */

typedef struct {
	uint8_t  leaf_pubkey[64]; /*  0  secp256r1 X||Y */
	uint8_t  root_sig[64];    /* 64  root's signature over sha256(bytes[0,64)) */
} iap_cert_t;

_Static_assert(sizeof(iap_cert_t) == IAP_CERT_SIZE,
		"iap_cert_t must be exactly 128 bytes (64 + 64, no padding)");

/*
 * Is this certificate actually vouched for by `root`, and has nobody revoked
 * the leaf it names?
 *
 * Checks root_sig over sha256(cert bytes [0, IAP_CERT_SIGNED_LEN)) using
 * `root`, AND that `leaf_is_revoked` is false. Says nothing about the image
 * the certificate will go on to authorise -- that is iap_cert_verify_image()'s
 * job, a separate step, because a caller that only needs to know "is this
 * leaf currently authorised" (session auth) should not also have to supply an
 * image hash.
 *
 * `leaf_is_revoked` is the caller's answer, not this file's -- see the header
 * comment above for why.
 */
bool iap_cert_verify(const iap_cert_t *cert, const uint8_t root[64], bool leaf_is_revoked);

/*
 * The two-step check a firmware image needs: the certificate is vouched for
 * by `root` (and not revoked), AND `signature` verifies over `hash` under the
 * certificate's leaf_pubkey. Both must hold -- a valid certificate says
 * nothing about who signed a *particular* image, and a valid image signature
 * under some leaf key says nothing if that leaf was never actually certified
 * or has since been revoked.
 */
bool iap_cert_verify_image(const uint8_t hash[32], const uint8_t signature[64],
		const iap_cert_t *cert, const uint8_t root[64], bool leaf_is_revoked);

#ifdef __cplusplus
}
#endif

#endif /* IAPSERVER_IAP_CERT_H_ */

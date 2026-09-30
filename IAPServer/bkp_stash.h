/*
 * bkp_stash.h -- one staged copy in backup SRAM, kept alive by VBAT.
 *
 * Holds what a sector-15 reclaim must not lose while the sector is erased
 * (root area and latest metadata). Survives a main-power cut only with the
 * backup regulator on (PWR_CR2.BREN), which bkp_stash_enable() turns on.
 * Sources: $PROD/maps/root-key-without-bootloader-reflash/ROOT-05-findings.md.
 * Ownership of the region: $PROD/docs/repo/ARCHITECTURE.md.
 */

#ifndef IAPSERVER_BKP_STASH_H_
#define IAPSERVER_BKP_STASH_H_

#include <stdint.h>
#include <stdbool.h>

/* Largest payload that fits behind the header in the 4 KiB region. */
#define BKP_STASH_MAX_PAYLOAD  (4096U - 64U)

/* Clock, write access and backup regulator. Idempotent. Returns false if the
 * regulator never reports ready -- a stash written then would not survive. */
bool bkp_stash_enable(void);

/* Store `len` bytes, bound to this board's UID and a SHA-256 of both, and
 * read it back. Returns false if it does not verify. */
bool bkp_stash_save(const void *payload, uint32_t len);

/* True and fills `out` only if a stash of exactly `len` bytes is present,
 * intact, and was written by this board. */
bool bkp_stash_load(void *out, uint32_t len);

void bkp_stash_clear(void);

#endif /* IAPSERVER_BKP_STASH_H_ */

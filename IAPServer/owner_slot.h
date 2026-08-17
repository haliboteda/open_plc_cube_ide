/*
 * owner_slot.h -- which public key this board trusts as its signing root.
 *
 * Requirement C10. Design: docs/OWNERSHIP.md. Plan: docs/handover/Todo/M1-owner-slot.md.
 *
 * An append-only record area in the top 8K of the bootloader's own flash
 * sector, reserved by STM32H743IIKX_FLASH.ld (FLASH LENGTH is 120K, not the
 * 128K of the sector, so the linker cannot place anything here). Empty means
 * the board falls back to the root compiled into fw_pubkey.c.
 *
 * Why this area and not the state sector: journal_reclaim() erases the state
 * sector whole. Copying owner records out and back would open a window the
 * width of an erase, and losing them there would silently return the board to
 * the factory root -- the one failure this must not have.
 *
 * ---------------------------------------------------------------------------
 * WHAT IS IMPLEMENTED SO FAR (step 2 of 6)
 * ---------------------------------------------------------------------------
 * Reading and structural parsing only. Nothing here writes flash.
 *
 * owner_slot_root() deliberately returns the compiled-in root even when a
 * record is present, because verifying a record's prev_sig is step 5 and does
 * not exist yet. Honouring an unverified record would mean any code that can
 * write flash could hand itself the trust root -- strictly worse than not
 * having the feature. It logs loudly instead.
 */

#ifndef IAPSERVER_OWNER_SLOT_H_
#define IAPSERVER_OWNER_SLOT_H_

#include <stdint.h>
#include <stdbool.h>

/* Top 8K of the bootloader sector. Must agree with FLASH LENGTH in
 * STM32H743IIKX_FLASH.ld: that script grants the linker 0x08000000..0x0801DFFF
 * and this area starts where it stops. */
#define OWNER_SLOT_BASE        0x0801E000UL
#define OWNER_SLOT_SIZE        (8U * 1024U)

/* 160 bytes = 5 x 32-byte flash words. The H7 programs a 256-bit word at a
 * time, so a record that is not a whole number of them cannot be appended
 * without a read-modify-write of a neighbour. */
#define OWNER_RECORD_SIZE      160U
#define OWNER_SLOT_MAX_RECORDS (OWNER_SLOT_SIZE / OWNER_RECORD_SIZE)   /* 51 */

#define OWNER_RECORD_TYPE      'O'
#define OWNER_RECORD_ERASED    0xFFU
#define OWNER_FORMAT_VER       1U
#define OWNER_RECORD_SLOTS     5U

/* flags */
#define OWNER_FLAG_CLEARED     0x00000001UL   /* factory reset: fall back to R0 */

/* Layout is fixed by docs/OWNERSHIP.md and locked by a _Static_assert in the
 * .c file. format_ver exists from the first version on purpose, so a later
 * format change is an upgrade rather than a breaking migration. */
typedef struct {
	uint8_t  type;             /*   0  'O', or 0xFF when erased               */
	uint8_t  slots;            /*   1  5                                       */
	uint16_t format_ver;       /*   2  1                                       */
	uint32_t generation;       /*   4  monotonic; highest valid record wins     */
	uint32_t flags;            /*   8  bit0 = cleared                          */
	uint8_t  root_pubkey[64];  /*  12  secp256r1 X||Y; all zero when cleared    */
	uint8_t  prev_sig[64];     /*  76  previous root's signature over bytes 0..75
	                            *      all zero for the first claim and for a
	                            *      cleared record -- those are gated by a
	                            *      physical action, not by a signature      */
	uint8_t  reserved[20];     /* 140                                          */
} owner_record_t;

/* Scan the area. Read-only; safe to call before anything else is up. */
void owner_slot_init(void);

/*
 * The root to verify firmware against.
 *
 * ⚠️ Until step 5 lands this always returns the compiled-in root. See the note
 * at the top of this file: trusting an unverified record would be worse than
 * having no owner slot at all.
 */
const uint8_t *owner_slot_root(void);

/* True when the area holds no record at all (a factory board). */
bool owner_slot_is_empty(void);

/* Number of structurally valid records found. */
uint32_t owner_slot_record_count(void);

/* One boot line describing what was found. */
void owner_slot_report(void);

#endif /* IAPSERVER_OWNER_SLOT_H_ */

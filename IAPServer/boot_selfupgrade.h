/*
 * boot_selfupgrade.h -- replace the bootloader in place, from RAM.
 *
 * Requirements R1-34..R1-37. Design: $PROD/docs/modules/M1/FLASHBOOT.md.
 *
 * The image is already staged and verified in SDRAM when this is called; all
 * that is left is the part that cannot run from flash, because the code doing
 * the erase lives in the sector being erased.
 */

#ifndef IAPSERVER_BOOT_SELFUPGRADE_H_
#define IAPSERVER_BOOT_SELFUPGRADE_H_

#include <stdint.h>
#include <stdbool.h>

/* Largest bootloader image this accepts: the whole of sector 0, the same bound
 * the linker script gives the FLASH region. */
uint32_t boot_selfupgrade_max_size(void);

/*
 * Erase sector 0 and write the staged image over it.
 *
 * ⚠️ On success this never returns -- it resets the board, because the code
 * that called it no longer exists at the address it returns to. It also
 * resets after a failure that happens at or after the erase, for the same
 * reason; the board then comes up dead and needs an ST-Link reflash.
 *
 * It returns false only for the failures it can detect before erasing
 * anything: a bad size, or a flash that will not unlock. The caller is still
 * intact then and can answer on the wire.
 */
bool boot_selfupgrade_commit(uint32_t image_size);

#endif /* IAPSERVER_BOOT_SELFUPGRADE_H_ */

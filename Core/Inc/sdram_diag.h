/*
 * SDRAM data-bus diagnostics.
 *
 * Exists because the SDRAM data bus on this board has no probe point: both
 * ends of every DQ net sit under a BGA (MCU U1 UFBGA176 0.65 mm, SDRAM U6
 * 54-ball FBGA), there is no series resistor and no test pad anywhere on the
 * bus. A multimeter and a scope have nowhere to land, so a bus fault can only
 * be characterised from inside the MCU. See docs/design/HARDWARE-FACTS.md.
 *
 * Nothing here changes behaviour: it writes only inside the staging area, which
 * is scratch, and restores every register it touches.
 */
#ifndef SDRAM_DIAG_H
#define SDRAM_DIAG_H

#include <stdint.h>

/*
 * The on-demand half of this module -- the four-measurement report and the
 * live error-rate loop, reachable as the serial commands "sdramdiag" and
 * "sdramlive" -- is compiled out by default.
 *
 * Two reasons, and the second is the one that matters:
 *
 *   1. Nobody needs it while the SDRAM investigation is parked.
 *   2. process_command() serves TCP and USB-CDC as well as the console, and
 *      neither command is authenticated. "sdramlive 600" would stall the
 *      superloop for ten minutes: no discovery replies, no uploads. A
 *      diagnostic for someone standing at the bench should not be a denial of
 *      service reachable from the network.
 *
 * Set this to 1 to bring both commands back -- that is what the next board is
 * for, since comparing a healthy board against 2026-08-21's capture is how we
 * find out whether the D1 fault is this board's or the design's. If it comes
 * back for good, gate the commands behind the same HMAC challenge the "flash"
 * command uses, or accept them from the console only.
 *
 * The boot-time summary below is NOT behind this flag: it is not reachable from
 * anywhere, costs nothing on a healthy board, and is the whole reason a failing
 * board can say which data line is bad.
 */
#ifndef IAP_SDRAM_DIAG_COMMANDS
#define IAP_SDRAM_DIAG_COMMANDS 0
#endif

/*
 * One line, printed at boot only when the SDRAM self-test failed. Names which
 * data bit is bad and how bad, so a failing board says what is wrong without
 * anyone having to run anything.
 */
void iap_sdram_diag_boot_summary(void);

#if IAP_SDRAM_DIAG_COMMANDS

/*
 * The full battery, on demand (serial command "sdramdiag"):
 *   1. per-bit error census, and the written-0 / written-1 split
 *   2. dwell sweep -- error rate against how long the level is held
 *   3. release-time comparison across all 16 DQ lines
 *   4. float test -- released with no pull, does the line hold or drift
 * Takes a few seconds and prints tables. 3 and 4 only mean something together:
 * see the comment above report_float() in sdram_diag.c.
 */
void iap_sdram_diag_full_report(void);

/*
 * Prints one error-rate line per second for `seconds` (serial command
 * "sdramlive"). For the freeze-spray / hot-air test: someone cools or heats one
 * package while watching whether the rate moves.
 */
void iap_sdram_diag_live(uint32_t seconds);

#endif /* IAP_SDRAM_DIAG_COMMANDS */

#endif /* SDRAM_DIAG_H */

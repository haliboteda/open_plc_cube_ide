// porttool_handover.h
//
// Hands the board over to one of the standalone bring-up entries.
//
// Those entries (CAN's five phases, KNX's bit-timing analysis, RS485's D1/D2/D3
// diagnostics) are deep troubleshooting tools, not routine port checks. They
// own the CPU and do not come back. Rather than reshape them into sessions -
// they are large, timing-sensitive state machines - the tool simply gives them
// the board when asked.
//
// *** Handing over is one-way. The board runs that entry until it is reset.
// *** That is why it is a separate command and not a session.
//
// This is what replaced the pile of X_TEST_ENABLE blocks in main.c: one image
// now carries every entry instead of one image per entry.

#ifndef TESTCASE_PORTTOOL_PORTTOOL_HANDOVER_H_
#define TESTCASE_PORTTOOL_PORTTOOL_HANDOVER_H_

#include <stdint.h>

/* Runs the named entry. Returns 0 if there is no such target. Normally does
 * not return at all; if the entry does hand control back, this returns 1 so
 * the caller can say so rather than pretend the board is still testing. */
int PortTool_Handover(const char *name);

/* Prints the available targets, one "OK handover=<name>" line each. */
void PortTool_HandoverList(void);

/* Contributes the handover ports to pt.caps: one "OK port=... kind=handover"
 * line per group of targets that share hardware, listing the group's variants
 * in targets=. Returns how many lines that is; with `emit` 0 it only counts,
 * which is how pt.caps knows its own length before printing the header. */
uint32_t PortTool_HandoverCaps(int emit);

/* Writes the handover targets that belong to `port` as "a,b,c" and returns how
 * many there were.
 *
 * A port with both a row of its own and a deep handover entry - rs485's
 * session, sdram's kind=run row - is one piece of hardware, so pt.caps reports
 * it once and lists the entries beside it rather than inventing a second port
 * with the same name. */
uint32_t PortTool_HandoverTargetsFor(const char *port, char *out, uint32_t out_len);

#endif /* TESTCASE_PORTTOOL_PORTTOOL_HANDOVER_H_ */

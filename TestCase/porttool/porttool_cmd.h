// porttool_cmd.h
//
// Parsing for one command line: "pt.start din ch=1,3,5 period=200".
//
// Values may be quoted when they contain spaces:
//   pt.set rs485 text="T={temp1} #{seq}" period=3000

#ifndef TESTCASE_PORTTOOL_PORTTOOL_CMD_H_
#define TESTCASE_PORTTOOL_PORTTOOL_CMD_H_

#include <stdint.h>

/* Copies the value of `key` into `out`. Returns 0 when the key is absent, so
 * a caller can tell "not given" from "given as empty". */
int PortCmd_GetStr(const char *args, const char *key, char *out, uint32_t out_len);

/* Returns 0 when the key is absent or the value is not a plain number. */
int PortCmd_GetU32(const char *args, const char *key, uint32_t *out);

/* Parses "1,3,5" into a bitmask with bit 0 = channel 1. Returns 0 when the key
 * is absent or any channel is outside 1..max_ch - a typo must not silently
 * become a different set of channels. */
int PortCmd_GetMask(const char *args, const char *key, uint32_t max_ch, uint32_t *mask);

/* Renders a channel bitmask back as "1,3,5" for replies and caps lines. */
void PortCmd_FormatMask(uint32_t mask, uint32_t max_ch, char *out, uint32_t out_len);

/* Parses "1:20,3:50" - a value per named channel - into vals[0..max_ch-1],
 * and reports which channels were named in `touched`. Channels not named keep
 * whatever they held.
 *
 * Returns 0 for anything malformed: a channel outside 1..max_ch, a value above
 * val_max, a missing half, or the same channel twice. Nothing is written to
 * vals in that case, so a rejected parameter cannot half-apply.
 *
 * This is what lets a port be driven per channel rather than all together -
 * dout's duty= and relay's on= both use it. */
int PortCmd_GetPairs(const char *args, const char *key, uint32_t max_ch,
                     uint32_t val_max, uint32_t *vals, uint32_t *touched);

/* Renders the channels in `mask` back as "1:20,3:50" for caps lines. */
void PortCmd_FormatPairs(const uint32_t *vals, uint32_t mask, uint32_t max_ch,
                         char *out, uint32_t out_len);

/* Splits the first whitespace-delimited word off `line`, leaving `rest`
 * pointing at whatever follows it. */
const char *PortCmd_Word(const char *line, char *out, uint32_t out_len, const char **rest);

#endif /* TESTCASE_PORTTOOL_PORTTOOL_CMD_H_ */

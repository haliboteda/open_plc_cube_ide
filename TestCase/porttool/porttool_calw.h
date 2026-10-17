// porttool_calw.h
//
// pt.calw: the fixture firmware writes the calibration area into flash
// sector 15 itself (decision 142). The only flash this image ever erases or
// programs. Protocol: $PROD/docs/modules/M4/PORTTOOL-FLOW.md A.3, A.4.

#ifndef TESTCASE_PORTTOOL_PORTTOOL_CALW_H_
#define TESTCASE_PORTTOOL_PORTTOOL_CALW_H_

#include <stdint.h>

#include "calib_area.h"
#include "main.h"   /* before the fallback: the host HAL stub defines the base */

/* The host harness points this at an array standing in for sector 15; on the
 * board it is the address calib_area.h owns, which is the sector's start. */
#ifndef PORTTOOL_CALIB_BASE
#define PORTTOOL_CALIB_BASE CALIB_AREA_ADDR
#endif

/* pt.calw <112 hex digits>: validates the 56-byte record, rewrites sector 15
 * with it and prints one OK or ERR line. Blocks for the sector erase. */
void PortTool_CalWrite(const char *args);

/* Programs one 32-byte flash word. Returns 0 without touching flash unless
 * dst is a word-aligned address inside sector 15. */
int PortTool_S15Program(uintptr_t dst, const uint8_t *src);

#endif /* TESTCASE_PORTTOOL_PORTTOOL_CALW_H_ */

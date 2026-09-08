// port_can.h
//
// The FDCAN1 hardware, with no opinion about what it is used for.
//
// Everything here returns. The bring-up test in TestCase/CAN/ owns the
// choreography - phases, diagnosis, the printed narrative - and the port tool
// session owns a non-blocking one; both drive the same peripheral through
// this, so the two cannot configure it differently and then disagree about
// what the bus did.
//
// Hardware (Upper Deck): PB9 = FDCAN1_TX, PI9 = FDCAN1_RX, AF9. Transceiver is
// an ISO1044, which is isolated and has no silent/standby pin - listen-only is
// the controller's bus-monitoring mode, not a transceiver setting. The
// isolated side is powered by U7; if U7 is dead the controller looks healthy
// and the bus does nothing. Terminal C07/C08. Details and sources in
// ../../docs/design/HARDWARE-FACTS.md.
//
// *** The kernel clock is HSE. *** The bit timings below are exact only for
// HSE, and the reset default happens to be HSE too - but PortCan_Open()
// selects it explicitly rather than relying on that.

// *** INCLUDE THIS BEFORE main.h. ***
//
// The FDCAN HAL driver is a private copy in TestCase/common, not a project-wide
// module - stm32h7xx_hal_conf.h has HAL_FDCAN_MODULE_ENABLED commented out
// because FDCAN is not in the .ioc. The define below is what makes hal_conf
// pull in the local header, and it only works if it lands before main.h drags
// stm32h7xx_hal.h in. A file that includes main.h first gets no
// FDCAN_HandleTypeDef and a screenful of unknown-type errors.
//
// Background, and what to do the day FDCAN becomes a real CubeMX peripheral:
// testcase_hal_guard.h, which is included here and fails the build with
// instructions if that day has come.

#ifndef TESTCASE_COMMON_PORT_CAN_H_
#define TESTCASE_COMMON_PORT_CAN_H_

#include "testcase_hal_guard.h"
#ifndef HAL_FDCAN_MODULE_ENABLED
#define HAL_FDCAN_MODULE_ENABLED
#endif

#include "main.h"
#include <stdint.h>

/* The rates the timing table covers, in the order PortCan_RateBps indexes. */
#define PORT_CAN_RATE_COUNT 4u
#define PORT_CAN_RATE_DEFAULT 2u   /* 500 kbit/s */

/* FDCAN_ENDN reads this on a live peripheral. A different value means the
 * kernel clock never arrived, which looks exactly like a dead bus. */
#define PORT_CAN_ENDN_EXPECT 0x87654321u

uint32_t PortCan_RateBps(uint8_t index);

/* The DLC code for a payload length, for a caller building its own header -
 * the echo phase answers on whatever ID type came in, which PortCan_Send does
 * not do. */
uint32_t PortCan_Dlc(uint8_t len);

/* Finds the index for a rate in bit/s. 0 when the table has no such rate. */
int PortCan_RateIndex(uint32_t bps, uint8_t *out_index);

/* The segment values behind a rate, for a report that wants to show the bit
 * rate the hardware will actually produce rather than the one asked for. Any
 * out pointer may be NULL. 0 when the index is out of range. */
int PortCan_Timing(uint8_t index, uint16_t *prescaler, uint16_t *seg1,
                   uint16_t *seg2, uint16_t *sjw);

/* Kernel clock and pins. Safe to call more than once. */
int PortCan_Init(void);

/* mode is one of the HAL's FDCAN_MODE_* values. auto_retx off is what a lone
 * node wants: with no second node to acknowledge, retrying forever buries the
 * symptom instead of reporting it. */
int PortCan_Open(uint8_t rate_index, uint32_t mode, int auto_retx);
void PortCan_Close(void);

int PortCan_Send(uint32_t id, const uint8_t *data, uint8_t len);

/* One frame, or 0 when the FIFO is empty. Never blocks. */
int PortCan_Receive(uint32_t *id, uint8_t *data, uint8_t *len);

void PortCan_Counters(uint32_t *tec, uint32_t *rec);

/* PSR.LEC is reset-on-read: reading it here clears it, so a caller that wants
 * to report and then diagnose has to keep the value it got. */
uint32_t PortCan_LastError(void);

int PortCan_Alive(void);
uint32_t PortCan_ClockHz(void);
const char *PortCan_ClockName(void);

/* The handle, for the bring-up test's richer status readout. Nothing new
 * should reach for this: it exists so the existing diagnosis code could move
 * across unchanged rather than being rewritten in the same commit that
 * extracted the driver. */
FDCAN_HandleTypeDef *PortCan_Handle(void);

#endif /* TESTCASE_COMMON_PORT_CAN_H_ */

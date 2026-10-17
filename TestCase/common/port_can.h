// port_can.h
//
// The FDCAN1 hardware, with no opinion about what it is used for.
//
// Everything here returns. The port tool's can session (porttool_can.c) owns
// the choreography.
//
// Transceiver ISO1044 (isolated, no silent pin: listen-only is the controller's
// bus-monitoring mode), pins and terminals in HARDWARE-FACTS.md CAN.
//
// The kernel clock is HSE, selected explicitly: the bit timings are exact only
// for HSE (DECISIONS.md 124).
//
// INCLUDE THIS BEFORE main.h: it enables the local FDCAN HAL copy, which only
// works before stm32h7xx_hal.h is pulled in (testcase_hal_guard.h).

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

/* FDCAN_ENDN on a clocked peripheral. */
#define PORT_CAN_ENDN_EXPECT 0x87654321u

uint32_t PortCan_RateBps(uint8_t index);

/* The DLC code for a payload length. */
uint32_t PortCan_Dlc(uint8_t len);

/* Finds the index for a rate in bit/s. 0 when the table has no such rate. */
int PortCan_RateIndex(uint32_t bps, uint8_t *out_index);

/* The segment values behind a rate, for pt.caps info=. Any out pointer may be
 * NULL. 0 when the index is out of range. */
int PortCan_Timing(uint8_t index, uint16_t *prescaler, uint16_t *seg1,
                   uint16_t *seg2, uint16_t *sjw);

/* Kernel clock and pins. Safe to call more than once. */
int PortCan_Init(void);

/* mode is one of the HAL's FDCAN_MODE_* values. auto_retx off suits a lone
 * node: retrying forever would hide a missing acknowledge. */
int PortCan_Open(uint8_t rate_index, uint32_t mode, int auto_retx);
void PortCan_Close(void);

int PortCan_Send(uint32_t id, const uint8_t *data, uint8_t len);

/* One frame, or 0 when the FIFO is empty. Never blocks. */
int PortCan_Receive(uint32_t *id, uint8_t *data, uint8_t *len);

void PortCan_Counters(uint32_t *tec, uint32_t *rec);

/* PSR.LEC is reset-on-read: keep the value. */
uint32_t PortCan_LastError(void);

/* The controller's error state from PSR: PORT_CAN_STATE_*. PSR also holds
 * LEC, which reading clears - call this after PortCan_LastError. */
#define PORT_CAN_STATE_ACTIVE  0
#define PORT_CAN_STATE_WARNING 1
#define PORT_CAN_STATE_PASSIVE 2
#define PORT_CAN_STATE_BUSOFF  3
int PortCan_State(void);

int PortCan_Alive(void);
uint32_t PortCan_ClockHz(void);
const char *PortCan_ClockName(void);

#endif /* TESTCASE_COMMON_PORT_CAN_H_ */

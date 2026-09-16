// port_din.h
//
// Shared access to the eight Digital In pins, so a bring-up case and the port
// tool read the same table instead of each carrying its own copy.
//
// The wiring gotcha for these pins is kept in din_test.h, where the documented
// "grep for ***" sweep over *_test.h finds it.
//
// Every line already carries an external 10k pull-up to +3V3 through a 50R
// series resistor and shares its node with an LM339LV open-collector comparator
// output (see $PROD/docs/hardware/HARDWARE-FACTS.md), so no internal pull is configured
// - one would fight the board and distort the reading.

#ifndef TESTCASE_COMMON_PORT_DIN_H_
#define TESTCASE_COMMON_PORT_DIN_H_

#include "main.h"
#include <stdint.h>

#define PORT_DIN_COUNT 8

typedef struct {
    GPIO_TypeDef *port;
    uint16_t      pin;
    const char   *name;
} port_din_t;

/* Terminal order: index 0 is Digital IN 1 (terminal D02, PC6), index 1 is
 * Digital IN 2 (D03, PB5). Matches DIN_1..DIN_8 in the Arduino variant header,
 * not the order this table used to be written in. */
extern const port_din_t port_din_pins[PORT_DIN_COUNT];

/* Configures all eight as high-impedance inputs with no internal pull. */
void PortDin_Init(void);

/* Reads all eight into one byte, bit 0 = Digital IN 1. */
uint8_t PortDin_ReadBits(void);

#endif /* TESTCASE_COMMON_PORT_DIN_H_ */

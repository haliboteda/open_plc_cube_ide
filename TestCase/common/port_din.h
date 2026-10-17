// port_din.h
//
// Access to the eight Digital In pins for the port tool.
//
// The idle reading depends on whether 24 V is connected; neither is a fault.
// No internal pull: the board already has one (HARDWARE-FACTS.md
// "Digital In 八路").

#ifndef TESTCASE_COMMON_PORT_DIN_H_
#define TESTCASE_COMMON_PORT_DIN_H_

#include "main.h"
#include <stdint.h>

#define PORT_DIN_COUNT 8

typedef struct {
    GPIO_TypeDef *port;
    uint16_t      pin;
} port_din_t;

/* Terminal order: index 0 is Digital IN 1 (terminal D02), as DIN_1..DIN_8 in
 * the Arduino variant header. */
extern const port_din_t port_din_pins[PORT_DIN_COUNT];

/* Configures all eight as high-impedance inputs with no internal pull. */
void PortDin_Init(void);

/* Reads all eight into one byte, bit 0 = Digital IN 1. */
uint8_t PortDin_ReadBits(void);

#endif /* TESTCASE_COMMON_PORT_DIN_H_ */

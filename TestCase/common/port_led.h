// port_led.h
//
// The system indicator on PE2, driven high to light it.
//
// MX_GPIO_Init() enables the GPIOE clock but leaves this pin unconfigured -
// the pin assignment spreadsheet files it under "unused pins" even though it
// has a function - so anything using it has to configure it first. PortLed_Init
// does that and is safe to call more than once.
//
// There is no readback on this pin. Nothing here can report whether the lamp
// actually lit; that is a person's verdict.

#ifndef TESTCASE_COMMON_PORT_LED_H_
#define TESTCASE_COMMON_PORT_LED_H_

#include <stdint.h>

void PortLed_Init(void);
void PortLed_Set(int on);

/* Blocks for pulses * (2 * half_ms). For a one-shot check, not for a loop that
 * has other work to do - pt.led drives PortLed_Set() directly. */
void PortLed_Blink(uint32_t pulses, uint32_t half_ms);

#endif /* TESTCASE_COMMON_PORT_LED_H_ */

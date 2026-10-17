// port_led.h
//
// The system indicator on PE2, high = lit (HARDWARE-FACTS.md PE2). MX_GPIO_Init
// leaves the pin unconfigured, so PortLed_Init configures it (idempotent).
// No readback: whether it lit is a person's verdict.

#ifndef TESTCASE_COMMON_PORT_LED_H_
#define TESTCASE_COMMON_PORT_LED_H_

#include <stdint.h>

void PortLed_Init(void);
void PortLed_Set(int on);

/* Blocks for pulses * (2 * half_ms). For a one-shot check, not for a loop that
 * has other work to do - pt.led drives PortLed_Set() directly. */
void PortLed_Blink(uint32_t pulses, uint32_t half_ms);

#endif /* TESTCASE_COMMON_PORT_LED_H_ */

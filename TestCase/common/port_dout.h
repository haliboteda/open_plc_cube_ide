// port_dout.h
//
// Shared access to the eight Digital Out pins (Klemmblock A, A03-A10), each
// able to hold a level or run its own PWM duty cycle.
//
// Pin and timer facts: HARDWARE-FACTS.md.
//
// Software PWM: the pins pair up on shared timer capture/compare units, so
// hardware PWM would give four independent duties, not eight (DECISIONS.md 10).
//
// freq is left wide open: the VNQ5160K-E's switching limit is unknown and is
// to be measured.

#ifndef TESTCASE_COMMON_PORT_DOUT_H_
#define TESTCASE_COMMON_PORT_DOUT_H_

#include "main.h"
#include <stdint.h>

#define PORT_DOUT_COUNT 8

/* Duty resolution in bits, shared by all eight channels; one PWM period is
 * 2^res interrupts and duty is a raw count 0..2^res-1. 8 is the Arduino
 * default. See $PROD/docs/tables/DECISIONS.md 101. */
#define PORT_DOUT_RES_MIN  1U
#define PORT_DOUT_RES_MAX  16U
#define PORT_DOUT_RES_DEF  8U

/* Cap on the shared interrupt rate (fastest freq * 2^res): 1000 Hz at the
 * default 8 bits. See $PROD/docs/tables/DECISIONS.md 101. */
#define PORT_DOUT_TICK_MAX_HZ 256000U

#define PORT_DOUT_FREQ_DEF_HZ 1000U
#define PORT_DOUT_FREQ_MIN_HZ 1U
/* Highest frequency a channel may ask for at `res` bits. */
#define PORT_DOUT_FREQ_MAX_HZ(res) (PORT_DOUT_TICK_MAX_HZ >> (res))

typedef struct {
    GPIO_TypeDef *port;
    uint16_t      pin;
} port_dout_t;

/* Terminal order: index 0 is Digital Out 1 (terminal A03, PB13). */
extern const port_dout_t port_dout_pins[PORT_DOUT_COUNT];

/* Configures all eight as push-pull outputs, driven low, and starts the PWM
 * timer. Every channel starts at PORT_DOUT_FREQ_DEF_HZ. Returns 0 if the timer
 * refused to start, in which case nothing is driven - a half-configured output
 * port would be worse than none. Safe to call more than once. */
int PortDout_Init(void);

/* Duty resolution for all eight channels, PORT_DOUT_RES_MIN..MAX. Returns 0
 * if out of range, if any channel's frequency is over the limit at that
 * resolution, or if the timer refused the new rate; nothing changes then.
 * Duties are not rescaled: callers set them again in the new unit. */
int PortDout_SetRes(uint32_t res);
uint32_t PortDout_Res(void);

/* Frequency for ONE channel, clamped to the min/max above. Returns 0 only if
 * the timer refused the new rate; the previous frequency is then put back.
 * The interrupt rate is shared and set by the fastest channel. */
int PortDout_SetFreq(int ch, uint32_t freq_hz);

/* Raw count 0..2^res-1, clamped; 0 and full are held steady, not switched.
 * Out-of-range channels are ignored. */
void PortDout_SetDuty(int ch, uint32_t duty);

/* Drives every channel low and leaves them there. */
void PortDout_AllOff(void);

/* Drives everything low and stops the PWM timer. */
void PortDout_Stop(void);

/* What one channel actually landed on (rate and increment are quantised). */
uint32_t PortDout_ActualFreqHz(int ch);

/* The shared interrupt rate. */
uint32_t PortDout_TickHz(void);

#endif /* TESTCASE_COMMON_PORT_DOUT_H_ */

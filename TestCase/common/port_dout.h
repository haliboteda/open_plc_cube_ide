// port_dout.h
//
// Shared access to the eight Digital Out pins (Klemmblock A, A03-A10), each
// able to hold a level or run its own PWM duty cycle.
//
// The wiring gotchas for this port are kept in the bring-up notes; the pin and
// timer facts are in docs/design/HARDWARE-FACTS.md.
//
// *** Why software PWM and not the timer channels: all eight pins do sit on a
// *** timer channel, but they pair up - DO1/DO5 are TIM1_CH1 and its
// *** complementary output, DO2/DO6 are TIM1_CH2, DO3/DO7 TIM8_CH3, DO4/DO8
// *** TIM15_CH1. Two pins sharing one capture/compare unit cannot hold
// *** different duty cycles, so hardware PWM would give four, not eight, and
// *** "you ticked DO1 and DO5 moved" is not something a panel can explain.
// *** See docs/design/DECISIONS.md 10.
//
// *** The switching limit of the VNQ5160K-E high-side driver in front of these
// *** terminals is NOT known - it is a smart switch with a charge pump, not a
// *** bare MOSFET, and no datasheet for it is on hand. freq is therefore left
// *** wide open so the limit can be measured rather than assumed.

#ifndef TESTCASE_COMMON_PORT_DOUT_H_
#define TESTCASE_COMMON_PORT_DOUT_H_

#include "main.h"
#include <stdint.h>

#define PORT_DOUT_COUNT 8

/* Duty is set in whole percent, so one PWM period is this many interrupts. */
#define PORT_DOUT_STEPS 100U

#define PORT_DOUT_FREQ_DEF_HZ 1000U
#define PORT_DOUT_FREQ_MIN_HZ 1U
/* The interrupt runs at freq * PORT_DOUT_STEPS. At the cap that is 200 kHz,
 * which on a 400 MHz M7 leaves about 2000 cycles per pass for an interrupt
 * that needs a few dozen - and far past anything a 24 V smart high-side
 * switch will reproduce. */
#define PORT_DOUT_FREQ_MAX_HZ 2000U

typedef struct {
    GPIO_TypeDef *port;
    uint16_t      pin;
    const char   *name;   /* terminal, e.g. "A03" */
} port_dout_t;

/* Terminal order: index 0 is Digital Out 1 (terminal A03, PB13). */
extern const port_dout_t port_dout_pins[PORT_DOUT_COUNT];

/* Configures all eight as push-pull outputs, driven low, and starts the PWM
 * timer. Every channel starts at PORT_DOUT_FREQ_DEF_HZ. Returns 0 if the timer
 * refused to start, in which case nothing is driven - a half-configured output
 * port would be worse than none. Safe to call more than once. */
int PortDout_Init(void);

/* Frequency for ONE channel. Clamped to the min/max above. Returns 0 only if
 * the timer refused the rate the new set of frequencies needs, in which case
 * the previous frequency is put back.
 *
 * *** Each channel really is independent, unlike the hardware timer channels
 * *** these pins sit on - see the note at the top of this file. What is shared
 * *** is the interrupt rate, which the fastest channel sets. A slow channel
 * *** beside a fast one is therefore resolved more finely, never less. */
int PortDout_SetFreq(int ch, uint32_t freq_hz);

/* 0..100. A channel at 0 or 100 is held steady rather than switched, so a
 * plain on/off needs no PWM edges at all. Out-of-range channels are ignored. */
void PortDout_SetDuty(int ch, uint32_t duty_pct);

/* Drives every channel low and leaves them there. */
void PortDout_AllOff(void);

/* Drives everything low and stops the PWM timer, so nothing is left switching
 * and no interrupt keeps firing. Called when the session stops. */
void PortDout_Stop(void);

/* What one channel actually landed on, which is not always what was asked for:
 * the interrupt rate is quantised by the prescaler, and each channel's
 * increment is quantised by that rate. */
uint32_t PortDout_ActualFreqHz(int ch);

/* The shared interrupt rate. Reported so the resolution behind those
 * frequencies is visible rather than something to be inferred. */
uint32_t PortDout_TickHz(void);

#endif /* TESTCASE_COMMON_PORT_DOUT_H_ */

// port_pwm.h
//
// Shared TIM1_CH2 / PA9 PWM output - that pin is Digital Out 6 (HIGHSIDE_FET6,
// terminal A08). The handle stays inside port_pwm.c so that including this
// header does not drag in the HAL TIM module guard.
//
// *** Only this one of the eight digital outputs is covered here, because it is
// *** the only one any existing bring-up case drives. The other seven have no
// *** module yet.

#ifndef TESTCASE_COMMON_PORT_PWM_H_
#define TESTCASE_COMMON_PORT_PWM_H_

#include <stdint.h>

#define PORT_PWM_DEFAULT_FREQ_HZ 1000U   /* well above visible flicker */
#define PORT_PWM_STEPS           1000U   /* duty resolution: 0.1% */

/* Configures TIM1_CH2 on PA9 and starts it at `duty_pct`. Returns 0 on
 * failure. Safe to call more than once. */
int PortPwm_Init(uint32_t freq_hz, uint32_t duty_pct);

/* 0..100. */
void PortPwm_SetDuty(uint32_t duty_pct);

/* What the prescaler actually landed on, for a caller that wants to report it. */
uint32_t PortPwm_KernelClockHz(void);
uint32_t PortPwm_Prescaler(void);
uint32_t PortPwm_ActualFreqHz(void);

#endif /* TESTCASE_COMMON_PORT_PWM_H_ */

// pwm_test.h
//
// Standalone PWM bring-up test for the STM32H743 OpenPLC board.
// Not part of the bootloader's core logic - safe to delete once the
// PWM driver has been validated and ported into the Arduino core.
//
// Output under test : Digital Out 6 / HIGHSIDE_FET6, PA9 = TIM1_CH2
//
// This drives a slow "breathing" PWM (duty ramps 0% -> 100% -> 0%) on
// Digital Out 6 so the result can be checked visually with an LED wired
// to that terminal - no jumper wire, scope, or multimeter required.
// See pwm_test.c for the wiring notes (LED + series resistor).
//
// Runs forever (does not return) - flash this build only for the bring-up
// check, then reflash the normal firmware.

#ifndef INC_PWM_TEST_H_
#define INC_PWM_TEST_H_

void PWM_Test_Run(void);

#endif /* INC_PWM_TEST_H_ */

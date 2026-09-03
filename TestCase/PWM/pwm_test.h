// pwm_test.h
//
// *** Digital Out 6 is a high-side FET fed from the 24 V input, so 24 V must
// *** be connected or the output cannot drive the LED at all. Size the series
// *** resistor for that rail, not for 3V3.
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

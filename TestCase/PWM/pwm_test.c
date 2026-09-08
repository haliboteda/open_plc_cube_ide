// pwm_test.c
//
// Standalone PWM bring-up test - see pwm_test.h for wiring/pin notes.
//
// Drives TIM1_CH2 / PA9 (Digital Out 6 / HIGHSIDE_FET6 terminal) with a
// 1 kHz PWM that ramps perceived brightness 0% -> 100% -> 0% forever
// ("breathing"), 3s per direction by default. The duty cycle itself is NOT
// a linear ramp - see PWM_Test_LightnessToDuty() for why.
// Wire an LED (+ series resistor, sized for the Digital Out supply rail)
// across the Digital Out 6 terminal and its return/GND terminal: it should
// fade smoothly up and down. No jumper wire or instrument needed to see
// that the timer channel is alive and the duty cycle is actually changing.
//
// What this does NOT prove (needs a scope/multimeter):
//   - edge quality (rise/fall time, ringing, actual voltage levels)
//   - exact frequency/duty accuracy, or behaviour under a real load
//     (motor/lamp/fan) on the VNQ5160K-E high-side switch
//
// The timer itself is set up through TestCase/common/port_pwm.*, which the port
// tool shares. What stays here is the breathing ramp and the wording.

#include "pwm_test.h"
#include "port_pwm.h"
#include "main.h"
#include <stdio.h>

/* ---- Breathing ramp ----------------------------------------------------- */
#define BREATHE_STEP_PCT         1U      /* brightness change per step */
#define BREATHE_STEP_DELAY_MS    30U     /* 100 steps * 30ms = 3s per direction */

/* Human eyes don't perceive duty cycle linearly (far more sensitive at low
   duty, almost flat near 100%) - a linear duty ramp looks like it snaps to
   "bright" in the first fraction of the ramp and just sits there. This maps
   a linearly-increasing perceived lightness (0-100, "eyeball %") to the duty
   cycle (0-100) that actually produces it, using the CIE 1931 lightness
   formula. Feed it a linear time ramp and the LED looks evenly brighter. */
static uint32_t PWM_Test_LightnessToDuty(uint32_t lightness_pct)
{
    float l = (float)lightness_pct;
    float y;
    if (l <= 8.0f) {
        y = l / 903.3f;
    } else {
        float t = (l + 16.0f) / 116.0f;
        y = t * t * t;
    }
    return (uint32_t)(y * 100.0f + 0.5f);
}

void PWM_Test_Run(void)
{
    printf("PWM_TEST: LED breathing on Digital Out 6 (PA9/TIM1_CH2/HIGHSIDE_FET6)\r\n");

    if (!PortPwm_Init(PORT_PWM_DEFAULT_FREQ_HZ, 0U)) {
        printf("PWM_TEST: FAIL reason=output_init\r\n");
        return;
    }

    printf("PWM_TEST: output cfg TIM1_CH2/PA9 tim1clk=%luHz psc=%lu arr=%lu -> freq=%luHz\r\n",
           (unsigned long)PortPwm_KernelClockHz(),
           (unsigned long)PortPwm_Prescaler(),
           (unsigned long)(PORT_PWM_STEPS - 1U),
           (unsigned long)PortPwm_ActualFreqHz());

    for (;;) {
        for (uint32_t lightness = 0U; lightness <= 100U; lightness += BREATHE_STEP_PCT) {
            PortPwm_SetDuty(PWM_Test_LightnessToDuty(lightness));
            HAL_Delay(BREATHE_STEP_DELAY_MS);
        }
        for (uint32_t lightness = 100U; lightness > 0U; lightness -= BREATHE_STEP_PCT) {
            PortPwm_SetDuty(PWM_Test_LightnessToDuty(lightness - BREATHE_STEP_PCT));
            HAL_Delay(BREATHE_STEP_DELAY_MS);
        }
    }
}

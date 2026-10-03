/*
 * safe_outputs.h
 *
 * Every board output at 0 until the user's sketch first writes it: DO1-DO8
 * off, AO1/AO2 at 0 mA, KNX_TX idle. Decision 81, $PROD/docs/tables/DECISIONS.md;
 * sequence in $PROD/docs/modules/M1/BOOT-SEQUENCE.md.
 */
#ifndef SAFE_OUTPUTS_H_
#define SAFE_OUTPUTS_H_

/* Drives the eleven pins low as push-pull outputs. Needs no HAL and no clock tree,
 * so main() calls it before anything else is initialised, and
 * server_jump_to_app() calls it again after HAL_DeInit() has reset the GPIO
 * ports. The relays are not here: MX_GPIO_Init() already drives them low. */
void safe_outputs_init(void);

#endif /* SAFE_OUTPUTS_H_ */

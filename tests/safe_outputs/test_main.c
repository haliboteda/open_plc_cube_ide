/*
 * T1-36: safe_outputs_init() leaves DO1-DO8 and the two AO pins as push-pull
 * outputs at 0, with their GPIO clocks on, and touches no other pin.
 * Pins from $HW GPIO assignment rows 92-93, 99-106. Decision 81.
 */
#include <stdio.h>
#include "main.h"
#include "safe_outputs.h"

GPIO_TypeDef fake_gpio[9];
RCC_TypeDef  fake_rcc;

static int failures;

static void check(int ok, const char *what)
{
	printf("[%s] %s\n", ok ? "PASS" : "FAIL", what);
	if (!ok) {
		failures++;
	}
}

static const struct { int port; unsigned pin; const char *name; } k_expected[] = {
	{1, 13, "DO1 PB13"}, {1, 0, "DO2 PB0"}, {7, 15, "DO3 PH15"}, {4, 4, "DO4 PE4"},
	{0, 8, "DO5 PA8"},   {0, 9, "DO6 PA9"}, {8, 7, "DO7 PI7"},   {4, 5, "DO8 PE5"},
	{0, 4, "AO1 PA4"},   {0, 5, "AO2 PA5"},
};

int main(void)
{
	char msg[96];

	/* Reset values that would bite: every pin analog (MODER 11), every output
	 * high, pull-ups on, open-drain set. */
	for (int i = 0; i < 9; i++) {
		fake_gpio[i].MODER  = 0xFFFFFFFFUL;
		fake_gpio[i].ODR    = 0xFFFFUL;
		fake_gpio[i].PUPDR  = 0x55555555UL;
		fake_gpio[i].OTYPER = 0xFFFFUL;
	}
	fake_rcc.AHB4ENR = 0;

	safe_outputs_init();

	check((fake_rcc.AHB4ENR & 0x193UL) == 0x193UL, "GPIOA, B, E, H, I clocks enabled");

	uint32_t touched[9] = {0};
	for (unsigned k = 0; k < sizeof(k_expected) / sizeof(k_expected[0]); k++) {
		const GPIO_TypeDef *g = &fake_gpio[k_expected[k].port];
		const unsigned p = k_expected[k].pin;
		touched[k_expected[k].port] |= 1UL << p;
		snprintf(msg, sizeof(msg), "%s: output, push-pull, no pull, driven 0", k_expected[k].name);
		check((((g->MODER >> (p * 2)) & 3UL) == 1UL) && (((g->OTYPER >> p) & 1UL) == 0UL)
		      && (((g->PUPDR >> (p * 2)) & 3UL) == 0UL) && (((g->ODR >> p) & 1UL) == 0UL), msg);
	}

	int untouched = 1;
	for (int i = 0; i < 9; i++) {
		for (unsigned p = 0; p < 16; p++) {
			if ((touched[i] >> p) & 1UL) {
				continue;
			}
			if ((((fake_gpio[i].MODER >> (p * 2)) & 3UL) != 3UL) || (((fake_gpio[i].ODR >> p) & 1UL) != 1UL)) {
				untouched = 0;
			}
		}
	}
	check(untouched, "no other pin changed");

	printf(failures ? "T1-36: %d check(s) failed\n" : "T1-36: all checks passed\n", failures);
	return failures ? 1 : 0;
}

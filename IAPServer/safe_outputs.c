/*
 * safe_outputs.c -- see safe_outputs.h.
 */
#include "safe_outputs.h"
#include "main.h"

/* Pin source: $HW/STM32H743IIK6_GPIO_ASSIGNMENT_Schaeffer_Bridge_20260822.xlsx,
 * rows 99-106 (HSFET_1..8, the VNQ5160K-E inputs) and 92-93 (AOUT1/AOUT2,
 * which feed the XTR111 VIN through 10k: 0 V in is 0 mA out). */
typedef struct {
	GPIO_TypeDef *port;
	uint8_t pin;
} safe_pin_t;

static const safe_pin_t k_pins[] = {
	{ GPIOB, 13U },   /* DO1 */
	{ GPIOB,  0U },   /* DO2 */
	{ GPIOH, 15U },   /* DO3 */
	{ GPIOE,  4U },   /* DO4 */
	{ GPIOA,  8U },   /* DO5 */
	{ GPIOA,  9U },   /* DO6 */
	{ GPIOI,  7U },   /* DO7 */
	{ GPIOE,  5U },   /* DO8 */
	{ GPIOA,  4U },   /* AO1 VIN */
	{ GPIOA,  5U },   /* AO2 VIN */
};

void safe_outputs_init(void)
{
	RCC->AHB4ENR |= RCC_AHB4ENR_GPIOAEN | RCC_AHB4ENR_GPIOBEN | RCC_AHB4ENR_GPIOEEN
	              | RCC_AHB4ENR_GPIOHEN | RCC_AHB4ENR_GPIOIEN;
	(void)RCC->AHB4ENR;   /* the enable must land before the first GPIO access */

	for (unsigned i = 0U; i < (sizeof(k_pins) / sizeof(k_pins[0])); i++) {
		GPIO_TypeDef *const p = k_pins[i].port;
		const uint32_t pin = k_pins[i].pin;

		/* Output data first, so the pin is low the moment it becomes an output. */
		p->ODR    &= ~(1UL << pin);
		p->OTYPER &= ~(1UL << pin);
		p->PUPDR  &= ~(3UL << (pin * 2U));
		p->MODER   = (p->MODER & ~(3UL << (pin * 2U))) | (1UL << (pin * 2U));
	}
}

/* Host stand-in for main.h: just the registers safe_outputs.c touches, backed by
 * plain structs the test can inspect. */
#ifndef MAIN_H_
#define MAIN_H_

#include <stdint.h>

typedef struct {
	volatile uint32_t MODER, OTYPER, OSPEEDR, PUPDR, IDR, ODR, BSRR, LCKR, AFR[2];
} GPIO_TypeDef;

typedef struct {
	volatile uint32_t AHB4ENR;
} RCC_TypeDef;

extern GPIO_TypeDef fake_gpio[9];   /* A..I */
extern RCC_TypeDef  fake_rcc;

#define GPIOA (&fake_gpio[0])
#define GPIOB (&fake_gpio[1])
#define GPIOE (&fake_gpio[4])
#define GPIOH (&fake_gpio[7])
#define GPIOI (&fake_gpio[8])
#define RCC   (&fake_rcc)

#define RCC_AHB4ENR_GPIOAEN (1UL << 0)
#define RCC_AHB4ENR_GPIOBEN (1UL << 1)
#define RCC_AHB4ENR_GPIOEEN (1UL << 4)
#define RCC_AHB4ENR_GPIOHEN (1UL << 7)
#define RCC_AHB4ENR_GPIOIEN (1UL << 8)

#endif

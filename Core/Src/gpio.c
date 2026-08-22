/* USER CODE BEGIN Header */
/**
  ******************************************************************************
  * @file    gpio.c
  * @brief   This file provides code for the configuration
  *          of all used GPIO pins.
  ******************************************************************************
  * @attention
  *
  * Copyright (c) 2024 STMicroelectronics.
  * All rights reserved.
  *
  * This software is licensed under terms that can be found in the LICENSE file
  * in the root directory of this software component.
  * If no LICENSE file comes with this software, it is provided AS-IS.
  *
  ******************************************************************************
  */
/* USER CODE END Header */

/* Includes ------------------------------------------------------------------*/
#include "gpio.h"

/* USER CODE BEGIN 0 */

/* USER CODE END 0 */

/*----------------------------------------------------------------------------*/
/* Configure GPIO                                                             */
/*----------------------------------------------------------------------------*/
/* USER CODE BEGIN 1 */

/* USER CODE END 1 */

/** Configure pins
     PH0-OSC_IN (PH0)   ------> RCC_OSC_IN
     PH1-OSC_OUT (PH1)   ------> RCC_OSC_OUT
*/
void MX_GPIO_Init(void)
{

  GPIO_InitTypeDef GPIO_InitStruct = {0};

  /* GPIO Ports Clock Enable */
  __HAL_RCC_GPIOE_CLK_ENABLE();
  __HAL_RCC_GPIOG_CLK_ENABLE();
  __HAL_RCC_GPIOD_CLK_ENABLE();
  __HAL_RCC_GPIOC_CLK_ENABLE();
  __HAL_RCC_GPIOA_CLK_ENABLE();
  __HAL_RCC_GPIOI_CLK_ENABLE();
  __HAL_RCC_GPIOF_CLK_ENABLE();
  __HAL_RCC_GPIOH_CLK_ENABLE();
  __HAL_RCC_GPIOB_CLK_ENABLE();

  /*Configure GPIO pin Output Level */
  HAL_GPIO_WritePin(GPIOG, BOOT0_Pin|RY4_Pin|RY5_Pin, GPIO_PIN_RESET);

  /*Configure GPIO pin Output Level */
  HAL_GPIO_WritePin(GPIOI, RY1_Pin|RY2_Pin|RY3_Pin, GPIO_PIN_RESET);

  /*Configure GPIO pin Output Level */
  HAL_GPIO_WritePin(RY6_GPIO_Port, RY6_Pin, GPIO_PIN_RESET);

  /*Configure GPIO pin Output Level */
  HAL_GPIO_WritePin(RS232_Enable_GPIO_Port, RS232_Enable_Pin, GPIO_PIN_RESET);

  /*Configure GPIO pins : BOOT0_Pin RY4_Pin RY5_Pin */
  GPIO_InitStruct.Pin = BOOT0_Pin|RY4_Pin|RY5_Pin;
  GPIO_InitStruct.Mode = GPIO_MODE_OUTPUT_PP;
  GPIO_InitStruct.Pull = GPIO_NOPULL;
  GPIO_InitStruct.Speed = GPIO_SPEED_FREQ_LOW;
  HAL_GPIO_Init(GPIOG, &GPIO_InitStruct);

  /*Configure GPIO pins : RY1_Pin RY2_Pin RY3_Pin */
  GPIO_InitStruct.Pin = RY1_Pin|RY2_Pin|RY3_Pin;
  GPIO_InitStruct.Mode = GPIO_MODE_OUTPUT_PP;
  GPIO_InitStruct.Pull = GPIO_NOPULL;
  GPIO_InitStruct.Speed = GPIO_SPEED_FREQ_LOW;
  HAL_GPIO_Init(GPIOI, &GPIO_InitStruct);

  /*Configure GPIO pin : RY6_Pin */
  GPIO_InitStruct.Pin = RY6_Pin;
  GPIO_InitStruct.Mode = GPIO_MODE_OUTPUT_PP;
  GPIO_InitStruct.Pull = GPIO_NOPULL;
  GPIO_InitStruct.Speed = GPIO_SPEED_FREQ_LOW;
  HAL_GPIO_Init(RY6_GPIO_Port, &GPIO_InitStruct);

  /*Configure GPIO pin : RS232_Enable_Pin */
  GPIO_InitStruct.Pin = RS232_Enable_Pin;
  GPIO_InitStruct.Mode = GPIO_MODE_OUTPUT_PP;
  GPIO_InitStruct.Pull = GPIO_NOPULL;
  GPIO_InitStruct.Speed = GPIO_SPEED_FREQ_LOW;
  HAL_GPIO_Init(RS232_Enable_GPIO_Port, &GPIO_InitStruct);

}

/* USER CODE BEGIN 2 */

/*
 * Re-configure PG9 (the BOOT0 / SW2 net) as an input.
 *
 * MX_GPIO_Init() above is CubeMX-generated and leaves PG9 as a push-pull
 * output driving low, grouped with RY4/RY5. That is wrong for this net and
 * mildly dangerous: SW2 connects PG9 to 3V3, so pressing the button shorts
 * 3V3 into an output that is actively driving low. It survived only because
 * the startup window is 1.5 s, so nobody holds the button for long.
 *
 * PG9 does not need to be an output. The net is held low by R58 (10k to GND),
 * measured 2026-08-12: reading the pin as PULLUP, PULLDOWN and NOPULL all
 * returned 0, so even the internal pull-up cannot lift it. boot0_is_pressed()
 * reads IDR, which works the same either way.
 *
 * Done here rather than in the generated block above so that regenerating from
 * the .ioc cannot quietly undo it -- the same reason fmc.c keeps its power-up
 * sequence in a USER CODE section.
 *
 * ⚠️ TEMPORARY. The right place for this is the .ioc, so that MX_GPIO_Init()
 * generates the input configuration directly. It lives here because it was
 * made as a controlled experiment and regenerating the whole project would
 * have added a second variable to it. Once the .ioc is updated, this function
 * and its call in main.c become a redundant re-configuration and should both
 * be deleted -- see docs/work/ISSUES.md.
 *
 * ⚠️ There is a report that changing this pin to an input once stopped the
 * RESET button from working. docs/design/HARDWARE-FACTS.md records the cause as a
 * different change made at the same time (SystemClock_Config() had been
 * deleted, leaving the MCU reading flash out of spec at VOS3 + 64 MHz + 0 wait
 * states), not this pin. That is why this change is made on its own and the
 * RESET button is tested straight after it.
 */
void BOOT0_ConfigureAsInput(void)
{
	GPIO_InitTypeDef GPIO_InitStruct = {0};

	GPIO_InitStruct.Pin  = BOOT0_Pin;
	GPIO_InitStruct.Mode = GPIO_MODE_INPUT;
	GPIO_InitStruct.Pull = GPIO_NOPULL;   /* R58 does it, and it beats the internal pulls */
	HAL_GPIO_Init(BOOT0_GPIO_Port, &GPIO_InitStruct);
}

/* USER CODE END 2 */

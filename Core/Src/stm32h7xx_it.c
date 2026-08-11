/* USER CODE BEGIN Header */
/**
 ******************************************************************************
 * @file    stm32h7xx_it.c
 * @brief   Interrupt Service Routines.
 ******************************************************************************
 * @attention
 *
 * Copyright (c) 2023 STMicroelectronics.
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
#include "main.h"
#include "stm32h7xx_it.h"
/* Private includes ----------------------------------------------------------*/
/* USER CODE BEGIN Includes */
#include <stdio.h>
/* USER CODE END Includes */

/* Private typedef -----------------------------------------------------------*/
/* USER CODE BEGIN TD */

/* USER CODE END TD */

/* Private define ------------------------------------------------------------*/
/* USER CODE BEGIN PD */

/* USER CODE END PD */

/* Private macro -------------------------------------------------------------*/
/* USER CODE BEGIN PM */

/* USER CODE END PM */

/* Private variables ---------------------------------------------------------*/
/* USER CODE BEGIN PV */

/* USER CODE END PV */

/* Private function prototypes -----------------------------------------------*/
/* USER CODE BEGIN PFP */
void IT_Fault_Report(uint32_t *frame, uint32_t exc_return, uint32_t which);
/* USER CODE END PFP */

/* Private user code ---------------------------------------------------------*/
/* USER CODE BEGIN 0 */

/* USER CODE END 0 */

/* External variables --------------------------------------------------------*/
extern PCD_HandleTypeDef hpcd_USB_OTG_FS;
extern TIM_HandleTypeDef htim6;

/* USER CODE BEGIN EV */

/* USER CODE END EV */

/******************************************************************************/
/*           Cortex Processor Interruption and Exception Handlers          */
/******************************************************************************/
/**
  * @brief This function handles Non maskable interrupt.
  */
void NMI_Handler(void)
{
  /* USER CODE BEGIN NonMaskableInt_IRQn 0 */

  /* USER CODE END NonMaskableInt_IRQn 0 */
  /* USER CODE BEGIN NonMaskableInt_IRQn 1 */
	while (1) {
	}
  /* USER CODE END NonMaskableInt_IRQn 1 */
}

/*
 * The four fault handlers below are naked on purpose, and that replaces the
 * bodies CubeMX generates. Regenerating from the .ioc puts the plain versions
 * back and the register dump silently goes wrong again -- so if the dump ever
 * starts printing a stack address where xPSR should be, look here first.
 *
 * Why naked: the exception frame sits at the stack pointer *as it was on
 * exception entry*. A normal C function has already run its prologue by the
 * time its first statement executes, so __get_MSP() read from inside the
 * handler points below the frame by however many bytes that prologue used.
 * That is what the previous version did, which is why its R0/R1 landed near
 * the truth while LR, PC and xPSR were garbage.
 *
 * EXC_RETURN (in LR on entry) bit 2 says which stack was in use: clear = MSP,
 * set = PSP. Both are passed on so the report can say which.
 */
__attribute__((naked)) void HardFault_Handler(void)
{
  __asm volatile (
    "tst   lr, #4        \n"
    "ite   eq            \n"
    "mrseq r0, msp       \n"
    "mrsne r0, psp       \n"
    "mov   r1, lr        \n"
    "movs  r2, #1        \n"
    "b     IT_Fault_Report\n"
  );
}

__attribute__((naked)) void MemManage_Handler(void)
{
  __asm volatile (
    "tst   lr, #4        \n"
    "ite   eq            \n"
    "mrseq r0, msp       \n"
    "mrsne r0, psp       \n"
    "mov   r1, lr        \n"
    "movs  r2, #2        \n"
    "b     IT_Fault_Report\n"
  );
}

__attribute__((naked)) void BusFault_Handler(void)
{
  __asm volatile (
    "tst   lr, #4        \n"
    "ite   eq            \n"
    "mrseq r0, msp       \n"
    "mrsne r0, psp       \n"
    "mov   r1, lr        \n"
    "movs  r2, #3        \n"
    "b     IT_Fault_Report\n"
  );
}

__attribute__((naked)) void UsageFault_Handler(void)
{
  __asm volatile (
    "tst   lr, #4        \n"
    "ite   eq            \n"
    "mrseq r0, msp       \n"
    "mrsne r0, psp       \n"
    "mov   r1, lr        \n"
    "movs  r2, #4        \n"
    "b     IT_Fault_Report\n"
  );
}

/**
  * @brief This function handles System service call via SWI instruction.
  */
void SVC_Handler(void)
{
  /* USER CODE BEGIN SVCall_IRQn 0 */

  /* USER CODE END SVCall_IRQn 0 */
  /* USER CODE BEGIN SVCall_IRQn 1 */

  /* USER CODE END SVCall_IRQn 1 */
}

/**
  * @brief This function handles Debug monitor.
  */
void DebugMon_Handler(void)
{
  /* USER CODE BEGIN DebugMonitor_IRQn 0 */

  /* USER CODE END DebugMonitor_IRQn 0 */
  /* USER CODE BEGIN DebugMonitor_IRQn 1 */

  /* USER CODE END DebugMonitor_IRQn 1 */
}

/**
  * @brief This function handles Pendable request for system service.
  */
void PendSV_Handler(void)
{
  /* USER CODE BEGIN PendSV_IRQn 0 */

  /* USER CODE END PendSV_IRQn 0 */
  /* USER CODE BEGIN PendSV_IRQn 1 */

  /* USER CODE END PendSV_IRQn 1 */
}

/**
  * @brief This function handles System tick timer.
  */
void SysTick_Handler(void)
{
  /* USER CODE BEGIN SysTick_IRQn 0 */

  /* USER CODE END SysTick_IRQn 0 */

  /* USER CODE BEGIN SysTick_IRQn 1 */

  /* USER CODE END SysTick_IRQn 1 */
}

/******************************************************************************/
/* STM32H7xx Peripheral Interrupt Handlers                                    */
/* Add here the Interrupt Handlers for the used peripherals.                  */
/* For the available peripheral interrupt handler names,                      */
/* please refer to the startup file (startup_stm32h7xx.s).                    */
/******************************************************************************/

/**
  * @brief This function handles TIM6 global interrupt, DAC1_CH1 and DAC1_CH2 underrun error interrupts.
  */
void TIM6_DAC_IRQHandler(void)
{
  /* USER CODE BEGIN TIM6_DAC_IRQn 0 */

  /* USER CODE END TIM6_DAC_IRQn 0 */
  HAL_TIM_IRQHandler(&htim6);
  /* USER CODE BEGIN TIM6_DAC_IRQn 1 */

  /* USER CODE END TIM6_DAC_IRQn 1 */
}

/**
  * @brief This function handles USB On The Go FS global interrupt.
  */
void OTG_FS_IRQHandler(void)
{
  /* USER CODE BEGIN OTG_FS_IRQn 0 */

  /* USER CODE END OTG_FS_IRQn 0 */
  HAL_PCD_IRQHandler(&hpcd_USB_OTG_FS);
  /* USER CODE BEGIN OTG_FS_IRQn 1 */

  /* USER CODE END OTG_FS_IRQn 1 */
}

/* USER CODE BEGIN 1 */
/*
 * Called from the naked handlers above with the exception frame still intact.
 * `frame` is the stack pointer at exception entry, `exc_return` is the LR value
 * the core supplied, `which` names the handler.
 */
void IT_Fault_Report(uint32_t *frame, uint32_t exc_return, uint32_t which)
{
    static const char * const names[] = {
        "Unknown", "HardFault", "MemManage", "BusFault", "UsageFault"
    };
    const uint32_t cfsr  = SCB->CFSR;
    const uint32_t hfsr  = SCB->HFSR;
    const uint32_t mmfsr = cfsr & 0xFFU;
    const uint32_t bfsr  = (cfsr >> 8) & 0xFFU;
    const uint32_t ufsr  = (cfsr >> 16) & 0xFFFFU;

    printf("\n--- %s ---\n", names[(which < 5U) ? which : 0U]);
    printf("frame @ 0x%08lX (%s)  EXC_RETURN = 0x%08lX\n",
           (unsigned long) (uint32_t) frame,
           ((exc_return & 4U) != 0U) ? "PSP" : "MSP",
           (unsigned long) exc_return);

    /* The stacked frame: R0 R1 R2 R3 R12 LR PC xPSR. PC is the instruction
     * that faulted (or the one after it, for imprecise errors); LR is the
     * return address of whoever called that code -- run both through
     * addr2line to get file and line. */
    printf("R0  = 0x%08lX  R1 = 0x%08lX  R2 = 0x%08lX  R3 = 0x%08lX\n",
           (unsigned long) frame[0], (unsigned long) frame[1],
           (unsigned long) frame[2], (unsigned long) frame[3]);
    printf("R12 = 0x%08lX  LR = 0x%08lX  PC = 0x%08lX  xPSR = 0x%08lX\n",
           (unsigned long) frame[4], (unsigned long) frame[5],
           (unsigned long) frame[6], (unsigned long) frame[7]);

    printf("CFSR = 0x%08lX  HFSR = 0x%08lX\n",
           (unsigned long) cfsr, (unsigned long) hfsr);

    /* Decoded, because the bit that matters here is easy to miss in hex. */
    if (mmfsr != 0U) {
        printf("  MemManage:%s%s%s%s%s\n",
               (mmfsr & (1U << 0)) ? " IACCVIOL"  : "",
               (mmfsr & (1U << 1)) ? " DACCVIOL"  : "",
               (mmfsr & (1U << 3)) ? " MUNSTKERR" : "",
               (mmfsr & (1U << 4)) ? " MSTKERR"   : "",
               (mmfsr & (1U << 5)) ? " MLSPERR"   : "");
    }
    if (bfsr != 0U) {
        printf("  BusFault:%s%s%s%s%s%s\n",
               (bfsr & (1U << 0)) ? " IBUSERR"     : "",
               (bfsr & (1U << 1)) ? " PRECISERR"   : "",
               (bfsr & (1U << 2)) ? " IMPRECISERR" : "",
               (bfsr & (1U << 3)) ? " UNSTKERR"    : "",
               (bfsr & (1U << 4)) ? " STKERR"      : "",
               (bfsr & (1U << 5)) ? " LSPERR"      : "");
    }
    if (ufsr != 0U) {
        printf("  UsageFault:%s%s%s%s%s%s\n",
               (ufsr & (1U << 0)) ? " UNDEFINSTR" : "",
               (ufsr & (1U << 1)) ? " INVSTATE"   : "",
               (ufsr & (1U << 2)) ? " INVPC"      : "",
               (ufsr & (1U << 3)) ? " NOCP"       : "",
               (ufsr & (1U << 8)) ? " UNALIGNED"  : "",
               (ufsr & (1U << 9)) ? " DIVBYZERO"  : "");
    }

    /* Only meaningful when the corresponding valid bit is set; printing them
     * unconditionally, as the old version did with MMFAR, invites reading a
     * stale address as if it were this fault's. */
    if ((mmfsr & (1U << 7)) != 0U) {
        printf("  MMFAR = 0x%08lX\n", (unsigned long) SCB->MMFAR);
    }
    if ((bfsr & (1U << 7)) != 0U) {
        printf("  BFAR  = 0x%08lX\n", (unsigned long) SCB->BFAR);
    }
    if ((bfsr & (1U << 2)) != 0U) {
        printf("  (imprecise: the faulting store already retired, so PC points\n"
               "   at whatever was executing when the error caught up, not at\n"
               "   the access itself)\n");
    }

    fflush(stdout);
    while (1) { }
}
/* USER CODE END 1 */

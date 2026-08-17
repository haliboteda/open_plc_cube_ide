/* USER CODE BEGIN Header */
/**
 ******************************************************************************
 * @file           : main.c
 * @brief          : Main program body
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
#include "usart.h"
#include "gpio.h"

/* Private includes ----------------------------------------------------------*/
/* USER CODE BEGIN Includes */
/* Phase 2 peripherals: CubeMX no longer emits their init calls. */
#include "rtc.h"
#include "crc.h"
#include "fmc.h"
#include "usb_device.h"
#include "lwip.h"

#include "relay.h"
#include "IAP_server.h"
#include "IAP_boot_handoff.h"
#include "iap_auth.h"
/* USER CODE END Includes */

/* Private typedef -----------------------------------------------------------*/
/* USER CODE BEGIN PTD */
#ifdef __GNUC__
#define PUTCHAR_PROTOTYPE int __io_putchar(int ch)
#else
#define PUTCHAR_PROTOTYPE int fputc(int ch, FILE *f)
#endif /* __GNUC__ */
/* USER CODE END PTD */

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
void SystemClock_Config(void);
static void MPU_Config(void);
/* USER CODE BEGIN PFP */

/* Bounded stand-in for ITM_SendChar(): never spin forever on the trace FIFO. */
static void itm_putchar_bounded(uint8_t ch)
{
	if (((ITM->TCR & ITM_TCR_ITMENA_Msk) == 0UL) || ((ITM->TER & 1UL) == 0UL)) {
		return;
	}
	for (uint32_t spins = 0U; spins < 1000U; spins++) {
		if (ITM->PORT[0U].u32 != 0UL) {
			ITM->PORT[0U].u8 = ch;
			return;
		}
	}
}

/* stdout goes to SWO/ITM and RS232, both bounded: a bootloader must never hang
 * on a log line. */
PUTCHAR_PROTOTYPE {
	itm_putchar_bounded((uint8_t) ch);
	(void) HAL_UART_Transmit(&huart4, (uint8_t*) &ch, 1, 10);
	return ch;
}
/* USER CODE END PFP */

/* Private user code ---------------------------------------------------------*/
/* USER CODE BEGIN 0 */

/* What server_decide() settled on. Phase 2 and the superloop key off it. */
static IAP_Method s_boot_mode = IAP_NONE;

/* Startup window: the relay clicks are the operator's cue to press BOOT0.
 * 3 relays x 500 ms = 1.5 s, then one read. */
#define BOOT0_WINDOW_MS        500U
#define BOOT0_WINDOW_RELAYS    3U

static uint8_t boot_window_relay(void)
{
	for (uint32_t i = 0U; i < BOOT0_WINDOW_RELAYS; i++) {
		Relay_On((RELAY_Name) i);
		HAL_Delay(BOOT0_WINDOW_MS);
		Relay_Off((RELAY_Name) i);
	}
	return boot0_is_pressed();
}

/* USER CODE END 0 */

/**
  * @brief  The application entry point.
  * @retval int
  */
int main(void)
{

  /* USER CODE BEGIN 1 */

  /* Latch the reset cause before anything can clear RCC->RSR. The cold-boot
   * test that keeps us from reading never-written ECC SRAM depends on it. */
  boot_handoff_latch_reset_cause();

  /* Enable the three configurable fault exceptions. Left at their reset default
   * they stay disabled and every MemManage / BusFault / UsageFault escalates to
   * HardFault, so IT_Fault_Report() can only ever report "HardFault" and the
   * stacked PC is the escalation point rather than the faulting instruction.
   * Enabled, each fault takes its own vector and the report names it directly.
   *
   * First statement in main(): SCB needs no clock and no init, so this covers
   * the whole boot. A fault before MX_UART4_Init() still reports over SWO/ITM. */
  SCB->SHCSR |= SCB_SHCSR_MEMFAULTENA_Msk | SCB_SHCSR_BUSFAULTENA_Msk
              | SCB_SHCSR_USGFAULTENA_Msk;

  /* Initialise the D-cache RAMs even though the D-cache stays off. They come up
   * random at power-on, tag and ECC bits included, and any invalidate-by-address
   * has to look a tag up before it can act -- landing on a random entry whose
   * ECC does not check out raises an imprecise BusFault. This pass walks the
   * cache by set and way, which writes the tags instead of looking them up, so
   * it is safe on an uninitialised cache; SCB_EnableDCache() opens with the same
   * step for the same reason. Costs tens of microseconds, once. */
  SCB_InvalidateDCache();

  /* USER CODE END 1 */

  /* MPU Configuration--------------------------------------------------------*/
  MPU_Config();

  /* Enable the CPU Cache */

  /* Enable I-Cache---------------------------------------------------------*/
  SCB_EnableICache();

  /* MCU Configuration--------------------------------------------------------*/

  /* Reset of all peripherals, Initializes the Flash interface and the Systick. */
  HAL_Init();

  /* USER CODE BEGIN Init */

  /* USER CODE END Init */

  /* USER CODE BEGIN SysInit */

  SystemClock_Config();

  /* Phase 1: decide. Only what the decision itself needs is brought up here, so
   * the application inherits none of our peripheral state on the jump path. */
  MX_GPIO_Init();      /* BOOT0 net, relay outputs, RS232_Enable */
  BOOT0_ConfigureAsInput();  /* undo the generated output config on PG9 */
  Enable_RX_RS232();   /* the MAX3221 stays in shutdown until this pin is high */
  MX_UART4_Init();

  printf("** Checking Starting Mod ...\r\n"
		  "** (IF You want OpenPLC to stay in upload mode, please hold down the BOOT0 button for 3-5 seconds while clicking)\r\n");

  printf("** Reset cause: %s\r\n", boot_handoff_reset_cause_str());

  s_boot_mode = server_decide(boot_window_relay());

  if (s_boot_mode == IAP_NONE) {
    server_jump_to_app();   /* hands the hardware back; never returns */
  }
  /* USER CODE END SysInit */

  /* Initialize all configured peripherals */
  MX_GPIO_Init();
  /* USER CODE BEGIN 2 */

  /* Phase 2: staying in the bootloader. */

  /* MX_GPIO_Init() above drove RS232_Enable low again. */
  Enable_RX_RS232();

  MX_RTC_Init();   /* iap_auth's nonce counter lives in a backup register */
  iap_auth_report_backup_domain();
  MX_CRC_Init();   /* upload checksum */
  MX_FMC_Init();   /* external SDRAM: staging area for the incoming image */

  /* IAP_ALL means the channel is unknown, so open both rather than guess. */
  if ((s_boot_mode == IAP_CDC) || (s_boot_mode == IAP_ALL)) {
    MX_USB_DEVICE_Init();
  }
  if ((s_boot_mode == IAP_ETHERNET) || (s_boot_mode == IAP_ALL)) {
    MX_LWIP_Init();
  }

  IAP_servers_start(s_boot_mode);
  /* USER CODE END 2 */

  /* Infinite loop */
  /* USER CODE BEGIN WHILE */
  while (1) {
    /* USER CODE END WHILE */

    /* USER CODE BEGIN 3 */
    IAP_task();
    /* MX_LWIP_Init() only ran for the ethernet modes. */
    if ((s_boot_mode == IAP_ETHERNET) || (s_boot_mode == IAP_ALL)) {
      MX_LWIP_Process();
    }
    HAL_Delay(10);
  }
  /* USER CODE END 3 */
}

/**
  * @brief System Clock Configuration
  * @retval None
  */
void SystemClock_Config(void)
{
  RCC_OscInitTypeDef RCC_OscInitStruct = {0};
  RCC_ClkInitTypeDef RCC_ClkInitStruct = {0};

  /** Supply configuration update enable
  */
  HAL_PWREx_ConfigSupply(PWR_LDO_SUPPLY);

  /** Configure the main internal regulator output voltage
  */
  __HAL_PWR_VOLTAGESCALING_CONFIG(PWR_REGULATOR_VOLTAGE_SCALE1);

  while(!__HAL_PWR_GET_FLAG(PWR_FLAG_VOSRDY)) {}

  /** Initializes the RCC Oscillators according to the specified parameters
  * in the RCC_OscInitTypeDef structure.
  */
  RCC_OscInitStruct.OscillatorType = RCC_OSCILLATORTYPE_HSI48|RCC_OSCILLATORTYPE_LSI
                              |RCC_OSCILLATORTYPE_HSE;
  RCC_OscInitStruct.HSEState = RCC_HSE_ON;
  RCC_OscInitStruct.LSIState = RCC_LSI_ON;
  RCC_OscInitStruct.HSI48State = RCC_HSI48_ON;
  RCC_OscInitStruct.PLL.PLLState = RCC_PLL_ON;
  RCC_OscInitStruct.PLL.PLLSource = RCC_PLLSOURCE_HSE;
  RCC_OscInitStruct.PLL.PLLM = 2;
  RCC_OscInitStruct.PLL.PLLN = 64;
  RCC_OscInitStruct.PLL.PLLP = 2;
  RCC_OscInitStruct.PLL.PLLQ = 2;
  RCC_OscInitStruct.PLL.PLLR = 2;
  RCC_OscInitStruct.PLL.PLLRGE = RCC_PLL1VCIRANGE_3;
  RCC_OscInitStruct.PLL.PLLVCOSEL = RCC_PLL1VCOWIDE;
  RCC_OscInitStruct.PLL.PLLFRACN = 0;
  if (HAL_RCC_OscConfig(&RCC_OscInitStruct) != HAL_OK)
  {
    Error_Handler();
  }

  /** Initializes the CPU, AHB and APB buses clocks
  */
  RCC_ClkInitStruct.ClockType = RCC_CLOCKTYPE_HCLK|RCC_CLOCKTYPE_SYSCLK
                              |RCC_CLOCKTYPE_PCLK1|RCC_CLOCKTYPE_PCLK2
                              |RCC_CLOCKTYPE_D3PCLK1|RCC_CLOCKTYPE_D1PCLK1;
  RCC_ClkInitStruct.SYSCLKSource = RCC_SYSCLKSOURCE_PLLCLK;
  RCC_ClkInitStruct.SYSCLKDivider = RCC_SYSCLK_DIV1;
  RCC_ClkInitStruct.AHBCLKDivider = RCC_HCLK_DIV2;
  RCC_ClkInitStruct.APB3CLKDivider = RCC_APB3_DIV2;
  RCC_ClkInitStruct.APB1CLKDivider = RCC_APB1_DIV2;
  RCC_ClkInitStruct.APB2CLKDivider = RCC_APB2_DIV2;
  RCC_ClkInitStruct.APB4CLKDivider = RCC_APB4_DIV2;

  if (HAL_RCC_ClockConfig(&RCC_ClkInitStruct, FLASH_LATENCY_2) != HAL_OK)
  {
    Error_Handler();
  }
}

/* USER CODE BEGIN 4 */

/* USER CODE END 4 */

 /* MPU Configuration */

void MPU_Config(void)
{
  MPU_Region_InitTypeDef MPU_InitStruct = {0};

  /* Disables the MPU */
  HAL_MPU_Disable();

  /** Initializes and configures the Region and the memory to be protected
  */
  MPU_InitStruct.Enable = MPU_REGION_ENABLE;
  MPU_InitStruct.Number = MPU_REGION_NUMBER0;
  MPU_InitStruct.BaseAddress = 0x0;
  MPU_InitStruct.Size = MPU_REGION_SIZE_4GB;
  MPU_InitStruct.SubRegionDisable = 0xC7;
  MPU_InitStruct.TypeExtField = MPU_TEX_LEVEL0;
  MPU_InitStruct.AccessPermission = MPU_REGION_NO_ACCESS;
  MPU_InitStruct.DisableExec = MPU_INSTRUCTION_ACCESS_DISABLE;
  MPU_InitStruct.IsShareable = MPU_ACCESS_SHAREABLE;
  MPU_InitStruct.IsCacheable = MPU_ACCESS_NOT_CACHEABLE;
  MPU_InitStruct.IsBufferable = MPU_ACCESS_NOT_BUFFERABLE;

  HAL_MPU_ConfigRegion(&MPU_InitStruct);

  /** Initializes and configures the Region and the memory to be protected
  */
  MPU_InitStruct.Number = MPU_REGION_NUMBER1;
  MPU_InitStruct.BaseAddress = 0x30000000;
  MPU_InitStruct.Size = MPU_REGION_SIZE_128KB;
  MPU_InitStruct.SubRegionDisable = 0x0;
  MPU_InitStruct.TypeExtField = MPU_TEX_LEVEL1;
  MPU_InitStruct.AccessPermission = MPU_REGION_FULL_ACCESS;
  MPU_InitStruct.IsShareable = MPU_ACCESS_NOT_SHAREABLE;

  HAL_MPU_ConfigRegion(&MPU_InitStruct);

  /** Initializes and configures the Region and the memory to be protected
  */
  MPU_InitStruct.Number = MPU_REGION_NUMBER2;
  MPU_InitStruct.BaseAddress = 0x30020000;
  MPU_InitStruct.Size = MPU_REGION_SIZE_256B;
  MPU_InitStruct.TypeExtField = MPU_TEX_LEVEL0;
  MPU_InitStruct.IsShareable = MPU_ACCESS_SHAREABLE;
  MPU_InitStruct.IsBufferable = MPU_ACCESS_BUFFERABLE;

  HAL_MPU_ConfigRegion(&MPU_InitStruct);
  /* Enables the MPU */
  HAL_MPU_Enable(MPU_PRIVILEGED_DEFAULT);

}

/**
  * @brief  Period elapsed callback in non blocking mode
  * @note   This function is called  when TIM6 interrupt took place, inside
  * HAL_TIM_IRQHandler(). It makes a direct call to HAL_IncTick() to increment
  * a global variable "uwTick" used as application time base.
  * @param  htim : TIM handle
  * @retval None
  */
void HAL_TIM_PeriodElapsedCallback(TIM_HandleTypeDef *htim)
{
  /* USER CODE BEGIN Callback 0 */

  /* USER CODE END Callback 0 */
  if (htim->Instance == TIM6)
  {
    HAL_IncTick();
  }
  /* USER CODE BEGIN Callback 1 */

  /* USER CODE END Callback 1 */
}

/**
  * @brief  This function is executed in case of error occurrence.
  * @retval None
  */
void Error_Handler(void)
{
  /* USER CODE BEGIN Error_Handler_Debug */
	/* User can add his own implementation to report the HAL error return state */
	__disable_irq();
	while (1) {
	}
  /* USER CODE END Error_Handler_Debug */
}
#ifdef USE_FULL_ASSERT
/**
  * @brief  Reports the name of the source file and the source line number
  *         where the assert_param error has occurred.
  * @param  file: pointer to the source file name
  * @param  line: assert_param error line source number
  * @retval None
  */
void assert_failed(uint8_t *file, uint32_t line)
{
  /* USER CODE BEGIN 6 */
  /* User can add his own implementation to report the file name and line number,
     ex: printf("Wrong parameters value: file %s on line %d\r\n", file, line) */
  /* USER CODE END 6 */
}
#endif /* USE_FULL_ASSERT */

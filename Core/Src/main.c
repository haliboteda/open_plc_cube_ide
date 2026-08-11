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
/* These peripherals are no longer called from the generated init block ("Do not
 * generate function call" in CubeMX), so main.c has to reach their MX_*_Init()
 * itself -- see Phase 2 below for when each one is actually needed. */
#include "rtc.h"
#include "crc.h"
#include "fmc.h"
#include "usb_device.h"
#include "lwip.h"

#include "relay.h"
#include "IAP_server.h"
#include "pwm_test.h"
#include "rs232_test.h"
#include "adc_test.h"
#include "sdram_test.h"
#include "sd_test.h"
/* USER CODE END Includes */

/* Private typedef -----------------------------------------------------------*/
/* USER CODE BEGIN PTD */
#ifdef __GNUC__
/* With GCC/RAISONANCE, small printf (option LD Linker->Libraries->Small printf
 set to 'Yes') calls __io_putchar() */
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
// RS232
/*
 * Bounded stand-in for CMSIS ITM_SendChar(): that one spins forever waiting for
 * the trace FIFO if ITM is enabled but nothing is draining it -- e.g. a debug
 * session that died with the enable bits still set. That is the same hang we
 * remove from the UART path below, so it would be odd to add it back here.
 */
static void itm_putchar_bounded(uint8_t ch)
{
	if (((ITM->TCR & ITM_TCR_ITMENA_Msk) == 0UL) || ((ITM->TER & 1UL) == 0UL)) {
		return;   /* no debugger has enabled the trace port -- nothing to do */
	}
	for (uint32_t spins = 0U; spins < 1000U; spins++) {
		if (ITM->PORT[0U].u32 != 0UL) {
			ITM->PORT[0U].u8 = ch;
			return;
		}
	}
	/* FIFO never drained: drop the character rather than wedge the board. */
}

PUTCHAR_PROTOTYPE {
	/* Two sinks. SWO/ITM costs nothing when no debugger is attached and lets the
	 * boot log be read in CubeIDE's SWV console without wiring up an RS232
	 * adapter; RS232 stays for field diagnostics, where there is no debugger. */
	itm_putchar_bounded((uint8_t) ch);

	/* Bounded timeout rather than HAL_MAX_DELAY: a bootloader is the last place
	 * that should be able to hang forever on a log line. Losing a character is
	 * strictly better than wedging the board. */
	(void) HAL_UART_Transmit(&huart4, (uint8_t*) &ch, 1, 10);
	return ch;
}
/* USER CODE END PFP */

/* Private user code ---------------------------------------------------------*/
/* USER CODE BEGIN 0 */
void array_prinf(unsigned char *data, unsigned int len) {
	printf("{\r\n");
	for (int i = 0; i < len; i++) {
		printf("0x%02x,", data[i]);

		if ((len % 8) == 7)
			printf("\r\n");
	}
	printf("}\r\n");
}

/* ===================== TEMPORARY boot diagnostics ==========================
 *
 * Here to chase the "reset button does nothing until you click it several times
 * quickly" fault. Delete this block, its call in main() and the BOOT: line once
 * that is settled.
 *
 * boot_diag_first_breath() is the very first thing main() runs -- before the
 * MPU, before the clock tree, before HAL_Init() -- so it touches raw registers
 * only and can rely on nothing. Two jobs:
 *
 *   1. Latch RCC->RSR while it is still intact. boot_handoff_take() clears it
 *      later, and the flags say what kind of reset this actually was: a real
 *      pin reset from SW1, or a power-on / brown-out (which would mean the
 *      button press is dropping the rail rather than pulling NRST).
 *
 *   2. Click RY1 at four points along the boot, so a boot that dies before
 *      UART4 exists still says *where* it died. Count the slow clicks that come
 *      before the one-second silence:
 *
 *        0  never reached main() -- so not this commit's doing: nothing in it
 *           touches the startup file, the system file, _estack or where .data
 *           and .bss live. Suspect the CPU never left reset.
 *        1  died in MPU_Config() or HAL_Init()
 *        2  died in SystemClock_Config()   <- Error_Handler() traps here, and
 *                                             it is a silent __disable_irq()
 *                                             plus while(1)
 *        3  died in MX_GPIO_Init() / Enable_RX_RS232() / MX_UART4_Init()
 *        4  boot is fine this far; the startup window and the log follow
 *
 *      Calibrate first: on a boot that works, all four have to be audible and
 *      countable. If they are not, this probe proves nothing and the earlier
 *      "no sound means the CPU never ran" reading was worthless.
 */
static uint32_t s_reset_flags;

/* Before HAL_Init() there is no SysTick, so this one busy-waits. ~250 ms at the
 * 64 MHz reset-default clock, which is also the only clock it ever runs at. */
static void boot_diag_click_raw(void)
{
	volatile uint32_t spin;

	/* RY1 is PI8. Enable GPIOI, read back so the enable has landed before the
	 * first access, then drive PI8 as a plain push-pull output. */
	RCC->AHB4ENR |= RCC_AHB4ENR_GPIOIEN;
	(void)RCC->AHB4ENR;
	GPIOI->MODER = (GPIOI->MODER & ~(3UL << (8U * 2U))) | (1UL << (8U * 2U));

	GPIOI->BSRR = GPIO_PIN_8;
	for (spin = 0U; spin < 1800000U; spin++) { }
	GPIOI->BSRR = (uint32_t)GPIO_PIN_8 << 16U;
	for (spin = 0U; spin < 1800000U; spin++) { }
}

/* Once HAL_Init() has run, SysTick gives the same 250 ms at either clock:
 * HAL_RCC_ClockConfig() re-runs HAL_InitTick(), so this stays honest across
 * SystemClock_Config() taking the core from 64 MHz to 400 MHz. */
static void boot_diag_click(void)
{
	GPIOI->BSRR = GPIO_PIN_8;
	HAL_Delay(250);
	GPIOI->BSRR = (uint32_t)GPIO_PIN_8 << 16U;
	HAL_Delay(250);
}
/* =================== end TEMPORARY boot diagnostics ======================== */

/* What server_decide() settled on. Phase 2 and the superloop both key off it. */
static IAP_Method s_boot_mode = IAP_NONE;

/*
 * Startup relay window.
 *
 * The clicking is not decoration: it IS the window in which the operator can
 * press BOOT0 to interrupt the normal boot, and the only cue that the window is
 * open. The board gives no other feedback with nothing plugged in.
 *
 * So the pin is polled for the whole duration and the result latched, rather
 * than read once when the clicking stops. A press-and-release anywhere inside
 * the window counts. (Reading the pin a few milliseconds after reset, as an
 * earlier version of this did, asks for a reaction time no human has -- the
 * decision was already made and the board had jumped to the application before
 * a finger could get there.)
 *
 * All six relays, 500 ms each, is 3 s of window -- long enough to match the
 * startup banner's "hold down the BOOT0 button for 3-5 seconds while clicking",
 * and long enough that the operator hears several clicks before having to
 * decide. BOOT0_WINDOW_STEP_MS x BOOT0_WINDOW_STEPS is the per-relay slice.
 */
#define BOOT0_WINDOW_STEP_MS   10U
#define BOOT0_WINDOW_STEPS     50U   /* 50 x 10 ms = 500 ms per relay */

static uint8_t boot_window_relay(void)
{
	uint8_t pressed = 0U;

	for (RELAY_Name relay = RELAY_1; relay < RELAY_COUNT; ++relay) {
		Relay_On(relay);
		for (uint32_t step = 0U; step < BOOT0_WINDOW_STEPS; step++) {
			HAL_Delay(BOOT0_WINDOW_STEP_MS);
			if (boot0_is_pressed()) {
				pressed = 1U;   /* latch: do not require it to still be held */
			}
		}
		Relay_Off(relay);
	}
	return pressed;
}

/* USER CODE END 0 */

/**
  * @brief  The application entry point.
  * @retval int
  */
int main(void)
{

  /* USER CODE BEGIN 1 */
  /* TEMPORARY -- mark 1. Latch RSR before anything can clear it. */
  s_reset_flags = RCC->RSR;
  boot_diag_click_raw();
  /* USER CODE END 1 */

  /* MPU Configuration--------------------------------------------------------*/
  MPU_Config();

  /* MCU Configuration--------------------------------------------------------*/

  /* Reset of all peripherals, Initializes the Flash interface and the Systick. */
  HAL_Init();

  /* USER CODE BEGIN Init */
  boot_diag_click();   /* TEMPORARY -- mark 2 */
  /* USER CODE END Init */

  /* Configure the system clock */
  SystemClock_Config();

  /* USER CODE BEGIN SysInit */
  /* This call is not optional, however little of the clock tree Phase 1 seems
   * to need. Without it the whole bootloader runs on the reset defaults --
   * VOS3, HSI at 64 MHz, FLASH_ACR.LATENCY = 0 -- and that combination is
   * outside the reference manual's wait-state table, which allows 0 WS at VOS3
   * only up to 45 MHz. A stock project passes through the same state, but only
   * for the few hundred instructions before this line and with the I-cache on;
   * here every instruction fetch for the entire boot would read flash out of
   * spec, which showed up as the board locking up silently on roughly one reset
   * in a few. Two more things depend on it: MX_FMC_Init()'s SDRAM refresh count
   * is computed for a 200 MHz HCLK, and USB needs HSI48.
   *
   * It costs the application nothing: server_jump_to_app() calls
   * HAL_RCC_DeInit() before jumping, so the clock tree is back at its reset
   * defaults by the time the application's own SystemClock_Config() runs.
   */

  /* ------------------------- Phase 1: decide ------------------------------
   *
   * Only what the decision itself needs comes up here. USB, lwIP, RTC, CRC and
   * FMC all wait for Phase 2, which runs *only* when we stay in the bootloader.
   * That is what keeps the application from inheriting our peripheral state:
   * "never initialised" cannot be got wrong, whereas a de-init list has to be
   * maintained by hand and rots silently. It also means enabling FMC for the
   * SDRAM staging area can never leak into a user's sketch.
   */
  boot_diag_click();   /* TEMPORARY -- mark 3 */

  MX_GPIO_Init();      /* BOOT0 input, relay outputs, RS232_Enable */
  Enable_RX_RS232();   /* the MAX3221 stays in shutdown until this pin is high, */
  MX_UART4_Init();     /* so without it the printf below reaches nothing at all */

  boot_diag_click();   /* TEMPORARY -- mark 4 */
  HAL_Delay(1000);     /* TEMPORARY -- the silence that ends the mark sequence */

  /* TEMPORARY -- reset cause, from the RSR latched before anything cleared it.
   * SYSCLK is on the same line to confirm SystemClock_Config() actually ran. */
  printf("\r\n** BOOT: RSR=%08lX%s%s%s%s%s%s%s SYSCLK=%lu\r\n",
         (unsigned long) s_reset_flags,
         (s_reset_flags & RCC_RSR_PORRSTF)   ? " POR"  : "",
         (s_reset_flags & RCC_RSR_BORRSTF)   ? " BOR"  : "",
         (s_reset_flags & RCC_RSR_PINRSTF)   ? " PIN"  : "",
         (s_reset_flags & RCC_RSR_SFTRSTF)   ? " SFT"  : "",
         (s_reset_flags & RCC_RSR_IWDG1RSTF) ? " IWDG" : "",
         (s_reset_flags & RCC_RSR_WWDG1RSTF) ? " WWDG" : "",
         (s_reset_flags & RCC_RSR_LPWRRSTF)  ? " LPWR" : "",
         (unsigned long) HAL_RCC_GetSysClockFreq());

  printf("** Checking Starting Mod ...\r\n"
		  "** (IF You want OpenPLC to stay in upload mode, please hold down the BOOT0 button for 3-5 seconds while clicking)\r\n");

  /* 3 s of relay clicking = the operator's window to press BOOT0. Polled
   * throughout, so a press anywhere inside it counts -- and a press ends the
   * decision there: server_decide() stays in the bootloader without consulting
   * the handoff request or the app signature. No press and the usual checks
   * decide, which is normally a jump to the application. */
  s_boot_mode = server_decide(boot_window_relay());

  if (s_boot_mode == IAP_NONE) {
    server_jump_to_app();   /* hands the hardware back; never returns */
  }
  /* USER CODE END SysInit */

  /* Initialize all configured peripherals */
  MX_GPIO_Init();
  /* USER CODE BEGIN 2 */
  /* ------------------- Phase 2: staying in the bootloader ------------------ */

  /* The generated MX_GPIO_Init() above runs a second time and pulls
   * RS232_Enable low again (gpio.c writes every output low before configuring
   * it), so re-assert it. Suppressing that call in CubeMX, or setting the pin's
   * default output level to High, would make this line unnecessary. */
  Enable_RX_RS232();

  MX_RTC_Init();   /* iap_auth's nonce counter lives in a backup register */
  MX_CRC_Init();   /* upload checksum */
  MX_FMC_Init();   /* external SDRAM: staging area for the incoming image */

  /* Bring up only the channel we were asked for. IAP_ALL means we could not tell
   * which one the operator is on, so open both rather than guess. */
  if ((s_boot_mode == IAP_CDC) || (s_boot_mode == IAP_ALL)) {
    MX_USB_DEVICE_Init();
  }
  if ((s_boot_mode == IAP_ETHERNET) || (s_boot_mode == IAP_ALL)) {
    MX_LWIP_Init();
  }

  // Only one of these runs forever and hijacks the boot - uncomment the one
  // you're currently bringing up, leave the rest commented out.
  // PWM_Test_Run();        // Digital Out 6 breathing LED
  //ADC_Test_Run();        // on-board temp sensors (PA0/PA3)
  //RS232_Test_Run();         // RS232 RX echo
  //SDRAM_Test_Capacity();            // external SDRAM: how big is it really?
  //SDRAM_Test_Retention();          // external SDRAM: random addr, wait 5s, read back
  //SDRAM_Test_CubeProgrammerVerify(); // external SDRAM: write via CubeProgrammer, verify CRC32 here
  //SD_Test_Info();                   // microSD: is a card detected, how big is it?
  //SD_Test_FileIntegrity();          // microSD: FatFs write+read-back+CRC32 on the inserted card

  IAP_servers_start(s_boot_mode);
  /* USER CODE END 2 */

  /* Infinite loop */
  /* USER CODE BEGIN WHILE */
  while (1) {
    /* USER CODE END WHILE */

    /* USER CODE BEGIN 3 */
    IAP_task();
    /* Guarded: MX_LWIP_Init() only ran for the ethernet modes, and calling
     * MX_LWIP_Process() on an uninitialised stack is not harmless. */
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

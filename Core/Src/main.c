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
#include "mbedtls.h"
#include "usart.h"
#include "usb_device.h"
#include "gpio.h"

/* Private includes ----------------------------------------------------------*/
/* USER CODE BEGIN Includes */
#include "relay.h"
#include "md5.h"
#include "usbd_cdc_flash.h"
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

typedef void (*pFunction)(void);
/* USER CODE END PTD */

/* Private define ------------------------------------------------------------*/
/* USER CODE BEGIN PD */

/* USER CODE END PD */

/* Private macro -------------------------------------------------------------*/
/* USER CODE BEGIN PM */

/* USER CODE END PM */

/* Private variables ---------------------------------------------------------*/

/* USER CODE BEGIN PV */
pFunction JumpToApplication;
uint32_t JumpAddress;
extern uint8_t FlagAppToBeUpgraded;
extern uint32_t BinFileSize;
extern uint32_t BinFileRXLen;     // bin file received length
extern uint8_t PackageNumTOFlash; // 0 - no 1 - yes
extern uint32_t LenInWRBuffer;    // data length to write to flash

extern uint8_t BinFileWRBuf[BIN_FILE_BUF_SIZE];
extern void reset_buf(void);
extern void empty_buf(uint8_t *Buf, uint32_t Len);

uint8_t FlashBuffer[BIN_FILE_BUF_SIZE];
/* USER CODE END PV */

/* Private function prototypes -----------------------------------------------*/
void SystemClock_Config(void);
/* USER CODE BEGIN PFP */
// RS232
PUTCHAR_PROTOTYPE
{
  /* Place your implementation of fputc here */
  /* e.g. write a character to the USART1 and Loop until the end of transmission */
  HAL_UART_Transmit(&huart4, (uint8_t *)&ch, 1, HAL_MAX_DELAY);
  return ch;
}
/* USER CODE END PFP */

/* Private user code ---------------------------------------------------------*/
/* USER CODE BEGIN 0 */
void array_prinf(unsigned char *data, unsigned int len)
{
  printf("{\r\n");
  for (int i = 0; i < len; i++)
  {
    printf("0x%02x,", data[i]);

    if ((len % 8) == 7)
      printf("\r\n");
  }
  printf("}\r\n");
}
//
void reset_param(void)
{
  reset_buf();
}

/* USER CODE END 0 */

/**
  * @brief  The application entry point.
  * @retval int
  */
int main(void)
{

  /* USER CODE BEGIN 1 */
  char menuNum = '2';
  /* USER CODE END 1 */

  /* MCU Configuration--------------------------------------------------------*/

  /* Reset of all peripherals, Initializes the Flash interface and the Systick. */
  HAL_Init();

  /* USER CODE BEGIN Init */

  /* USER CODE END Init */

  /* Configure the system clock */
  SystemClock_Config();

  /* USER CODE BEGIN SysInit */

  /* USER CODE END SysInit */

  /* Initialize all configured peripherals */
  MX_GPIO_Init();
  MX_MBEDTLS_Init();
  MX_USB_DEVICE_Init();
  MX_UART4_Init();
  /* USER CODE BEGIN 2 */
  Enable_RX_RS232();

  printf("******************************************************\r\n");
  printf("** After the click sound, please input the menu number to choose.\r\n");
  printf("** 1. OpenPLC board will stay in the CDC mode. \r\n");
  printf("** 2. Run the app directly.(default) \r\n");
  printf("**Please choose 1 or 2 in 5 seconds:");
  printf("******************************************************\r\n");

  // Einschalten aller HSFETs einmalig und dann ausschalten
  for (RELAY_Name relay = RELAY_1; relay < RELAY_COUNT; ++relay)
  {
    Relay_On(relay);
    HAL_Delay(500); // Einschaltverzoegerung von 500 Millisekunden
    Relay_Off(relay);
  }

    HAL_UART_Receive(&huart4, &menuNum, 1, 5000);
    switch (menuNum)
    {
    case '1':
      break;
    default:
      if (((*(__IO uint32_t *)CDC_APP_ADDRESS) & 0x2FFE0000) == 0x24080000)
      {
        printf("\r\n** Go to app.\r\n");
        /* Jump to user application */
        JumpAddress = *(__IO uint32_t *)(CDC_APP_ADDRESS + 4);

        JumpToApplication = (pFunction)JumpAddress;

        MX_USB_DEVICE_DeInit();
        HAL_RCC_DeInit();
        HAL_DeInit();

        // DeInitializing systick peripheral
        SysTick->CTRL = 0;
        SysTick->LOAD = 0;
        SysTick->VAL = 0;

        /* Initialize application Stack Pointer */
        __set_MSP(*(__IO uint32_t *)CDC_APP_ADDRESS);
        JumpToApplication();
      }
      else
      {
        printf("\r\n** NO APP! Stay in the CDC mode.\r\n");
      }
      break;
    }
  printf("** In the CDC mode.\r\n");
  /* USER CODE END 2 */

  /* Infinite loop */
  /* USER CODE BEGIN WHILE */
  uint32_t w_len = 0;
  //uint32_t packageNum = 0;
  while (1)
  {
    /* USER CODE END WHILE */

    /* USER CODE BEGIN 3 */
    // ready to write flash
    if (PackageNumTOFlash > 0)
    {
      //packageNum = PackageNumTOFlash;
      uint32_t LenToFlash = LenInWRBuffer;
      // copy WRBuffer to Flash buffer and empty it
      memcpy(FlashBuffer, BinFileWRBuf, LenToFlash);
      empty_buf(BinFileWRBuf, LenToFlash);
      // LenToFlash need 32-bit aligned
      int paddingSize = (32 - LenToFlash) & 31;
      printf("LenToFlash after aligned. %d\r\n", LenToFlash + paddingSize);
      HAL_StatusTypeDef status = Flash_If_Write(FlashBuffer, CDC_APP_ADDRESS + w_len, LenToFlash + paddingSize);
      if (status != HAL_OK)
      {
        printf("There is error when writing flash. Addr:%x Data:%x Status:%d\r\n", CDC_APP_ADDRESS + w_len, FlashBuffer, status);
        // clean
        w_len = 0;
        reset_param();
      }
      else
      {
        w_len += LenToFlash;
        PackageNumTOFlash= 0;
        //printf("Write Done. Addr:%x Len:%d Package %d of %d\r\n", CDC_APP_ADDRESS, w_len, packageNum, PackageNumTOFlash);
        printf("Done. Write Addr:%x %d (written:%d of total:%d) \r\n", CDC_APP_ADDRESS, LenToFlash + paddingSize, w_len, BinFileSize);
        //
        empty_buf(FlashBuffer, LenToFlash);
        LenToFlash = 0;
      }

//      if (PackageNumTOFlash - packageNum > 2)
//      {
//        printf("PackageNumTOFlash should be sequential. Otherwise there are some packages lost, which number are between %d and %d. Please check\r\n", packageNum, PackageNumTOFlash);
//        // clean
//        w_len = 0;
//        reset_param();
//      }
//      else if (PackageNumTOFlash - packageNum == 0)
//      {
        if (BinFileRXLen >= BinFileSize)
        {
          //        // all finished
          //        //
          //        // samples for MD5 usage
          //        int i;
          //        unsigned char decrypt[16];
          //        MD5_CTX md5;
          //        MD5Init(&md5);
          //        MD5Update(&md5, BinFileBuf_test, bin_file_size);
          //        MD5Final(&md5, decrypt);
          //        printf("DDD:");
          //        for (i = 0; i < 16; i++)
          //        {
          //          printf("%02x", decrypt[i]);
          //        }
          printf("All finished. Addr:%x Write Len:%ld. Bin file size:%ld \r\n", CDC_APP_ADDRESS, w_len, BinFileSize);
          // clean
          w_len = 0;
          reset_param();
        }
//      }

      printf("------------------------------------------------\r\n");

      fflush(stdout);
    }
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
  __HAL_PWR_VOLTAGESCALING_CONFIG(PWR_REGULATOR_VOLTAGE_SCALE3);

  while(!__HAL_PWR_GET_FLAG(PWR_FLAG_VOSRDY)) {}

  /** Initializes the RCC Oscillators according to the specified parameters
  * in the RCC_OscInitTypeDef structure.
  */
  RCC_OscInitStruct.OscillatorType = RCC_OSCILLATORTYPE_HSI48|RCC_OSCILLATORTYPE_HSE;
  RCC_OscInitStruct.HSEState = RCC_HSE_ON;
  RCC_OscInitStruct.HSI48State = RCC_HSI48_ON;
  RCC_OscInitStruct.PLL.PLLState = RCC_PLL_ON;
  RCC_OscInitStruct.PLL.PLLSource = RCC_PLLSOURCE_HSE;
  RCC_OscInitStruct.PLL.PLLM = 2;
  RCC_OscInitStruct.PLL.PLLN = 12;
  RCC_OscInitStruct.PLL.PLLP = 2;
  RCC_OscInitStruct.PLL.PLLQ = 2;
  RCC_OscInitStruct.PLL.PLLR = 2;
  RCC_OscInitStruct.PLL.PLLRGE = RCC_PLL1VCIRANGE_3;
  RCC_OscInitStruct.PLL.PLLVCOSEL = RCC_PLL1VCOMEDIUM;
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
  RCC_ClkInitStruct.AHBCLKDivider = RCC_HCLK_DIV1;
  RCC_ClkInitStruct.APB3CLKDivider = RCC_APB3_DIV2;
  RCC_ClkInitStruct.APB1CLKDivider = RCC_APB1_DIV2;
  RCC_ClkInitStruct.APB2CLKDivider = RCC_APB2_DIV2;
  RCC_ClkInitStruct.APB4CLKDivider = RCC_APB4_DIV2;

  if (HAL_RCC_ClockConfig(&RCC_ClkInitStruct, FLASH_LATENCY_1) != HAL_OK)
  {
    Error_Handler();
  }
}

/* USER CODE BEGIN 4 */

/* USER CODE END 4 */

/**
  * @brief  This function is executed in case of error occurrence.
  * @retval None
  */
void Error_Handler(void)
{
  /* USER CODE BEGIN Error_Handler_Debug */
  /* User can add his own implementation to report the HAL error return state */
  __disable_irq();
  while (1)
  {
  }
  /* USER CODE END Error_Handler_Debug */
}

#ifdef  USE_FULL_ASSERT
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

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
#include "rtc.h"
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

typedef enum {
	IDLE, FLASH_RECEIVE
} CDC_State;

CDC_State currentState = IDLE;
int bytesToReceive;
int calculatedChecksum;

uint8_t DataReadyFlag = 0; // 0 - no 1 - yes
uint32_t LenInRXBuf = 0;    // received data length in buffer
uint8_t BinFileRXBuf[CDC_RX_BUFFER_SIZE]; // buffer to receive data
void reset_buf(void);
void empty_buf(uint8_t *Buf, uint32_t Len);

extern USBD_HandleTypeDef hUsbDeviceFS;
//uint8_t FlashBuffer[BIN_FILE_BUF_SIZE];
/* USER CODE END PV */

/* Private function prototypes -----------------------------------------------*/
void SystemClock_Config(void);
/* USER CODE BEGIN PFP */
// RS232
PUTCHAR_PROTOTYPE {
	/* Place your implementation of fputc here */
	/* e.g. write a character to the USART1 and Loop until the end of transmission */
	HAL_UART_Transmit(&huart4, (uint8_t*) &ch, 1, HAL_MAX_DELAY);
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

/**
 * @brief Empty the buffer
 * @param Buf:buffer address
 * @param Len:buffer length
 */
void empty_buf(uint8_t *Buf, uint32_t Len) {
	int i;
	for (i = 0; i < Len; i++) {
		*(Buf + i) = '\0';
	}
}

// checksum
uint8_t calculateChecksum(uint32_t startAddress, uint16_t length) {
    uint8_t checksum = 0;
    for (uint32_t i = 0; i < length; i++) {
        checksum += *(volatile uint8_t*)(startAddress + i);
    }
    printf("checksum %d", checksum);
    fflush(stdout);
    return checksum;
}
/**
 * @brief reset all buffers and variables to original state
 * */
void reset_buf(void) {
	DataReadyFlag = 0;
	LenInRXBuf = 0;
	empty_buf(BinFileRXBuf, CDC_RX_BUFFER_SIZE); //
	currentState = IDLE;
}

void process_command() {
	// command list
	typedef enum {
		CMD_PING, CMD_INFO, CMD_FLASH, CMD_RUN, CMD_UNKNOWN
	} CommandType;

	CommandType cmd_type;
	char tx_buffer[128]; // send buffer
	if (currentState == IDLE) {
		// check the command type
		if (strncmp(BinFileRXBuf, "ping", 4) == 0) {
			cmd_type = CMD_PING;
		} else if (strncmp(BinFileRXBuf, "info", 4) == 0) {
			cmd_type = CMD_INFO;
		} else if (strncmp(BinFileRXBuf, "flash", 5) == 0) {
			cmd_type = CMD_FLASH;
		} else if (strncmp(BinFileRXBuf, "run", 3) == 0) {
			cmd_type = CMD_RUN;
		} else {
			cmd_type = CMD_UNKNOWN;
		}

		// process command
		switch (cmd_type) {
		case CMD_PING:
			// return OK
			strcpy(tx_buffer, "OK\r\n");
			CDC_Transmit_FS(tx_buffer, strlen(tx_buffer));
			break;

		case CMD_INFO:
			// return boot loader version
			strcpy(tx_buffer, BOOT_LOADER_VERSION);
			CDC_Transmit_FS(tx_buffer, strlen(tx_buffer));
			break;

		case CMD_FLASH: {
			// decode flash command
			if (sscanf(BinFileRXBuf, "flash %lu %lu", &bytesToReceive,
					&calculatedChecksum) == 2) {
				snprintf(tx_buffer, sizeof(tx_buffer),
						"File size %d, checksum %d. Wait for erasing flash! \r\n",
						bytesToReceive, calculatedChecksum);
				CDC_Transmit_FS(tx_buffer, strlen(tx_buffer));
				HAL_StatusTypeDef status = Erase_FLASH(CDC_APP_ADDRESS, bytesToReceive);
				HAL_Delay(1);
				if (status != HAL_OK) {
					snprintf(tx_buffer, sizeof(tx_buffer),
							"There is error when erasing flash. Addr:%x Len:%d Status:%d\r\n",
							CDC_APP_ADDRESS, LenInRXBuf, status);
					CDC_Transmit_FS(tx_buffer, strlen((char*) tx_buffer));
				} else {
					snprintf(tx_buffer, sizeof(tx_buffer),
							"Done! Begin to transfer bin file.\r\n");
					CDC_Transmit_FS(tx_buffer, strlen((char*) tx_buffer));
					currentState = FLASH_RECEIVE;
				}
			} else {
				strcpy(tx_buffer, "Invalid flash command\r\n");
				CDC_Transmit_FS(tx_buffer, strlen(tx_buffer));
			}
			break;
		}
		case CMD_RUN:
			// jump to App
			reset_buf();
			HAL_NVIC_SystemReset();
			break;
		default:
			strcpy(tx_buffer, "Unknown command\r\n");
			CDC_Transmit_FS(tx_buffer, strlen(tx_buffer));
			break;
		}
	} else if (currentState == FLASH_RECEIVE) {
		// receiving Flash bin file
		static uint16_t receivedBytes = 0;

		HAL_StatusTypeDef status = Flash_If_Write(BinFileRXBuf, CDC_APP_ADDRESS + receivedBytes, LenInRXBuf);
		if (status != HAL_OK) {
			snprintf(tx_buffer, sizeof(tx_buffer),
					"There is error when writing flash. Addr:%x Data:%x Status:%d\r\n",
					CDC_APP_ADDRESS + receivedBytes, BinFileRXBuf, status);
			CDC_Transmit_FS(tx_buffer, strlen(tx_buffer));
			// clean
			reset_buf();
		} else {
			receivedBytes += LenInRXBuf;
			snprintf(tx_buffer, sizeof(tx_buffer),
					"Done. Write Addr:%x size:%d (written:%d of total:%d) \r\n",
					CDC_APP_ADDRESS, LenInRXBuf, receivedBytes, bytesToReceive);
			CDC_Transmit_FS(tx_buffer, strlen(tx_buffer));
		}
		//
		if (receivedBytes >= bytesToReceive) {
			currentState = IDLE;
			snprintf(tx_buffer, sizeof(tx_buffer), "Flash complete\r\n");
			CDC_Transmit_FS(tx_buffer, strlen(tx_buffer));

			// checksum
			if (calculatedChecksum
					== calculateChecksum(CDC_APP_ADDRESS, bytesToReceive)) {
				snprintf(tx_buffer, sizeof(tx_buffer), "Checksum valid\r\n");
			} else {
				snprintf(tx_buffer, sizeof(tx_buffer), "Checksum invalid\r\n");
			}
			CDC_Transmit_FS(tx_buffer, strlen(tx_buffer));
		}
	}
}
/* USER CODE END 0 */

/**
 * @brief  The application entry point.
 * @retval int
 */
int main(void) {

	/* USER CODE BEGIN 1 */
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
	MX_RTC_Init();
	/* USER CODE BEGIN 2 */
	Enable_RX_RS232();

	CDC_Transmit_FS("** Checking Start Mod ...\r\n", 27);
	// Einschalten aller HSFETs einmalig und dann ausschalten
	for (RELAY_Name relay = RELAY_1; relay < RELAY_COUNT / 2; ++relay) {
		Relay_On(relay);
		HAL_Delay(500); // Einschaltverzoegerung von 500 Millisekunden
		Relay_Off(relay);
	}

	// check MAGIC_BKP_REG
	uint32_t readData = HAL_RTCEx_BKUPRead(&hrtc, MAGIC_BKP_REG);
	if (readData == MAGIC_BOOTLOADER_FLAG
			|| ((*(__IO uint32_t*) CDC_APP_ADDRESS) & 0x2FFE0000) != 0x24080000) {
		CDC_Transmit_FS("** Boot loader Mod ...\r\n", 24);
		CDC_Transmit_FS("** Please input your command: \r\n", 32);
		//HAL_PWR_EnableBkUpAccess();
		HAL_RTCEx_BKUPWrite(&hrtc, RTC_BKP_DR0, MAGIC_APP_FLAG);
	    //HAL_PWR_DisableBkUpAccess();
	} else {
		CDC_Transmit_FS("** App Mod ...\r\n", 16);
		/* Jump to user application */
		JumpAddress = *(__IO uint32_t*) (CDC_APP_ADDRESS + 4);

		JumpToApplication = (pFunction) JumpAddress;

		MX_USB_DEVICE_DeInit();
		HAL_RCC_DeInit();
		HAL_DeInit();

		// DeInitializing systick peripheral
		SysTick->CTRL = 0;
		SysTick->LOAD = 0;
		SysTick->VAL = 0;

		/* Initialize application Stack Pointer */
		__set_MSP(*(__IO uint32_t*) CDC_APP_ADDRESS);
		JumpToApplication();
	}

	/* USER CODE END 2 */

	/* Infinite loop */
	/* USER CODE BEGIN WHILE */
	while (1) {
		/* USER CODE END WHILE */

		/* USER CODE BEGIN 3 */
		// ready to write flash
		if (DataReadyFlag > 0) {
			DataReadyFlag = 0;
			process_command();
		}
	}
	/* USER CODE END 3 */
}

/**
 * @brief System Clock Configuration
 * @retval None
 */
void SystemClock_Config(void) {
	RCC_OscInitTypeDef RCC_OscInitStruct = { 0 };
	RCC_ClkInitTypeDef RCC_ClkInitStruct = { 0 };

	/** Supply configuration update enable
	 */
	HAL_PWREx_ConfigSupply(PWR_LDO_SUPPLY);

	/** Configure the main internal regulator output voltage
	 */
	__HAL_PWR_VOLTAGESCALING_CONFIG(PWR_REGULATOR_VOLTAGE_SCALE3);

	while (!__HAL_PWR_GET_FLAG(PWR_FLAG_VOSRDY)) {
	}

	/** Initializes the RCC Oscillators according to the specified parameters
	 * in the RCC_OscInitTypeDef structure.
	 */
	RCC_OscInitStruct.OscillatorType = RCC_OSCILLATORTYPE_HSI48
			| RCC_OSCILLATORTYPE_LSI | RCC_OSCILLATORTYPE_HSE;
	RCC_OscInitStruct.HSEState = RCC_HSE_ON;
	RCC_OscInitStruct.LSIState = RCC_LSI_ON;
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
	if (HAL_RCC_OscConfig(&RCC_OscInitStruct) != HAL_OK) {
		Error_Handler();
	}

	/** Initializes the CPU, AHB and APB buses clocks
	 */
	RCC_ClkInitStruct.ClockType = RCC_CLOCKTYPE_HCLK | RCC_CLOCKTYPE_SYSCLK
			| RCC_CLOCKTYPE_PCLK1 | RCC_CLOCKTYPE_PCLK2 | RCC_CLOCKTYPE_D3PCLK1
			| RCC_CLOCKTYPE_D1PCLK1;
	RCC_ClkInitStruct.SYSCLKSource = RCC_SYSCLKSOURCE_PLLCLK;
	RCC_ClkInitStruct.SYSCLKDivider = RCC_SYSCLK_DIV1;
	RCC_ClkInitStruct.AHBCLKDivider = RCC_HCLK_DIV1;
	RCC_ClkInitStruct.APB3CLKDivider = RCC_APB3_DIV2;
	RCC_ClkInitStruct.APB1CLKDivider = RCC_APB1_DIV2;
	RCC_ClkInitStruct.APB2CLKDivider = RCC_APB2_DIV2;
	RCC_ClkInitStruct.APB4CLKDivider = RCC_APB4_DIV2;

	if (HAL_RCC_ClockConfig(&RCC_ClkInitStruct, FLASH_LATENCY_1) != HAL_OK) {
		Error_Handler();
	}
}

/* USER CODE BEGIN 4 */

/* USER CODE END 4 */

/**
 * @brief  This function is executed in case of error occurrence.
 * @retval None
 */
void Error_Handler(void) {
	/* USER CODE BEGIN Error_Handler_Debug */
	/* User can add his own implementation to report the HAL error return state */
	__disable_irq();
	while (1) {
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

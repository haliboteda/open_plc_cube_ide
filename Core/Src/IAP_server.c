/*
 * ota_processor.c
 *
 *  Created on: Apr 30, 2025
 *      Author: ziotier
 */
#include <IAP_server.h>
#include "crc.h"
#include "md5.h"

#include "main.h"

#include "rtc.h"
#include "usbd_cdc_flash.h"
#include "usb_device.h"

#include "tcp_server_s.h"
#include "usbd_cdc_if.h"

pFunction JumpToApplication;

uint32_t bytesToReceive;
uint32_t expectedChecksum;

uint8_t DataReadyFlag = 0; // 0 - no 1 - yes
uint32_t LenInRXBuffer = 0;    // received data length in buffer
uint8_t RXBuffer[IAP_RX_BUFFER_SIZE]; // buffer to receive data
uint32_t receivedBytes = 0; // received Bytes of Flash bin file

//uint8_t FlashBuffer[IAP_RX_BUFFER_SIZE];
//uint32_t LenInFlashBuffer = 0;
IAP_State currentState = IDLE;

static IAP_Method current_method = IAP_NONE;

//
extern uint32_t _sdata, _edata;
extern uint32_t _sbss, _ebss;
extern uint32_t _estack;
extern uint16_t Erase_FLASH(uint8_t *flashAddress, uint32_t Len);
extern uint16_t Flash_If_Write(uint8_t *DataAddress, uint8_t *FlashAddress, uint32_t Len);
//

void Reset_Buf(void) {
	DataReadyFlag = 0;
	LenInRXBuffer = 0;
	receivedBytes = 0;
	memset(RXBuffer, 0, IAP_RX_BUFFER_SIZE); //
	currentState = IDLE;
	current_method = IAP_NONE;
}

// 接口特定的发送响应
void Send_Response(char *msg) {
	switch (current_method) {
	case IAP_CDC:
		CDC_Transmit_FS((uint8_t*) msg, strlen(msg));
		break;
	case IAP_ETHERNET:
		tcp_server_send(msg, strlen(msg));
		break;
	default:
		break;
	}
}

void Process_Command() {
	if (currentState == IDLE) {
		// check the command type
		if (strncmp(RXBuffer, "ping", 4) == 0) { //
			Send_Response("OK");
		} else if (strncmp(RXBuffer, "info", 4) == 0) { //
			Send_Response(BOOT_LOADER_VERSION);
		} else if (strncmp(RXBuffer, "run", 3) == 0) { //
			// jump to App
			Reset_Buf();
			HAL_NVIC_SystemReset();
		} else if (strncmp(RXBuffer, "flash", 5) == 0) {			//
			// decode flash command
			if (sscanf(RXBuffer, "flash %lu %x", &bytesToReceive,
					&expectedChecksum) == 2) {			//
				printf(
						"File size %d, checksum %x. Wait for erasing flash! \r\n",
						bytesToReceive, expectedChecksum);


				taskENTER_CRITICAL();  // 这比 vTaskSuspendAll 更彻底，能屏蔽中断
				SCB_CleanInvalidateDCache();  // 清除 DCache，防止写入失败

				HAL_FLASH_Unlock();
				HAL_StatusTypeDef status = Erase_FLASH((uint8_t *)IAP_APP_ADDRESS, bytesToReceive);
				HAL_FLASH_Lock();

				SCB_CleanInvalidateDCache();  // 擦除完再次清理 Cache
				taskEXIT_CRITICAL();
				//
				if (status != HAL_OK) {
					printf("Errors when erasing flash. Status:%d\r\n", status);

					Send_Response("ERR");
				} else {

					printf("Done! Begin to transfer bin file.\r\n");
					currentState = FLASH_RECEIVE;

					Send_Response("OK");
				}



			} else {
				printf("Invalid flash command");
			}
		} else {
			Send_Response("Unknown command");
		}
		LenInRXBuffer = 0;
	} else if (currentState == FLASH_RECEIVE) {
		HAL_StatusTypeDef status = HAL_OK;//Flash_If_Write(RXBuffer, IAP_APP_ADDRESS + receivedBytes, LenInRXBuffer);			//
		if (status != HAL_OK) {
			printf("There is error when writing flash. Addr:%x Status:%d\r\n",
			IAP_APP_ADDRESS + receivedBytes, status);

			// clean
			Reset_Buf();

			Send_Response("Failed");
		} else {

			receivedBytes += LenInRXBuffer;			//LenInFlashBuffer;
			memset(RXBuffer, 0, LenInRXBuffer);			//
			printf("Done. Write Addr:%x size:%d (written:%d of total:%d) \r\n",
			IAP_APP_ADDRESS, LenInRXBuffer, receivedBytes, bytesToReceive);
			LenInRXBuffer = 0;

			Send_Response("OK");
		}
		//
		if (receivedBytes >= bytesToReceive) {
			//currentState = IDLE;
			printf("Flash complete\r\n");

			// CRC checksum -- damn result should be inverted!!
			uint32_t caledCRC = HAL_CRC_Calculate(&hcrc, (uint32_t *)IAP_APP_ADDRESS,
					bytesToReceive);
			if (expectedChecksum == ~caledCRC) {
				printf("Checksum valid and RESET in 0.5 Seconds\r\n");
				HAL_Delay(500);
				HAL_NVIC_SystemReset();
			} else {
				printf("Checksum invalid expected: %x, actual: %x\r\n",
						expectedChecksum, ~caledCRC);
			}
			Reset_Buf();
		}
	}
	fflush(stdout);
}

//
uint8_t Check_Boot0_Pressed(void) {
	// check if boot0 pin (PG9) is pressed
	if (HAL_GPIO_ReadPin(BOOT0_GPIO_Port, BOOT0_Pin) == GPIO_PIN_SET) {
		printf("BOOT0 button is pressed!\r\n");
		return 1;  //
	} else {
		printf("BOOT0 button is not pressed.\r\n");
		return 0;
	}
}
//
void IAP_Init(void) {
	// check Upload Mod situation
	// 1 MAGIC_BKP_REG is MAGIC_BOOTLOADER_FLAG
	// 2 IAP_APP_ADDRESS is empty
	// 3 IAP_APP_ADDRESS is not empty and Boot0 button is pressed
	uint8_t isBoot0Pressed = Check_Boot0_Pressed();
	uint32_t boot_flag = HAL_RTCEx_BKUPRead(&hrtc, MAGIC_BKP_REG);

	// 判断启动模式
	if (boot_flag == MAGIC_CDC_RATE) {
		current_method = IAP_CDC;

	} else if (boot_flag == MAGIC_ETHERNET_FLAG) {
		current_method = IAP_ETHERNET;
	} else if (((*(__IO uint32_t*) IAP_APP_ADDRESS) & 0x2FFE0000) != 0x24080000
			|| (isBoot0Pressed > 0
					&& ((*(__IO uint32_t*) IAP_APP_ADDRESS) & 0x2FFE0000)
							== 0x24080000)) {
		current_method = IAP_CDC;
	}

	if (current_method != IAP_NONE) {
		printf("** UPLOAD Mod ...\r\n");
		printf("** Please input your command: \r\n");
		HAL_PWR_EnableBkUpAccess();
		HAL_RTCEx_BKUPWrite(&hrtc, MAGIC_BKP_REG, MAGIC_APP_FLAG);
		HAL_PWR_DisableBkUpAccess();
	} else {
		printf("** APP Mod ...\r\n");
		/* Jump to user application */
		JumpToApplication = (pFunction) *(__IO uint32_t*) (IAP_APP_ADDRESS + 4);

		MX_USB_DEVICE_DeInit();
		HAL_RCC_DeInit();
		HAL_DeInit();

		// DeInitializing systick peripheral
		SysTick->CTRL = 0;
		SysTick->LOAD = 0;
		SysTick->VAL = 0;

		/* Initialize application Stack Pointer */
		__set_MSP(*(__IO uint32_t*) IAP_APP_ADDRESS);
		JumpToApplication();
	}

}

void IAP_Task(void) {
	if (DataReadyFlag > 0) {
		DataReadyFlag = 0;
		//printf("%d %d, data is ready LenInRXBuffer %d : receivedBytes %d of %d \r\n", DataReadyFlag, currentState, LenInRXBuffer,receivedBytes, bytesToReceive);
		//LenInFlashBuffer = LenInRXBuffer;
		//LenInRXBuffer = 0;
		//memcpy(FlashBuffer, RXBuffer, LenInFlashBuffer);
		Process_Command();
	}
//	//
//    printf("DATA section:  Start = 0x%08lx, End = 0x%08lx, Size = %lu bytes\r\n",
//           (uint32_t)&_sdata, (uint32_t)&_edata, (uint32_t)&_edata - (uint32_t)&_sdata);
//
//    printf("BSS  section:  Start = 0x%08lx, End = 0x%08lx, Size = %lu bytes\r\n",
//           (uint32_t)&_sbss, (uint32_t)&_ebss, (uint32_t)&_ebss - (uint32_t)&_sbss);

//    printf("Heap size    : %u bytes\r\n", configTOTAL_HEAP_SIZE);
//    printf("Free heap    : %u bytes\r\n", xPortGetFreeHeapSize());
//    printf("Min ever heap: %u bytes\r\n", xPortGetMinimumEverFreeHeapSize());
//    TaskStatus_t taskStatusArray[10];  // 假设你有不超过 10 个任务
//    UBaseType_t taskCount = uxTaskGetSystemState(taskStatusArray, 10, NULL);
//
//    for (int i = 0; i < taskCount; i++) {
//        printf("Task: %s, Stack HighWaterMark: %u words (%u bytes)\r\n",
//               taskStatusArray[i].pcTaskName,
//               taskStatusArray[i].usStackHighWaterMark,
//               taskStatusArray[i].usStackHighWaterMark * sizeof(StackType_t));
//    }
}

void IAP_Data_Recv(IAP_Method iapM, uint8_t *Buf, uint32_t Len) {
	current_method = iapM;
	// 安全写入缓冲区
	int dataLen = IAP_RX_BUFFER_SIZE >= Len ? Len : IAP_RX_BUFFER_SIZE;
	if (currentState == IDLE) {
		memcpy(RXBuffer, Buf, dataLen);
		LenInRXBuffer = dataLen;
		DataReadyFlag = 1;
		printf("IDLE ready \r\n");
	} else {
		memcpy(RXBuffer + LenInRXBuffer, Buf, Len);
		LenInRXBuffer += Len;
		printf("--LenInRXBuffer %d : receivedBytes %d of %d \r\n",
							LenInRXBuffer, receivedBytes, bytesToReceive);
		// all data received or receive length reach the buffer size
		if (LenInRXBuffer >= IAP_RX_BUFFER_SIZE
				|| receivedBytes + LenInRXBuffer >= bytesToReceive) {
			// buffer is full
			DataReadyFlag = 1;
			printf("Data is ready to FLash LenInRXBuffer %d : receivedBytes %d of %d \r\n",
					LenInRXBuffer, receivedBytes, bytesToReceive);
		}
	}

	 printf("Heap size    : %u bytes\r\n", configTOTAL_HEAP_SIZE);
	 printf("Free heap    : %u bytes\r\n", xPortGetFreeHeapSize());
	 printf("Min ever heap: %u bytes\r\n", xPortGetMinimumEverFreeHeapSize());
	fflush(stdout);
}

void IAP_CDC_Trigger(uint32_t bitrate) {
	uint32_t regV = HAL_RTCEx_BKUPRead(&hrtc, MAGIC_BKP_REG);
	if (bitrate == MAGIC_CDC_RATE && regV != MAGIC_CDC_FLAG) {
		HAL_RTCEx_BKUPWrite(&hrtc, MAGIC_BKP_REG, MAGIC_CDC_FLAG);
		HAL_NVIC_SystemReset();
	}
}

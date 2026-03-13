/*
 * ota_processor.c
 *
 *  Created on: Apr 30, 2025
 *      Author: ziotier
 */
#include <inttypes.h>
#include "IAP_server.h"
#include "tcp_server.h"
#include "udp_server.h"
#include "crc.h"
#include "md5.h"

#include "main.h"

#include "rtc.h"
#include "usbd_cdc_flash.h"
#include "usb_device.h"

#include "usbd_cdc_if.h"

pFunction JumpToApplication;

uint32_t expected_size;
uint32_t expected_checksum;

uint8_t data_is_ready = 0; // 0 - no 1 - yes
uint32_t len_in_RX_buffer = 0;    // received data length in buffer
uint8_t RXBuffer[IAP_RX_BUFFER_SIZE]; // buffer to receive data
uint32_t received_bytes = 0; // received Bytes of Flash bin file

//uint8_t FlashBuffer[IAP_RX_BUFFER_SIZE];
//uint32_t LenInFlashBuffer = 0;
IAP_STATUS current_status = IDLE;

static IAP_Method current_method = IAP_NONE;

//
extern uint32_t _sdata, _edata;
extern uint32_t _sbss, _ebss;
extern uint32_t _estack;
extern uint16_t Erase_FLASH(uint8_t *flashAddress, uint32_t Len);
extern uint16_t Flash_If_Write(uint8_t *DataAddress, uint8_t *FlashAddress,
		uint32_t Len);
//
uint32_t app_base, app_msp, app_reset;

void reset_buf(void) {
	data_is_ready = 0;
	len_in_RX_buffer = 0;
	received_bytes = 0;
	memset(RXBuffer, 0, IAP_RX_BUFFER_SIZE); //
	current_status = IDLE;
	current_method = IAP_NONE;
}

// 接口特定的发送响应
void send_response(char *msg) {
	switch (current_method) {
	case IAP_CDC:
		CDC_Transmit_FS((uint8_t*) msg, strlen(msg));
		break;
	case IAP_ETHERNET:
		tcp_server_send((uint8_t*) msg, strlen(msg));
		break;
	default:
		break;
	}
}

void process_command() {
	if (current_status == IDLE) {
		// check the command type
		if (strncmp((char *)RXBuffer, "ping", 4) == 0) {
			send_response("OK");
		} else if (strncmp((char *)RXBuffer, "info", 4) == 0) {
			send_response(BOOT_LOADER_VERSION);
		} else if (strncmp((char *)RXBuffer, "run", 3) == 0) {
			// jump to App
			reset_buf();
			HAL_NVIC_SystemReset();
		} else if (strncmp((char *)RXBuffer, "flash", 5) == 0) {
			// decode flash command
			if (sscanf((char *)RXBuffer, "flash %" SCNu32 " %" SCNx32, &expected_size, &expected_checksum) == 2) {
				printf("File size %" PRIu32 ", checksum %" PRIx32 ". Wait for erasing flash! \r\n",
                        expected_size, expected_checksum);
				//
				HAL_StatusTypeDef status = Erase_FLASH((uint8_t*) app_base, expected_size);
				if (status != HAL_OK) {
					printf("Errors when erasing flash. Status:%d\r\n", status);
					send_response("ERR");
				} else {
					printf("Done! Begin to transfer bin file.\r\n");
					current_status = FLASH_RECEIVE;
					send_response("OK");
				}
			} else {
				printf("Invalid flash command");
			}
		} else {
			send_response("Unknown command");
		}
		len_in_RX_buffer = 0;
	} else if (current_status == FLASH_RECEIVE) {
		HAL_StatusTypeDef status = Flash_If_Write(RXBuffer, (uint8_t *)(app_base + received_bytes), len_in_RX_buffer);
		if (status != HAL_OK) {
			printf("There is error when writing flash. Addr:%" PRIx32 " Status:%d\r\n", app_base + received_bytes, status);
			// clean
			reset_buf();
			send_response("Failed");
		} else {

			received_bytes += len_in_RX_buffer;			//LenInFlashBuffer;
			memset(RXBuffer, 0, len_in_RX_buffer);			//
			printf("Done. Write Addr:%" PRIx32 " size:%" PRIu32 " (written:%" PRIu32 " of total:%" PRIu32 ") \r\n",
                    app_base, len_in_RX_buffer, received_bytes, expected_size);
			len_in_RX_buffer = 0;

			send_response("OK");
		}

		// 检查是否传输完成
		if (received_bytes >= expected_size) {
			printf("Flash complete, verifying checksum...\r\n");

			uint32_t caledCRC = HAL_CRC_Calculate(&hcrc, (uint32_t *)app_base, expected_size);
			if (expected_checksum == ~caledCRC) {
				printf("Checksum OK. Rebooting...\r\n");
				HAL_Delay(500);
				HAL_NVIC_SystemReset();
			} else {
				printf("Checksum FAIL. Expected: %08" PRIX32 ", Got: %08" PRIX32 "\r\n",
						expected_checksum, ~caledCRC);
				send_response("Checksum Failed");
			}
			reset_buf();
		}
	} else {
		printf("What is currentState:%d\r\n", current_status);
	}
	fflush(stdout);
}

//
uint8_t boot0_is_pressed(void) {
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
void server_init (void) {
	// check Upload Mod situation
	// 1 MAGIC_BKP_REG is MAGIC_BOOTLOADER_FLAG
	// 2 IAP_APP_ADDRESS is empty
	// 3 IAP_APP_ADDRESS is not empty and Boot0 button is pressed
	app_base  = (uint32_t)IAP_APP_ADDRESS;
	app_msp   = *(__IO uint32_t *)IAP_APP_ADDRESS;
	app_reset = *(__IO uint32_t *)(IAP_APP_ADDRESS + 4);

	uint8_t isBoot0Pressed = boot0_is_pressed();
	uint32_t boot_flag = HAL_RTCEx_BKUPRead(&hrtc, MAGIC_BKP_REG);

	// 判断启动模式
	if (boot_flag == MAGIC_CDC_FLAG) {
		current_method = IAP_CDC;

	} else if (boot_flag == MAGIC_ETH_FLAG) {
		current_method = IAP_ETHERNET;
	} else if ((app_msp & 0x2FFE0000) != 0x24080000
			|| (isBoot0Pressed > 0 && (app_msp & 0x2FFE0000) == 0x24080000)) {
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
		/* Stop and De-initialize USB peripheral */
		MX_USB_DEVICE_DeInit();

		__disable_irq();

		/* stop SysTick */
		SysTick->CTRL = 0;
		SysTick->LOAD = 0;
		SysTick->VAL = 0;

		/* clear interrupt pending*/
		for (uint32_t i = 0; i < 16; i++) {
			NVIC->ICER[i] = 0xFFFFFFFFU;
			NVIC->ICPR[i] = 0xFFFFFFFFU;
		}

		/* */
		HAL_RCC_DeInit();
		HAL_DeInit();

		SCB->VTOR = app_base;   // set vtor
		__set_MSP(app_msp);
		__DSB();
		__ISB();

		__enable_irq();         //
		((void (*)(void)) app_reset)();
	}
}
//
void IAP_init(void) {
	server_init();

	tcp_server_start();
	openplc_udp_server_start();
	//  udp_server_start(IAP_ETH_Trigger);
	//  udp_server_stop();
}

void IAP_task(void) {
	if (data_is_ready > 0) {
		data_is_ready = 0;
		process_command();
	}
}

void IAP_data_recv(IAP_Method iapM, uint8_t *Buf, uint32_t Len) {
	current_method = iapM;
	// 安全写入缓冲区
	int dataLen = IAP_RX_BUFFER_SIZE >= Len ? Len : IAP_RX_BUFFER_SIZE;
	if (current_status == IDLE) {
		memcpy(RXBuffer, Buf, dataLen);
		len_in_RX_buffer = dataLen;
		data_is_ready = 1;
		printf("IDLE ready \r\n");
	} else {

		//printf("--LenInRXBuffer %d : Len %d \r\n", LenInRXBuffer, Len);

		memcpy(RXBuffer + len_in_RX_buffer, Buf, Len);
		len_in_RX_buffer += Len;

//		printf("--LenInRXBuffer %d : receivedBytes %d of %d \r\n",
//				LenInRXBuffer, receivedBytes, expected_size);

		// all data received or receive length reach the buffer size
		if (len_in_RX_buffer >= IAP_RX_BUFFER_SIZE
				|| received_bytes + len_in_RX_buffer >= expected_size) {
			// buffer is full
			data_is_ready = 1;
			printf("Data is ready to FLash LenInRXBuffer %"PRIu32 " : received %"PRIu32 " of %"PRIu32 " \r\n",
					len_in_RX_buffer, received_bytes, expected_size);
		}
	}

	fflush(stdout);
}

void IAP_CDC_reboot_trigger(uint32_t bitrate) {
	uint32_t regV = HAL_RTCEx_BKUPRead(&hrtc, MAGIC_BKP_REG);
	if (bitrate == MAGIC_CDC_RATE && regV != MAGIC_CDC_FLAG) {
		HAL_RTCEx_BKUPWrite(&hrtc, MAGIC_BKP_REG, MAGIC_CDC_FLAG);
		HAL_NVIC_SystemReset();
	}
}

void IAP_ETH_reboot_trigger() {
	uint32_t regV = HAL_RTCEx_BKUPRead(&hrtc, MAGIC_BKP_REG);
	if (regV != MAGIC_ETH_FLAG) {
		HAL_RTCEx_BKUPWrite(&hrtc, MAGIC_BKP_REG, MAGIC_ETH_FLAG);
		HAL_NVIC_SystemReset();
	}
}

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
#include "sha256.h"
#include "fw_verify.h"
#include "bootloader_state.h"
#include "iap_auth.h"

#include "main.h"

#include "rtc.h"
#include "usbd_cdc_flash.h"
#include "usb_device.h"

#include "usbd_cdc_if.h"

pFunction JumpToApplication;

uint32_t expected_size;
uint32_t expected_checksum;
static uint8_t expected_signature[FW_SIGNATURE_SIZE];
static bool have_expected_signature;
static uint32_t expected_version; /* 0 if the flash command didn't carry one (older PC tool) */

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

static bool hex_decode(const char *hex, uint8_t *out, uint32_t out_len) {
	uint32_t i;

	if (strlen(hex) != (size_t)(out_len * 2U)) {
		return false;
	}
	for (i = 0; i < out_len; i++) {
		char hi = hex[i * 2U];
		char lo = hex[i * 2U + 1U];
		int hi_v, lo_v;

		if (hi >= '0' && hi <= '9') hi_v = hi - '0';
		else if (hi >= 'a' && hi <= 'f') hi_v = hi - 'a' + 10;
		else if (hi >= 'A' && hi <= 'F') hi_v = hi - 'A' + 10;
		else return false;

		if (lo >= '0' && lo <= '9') lo_v = lo - '0';
		else if (lo >= 'a' && lo <= 'f') lo_v = lo - 'a' + 10;
		else if (lo >= 'A' && lo <= 'F') lo_v = lo - 'A' + 10;
		else return false;

		out[i] = (uint8_t)((hi_v << 4) | lo_v);
	}
	return true;
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
		} else if (strncmp((char *)RXBuffer, "authchallenge", 13) == 0) {
			char nonce_hex[IAP_AUTH_NONCE_SIZE * 2U + 1U];
			iap_auth_issue_challenge(nonce_hex);
			send_response(nonce_hex);
		} else if (strncmp((char *)RXBuffer, "getversion", 10) == 0) {
			// Lets the PC tool learn the version currently installed *before*
			// committing to a flash command, so it can warn the operator
			// about a downgrade in its own console. The bootloader itself
			// never refuses a downgrade -- that decision is the operator's.
			iap_fw_metadata_t meta;
			char verbuf[16];
			uint32_t v = bootloader_state_get_metadata(&meta) ? meta.fw_version : 0U;
			snprintf(verbuf, sizeof(verbuf), "%" PRIu32, v);
			send_response(verbuf);
		} else if (strncmp((char *)RXBuffer, "flash", 5) == 0) {
			// decode flash command: "flash <size> <crc32hex> <signature_hex> <hmac_hex> [version]"
			// - signature_hex: 64-byte ECDSA r||s from keys/sign_firmware.sh (128 hex chars)
			// - hmac_hex: HMAC-SHA256(auth_key, nonce || "flash <size> <crc32hex> <signature_hex>[ <version>]")
			//   for the nonce most recently returned by "authchallenge" (32 hex chars)
			// - version: optional decimal firmware version; omitted by older PC
			//   tools, in which case it's saved as 0 (no rollback comparison
			//   possible for that image, same as before this field existed)
			char sig_hex[129] = {0};
			char hmac_hex[65] = {0};
			uint32_t parsed_version = 0U;
			int nParsed = sscanf((char *)RXBuffer, "flash %" SCNu32 " %" SCNx32 " %128s %64s %" SCNu32,
					&expected_size, &expected_checksum, sig_hex, hmac_hex, &parsed_version);
			bool haveVersion = (nParsed == 5);
			expected_version = haveVersion ? parsed_version : 0U;

			have_expected_signature = false;
			if (nParsed >= 3) {
				if (hex_decode(sig_hex, expected_signature, FW_SIGNATURE_SIZE)) {
					have_expected_signature = true;
				} else {
					printf("Invalid signature hex in flash command\r\n");
				}
			}

			bool authOk = false;
			uint8_t hmacBytes[IAP_AUTH_HMAC_SIZE];
			if ((nParsed == 4 || nParsed == 5) && have_expected_signature
					&& hex_decode(hmac_hex, hmacBytes, IAP_AUTH_HMAC_SIZE)) {
				char authMsg[220];
				int authMsgLen = haveVersion
						? snprintf(authMsg, sizeof(authMsg), "flash %" PRIu32 " %" PRIx32 " %s %" PRIu32,
								expected_size, expected_checksum, sig_hex, parsed_version)
						: snprintf(authMsg, sizeof(authMsg), "flash %" PRIu32 " %" PRIx32 " %s",
								expected_size, expected_checksum, sig_hex);
				authOk = iap_auth_verify_and_consume((const uint8_t *)authMsg, (uint32_t)authMsgLen, hmacBytes);
			}

			if (nParsed >= 2) {
				if (expected_size == 0 || expected_size > IAP_APP_MAX_SIZE) {
					printf("Invalid flash size %" PRIu32 ", app region only has %" PRIu32 " bytes\r\n",
							expected_size, (uint32_t)IAP_APP_MAX_SIZE);
					send_response("ERR");
				} else if (!have_expected_signature) {
					// no valid signature was provided -- refuse before erasing
					// anything, so a rejected/malformed upload can never
					// destroy the currently-trusted app for nothing
					printf("flash command missing/invalid signature - refusing to erase\r\n");
					send_response("ERR");
				} else if (!authOk) {
					printf("flash command failed HMAC auth - refusing to erase\r\n");
					send_response("ERR");
					bootloader_state_log_event(IAP_EVT_AUTH_FAIL, (uint32_t)current_method,
							tcp_server_get_client_ip(), HAL_GetTick(), iap_auth_get_counter());
				} else {
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
			uint32_t peer_ip = tcp_server_get_client_ip();
			uint32_t nowTick = HAL_GetTick();
			uint32_t authCtr = iap_auth_get_counter();

			if (expected_checksum != ~caledCRC) {
				printf("Checksum FAIL. Expected: %08" PRIX32 ", Got: %08" PRIX32 "\r\n",
						expected_checksum, ~caledCRC);
				send_response("Checksum Failed");
				bootloader_state_log_event(IAP_EVT_CRC_FAIL, (uint32_t)current_method, peer_ip, nowTick, authCtr);
			} else if (!have_expected_signature) {
				printf("No valid signature was provided with this upload - refusing to trust it.\r\n");
				send_response("No Signature");
				bootloader_state_log_event(IAP_EVT_SIG_FAIL, (uint32_t)current_method, peer_ip, nowTick, authCtr);
			} else if (!bootloader_state_crypto_selftest_passed()) {
				// Same precondition server_init() applies to the boot-time
				// check (Step 1) -- don't trust a signature verification
				// result computed with a crypto implementation the self-test
				// already found broken at boot.
				printf("Crypto self-test failed earlier - refusing to trust signature verification.\r\n");
				send_response("Signature Failed");
				bootloader_state_log_event(IAP_EVT_SIG_FAIL, (uint32_t)current_method, peer_ip, nowTick, authCtr);
			} else {
				uint8_t hash[32];
				bootloader_state_hash_app(app_base, expected_size, hash);
				if (!fw_verify_signature(hash, expected_signature)) {
					printf("Signature verification FAILED - firmware not trusted.\r\n");
					send_response("Signature Failed");
					bootloader_state_log_event(IAP_EVT_SIG_FAIL, (uint32_t)current_method, peer_ip, nowTick, authCtr);
				} else {
					printf("Checksum and signature OK. Rebooting...\r\n");
					bootloader_state_save_metadata(expected_size, expected_version, hash, expected_signature);
					bootloader_state_log_event(IAP_EVT_UPDATE_OK, (uint32_t)current_method, peer_ip, nowTick, authCtr);
					HAL_Delay(500);
					HAL_NVIC_SystemReset();
				}
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

	// Is the app region's actual content the one that was signed and accepted
	// at the last successful update? This replaces the old "does the MSP
	// value merely look like a plausible RAM address" heuristic, which could
	// not tell a legitimate app from corrupted or maliciously-written flash
	// as long as the first word happened to look like a stack pointer.
	bool app_signature_valid = false;
	if (bootloader_state_crypto_selftest_passed()) {
		iap_fw_metadata_t meta;
		if (bootloader_state_get_metadata(&meta) && meta.app_size > 0U && meta.app_size <= IAP_APP_MAX_SIZE) {
			uint8_t hash[32];
			bootloader_state_hash_app(app_base, meta.app_size, hash);
			app_signature_valid = fw_verify_signature(hash, meta.signature);
		}
	}
	bootloader_state_set_app_valid(app_signature_valid);

	// 判断启动模式
	if (boot_flag == MAGIC_CDC_FLAG) {
		current_method = IAP_CDC;
	} else if (boot_flag == MAGIC_ETH_FLAG) {
		current_method = IAP_ETHERNET;
	} else if (!app_signature_valid || isBoot0Pressed > 0) {
		current_method = IAP_CDC;
		if (!app_signature_valid) {
			printf("** App signature invalid or absent - staying in bootloader **\r\n");
			bootloader_state_log_event(IAP_EVT_BOOT_VERIFY_FAIL, (uint32_t)IAP_NONE, 0U,
					HAL_GetTick(), iap_auth_get_counter());
		}
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
	bootloader_state_init();
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

		uint32_t remaining = IAP_RX_BUFFER_SIZE - len_in_RX_buffer;
		if (Len > remaining) {
			printf("RX overflow: Len %" PRIu32 " exceeds remaining buffer %" PRIu32 ". Aborting transfer.\r\n",
					Len, remaining);
			send_response("ERR");
			reset_buf();
			fflush(stdout);
			return;
		}

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

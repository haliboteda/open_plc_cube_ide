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
#include "iap_keyderive.h"
#include "IAP_boot_handoff.h"

#include "main.h"

#include "usart.h"          /* huart4, de-initialised before jumping to the app */
#include "usbd_cdc_flash.h"

#include "usbd_cdc_if.h"

pFunction JumpToApplication;

uint32_t expected_size;
uint32_t expected_checksum;
static uint8_t expected_signature[FW_SIGNATURE_SIZE];
static bool have_expected_signature;
static uint32_t expected_version; /* 0 if the flash command didn't carry one (older PC tool) */

// volatile: written from IAP_data_recv() (USB/lwIP receive callback context)
// and read/cleared from IAP_task() (main superloop context) -- without it
// the compiler is free to assume nothing outside the current function
// changes these between accesses.
volatile uint8_t data_is_ready = 0; // 0 - no 1 - yes
volatile uint32_t len_in_RX_buffer = 0;    // received data length in buffer
uint8_t RXBuffer[IAP_RX_BUFFER_SIZE]; // buffer to receive data
volatile uint32_t received_bytes = 0; // received Bytes of Flash bin file

//uint8_t FlashBuffer[IAP_RX_BUFFER_SIZE];
//uint32_t LenInFlashBuffer = 0;
volatile IAP_STATUS current_status = IDLE;

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
		} else if (strncmp((char *)RXBuffer, "getuid", 6) == 0) {
			// Lets a PC tool connected over CDC (no discovery reply available)
			// learn this device's machine ID, needed to derive its device key
			// before an authchallenge/flash exchange.
			char uid_hex[IAP_MACHINE_ID_HEX_LEN + 1U];
			iap_keyderive_get_machine_id_hex(uid_hex);
			send_response(uid_hex);
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
				// Same precondition server_decide() applies to the boot-time
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

/*
 * Raw, silent read of the BOOT0 / KNX-programming button (SW2 on PG9).
 *
 * Silent on purpose: the caller polls this in a loop for the whole length of the
 * startup relay window, so printing here would flood the log. server_decide()
 * reports the latched outcome once.
 *
 * Reads HIGH when pressed: SW2 ties the pin to 3V3, and the pin is configured
 * GPIO_Input + PULLDOWN, so it sits low when released. The macro name follows
 * the .ioc's User Label for PG9 and changes if that label is retyped.
 */
uint8_t boot0_is_pressed(void) {
	return (HAL_GPIO_ReadPin(Boot0_GPIO_Port, Boot0_Pin) == GPIO_PIN_SET) ? 1U : 0U;
}
// __set_MSP() from inside an ordinary C function is unsafe if anything else
// in that function runs afterward: the compiler doesn't know the hardware
// stack pointer just moved, and still emits its normal epilogue (restoring
// callee-saved registers and deallocating locals) relative to the *old*
// stack. If that epilogue instead executes after MSP already points at the
// jumped-to app's fresh stack, "restore/deallocate" becomes "read garbage
// past the top of the app's RAM" -- which is exactly what was happening
// here: server_jump_to_app()'s compiler-generated epilogue (`add sp,#0xA0` +
// `ldmia sp!,{r4-r9,sl,lr}`) ran *after* MSP had already been set to
// app_msp, corrupting SP to app_msp+0xA0 (0x240800A0, just past the end of
// AXI SRAM) and bus-faulting on the register pop. A naked function has no
// compiler-generated prologue/epilogue at all, so nothing can run between
// setting MSP and jumping.
__attribute__((naked)) static void jump_to_app(uint32_t msp, uint32_t reset_vector)
{
	__asm volatile (
		"msr msp, r0\n"
		"dsb\n"
		"isb\n"
		"cpsie i\n"
		"bx r1\n"
	);
}

/*
 * Phase 1 of the boot sequence: decide whether this reset runs the application
 * or stays in the bootloader, and return the answer without acting on it.
 *
 * Everything needed here is available before a single peripheral has been
 * initialised: flash reads, software SHA-256/ECDSA, one GPIO pin for BOOT0, and
 * the handoff record in SRAM4. That is the point -- it lets USB, ethernet and
 * FMC stay untouched on the path that boots the application, so the application
 * inherits almost none of our state. "Never initialised" cannot be got wrong,
 * whereas a de-init list has to be maintained by hand and silently rots.
 *
 * Deliberately does not jump: the caller owns that step (server_jump_to_app) so
 * it can do its own work first, such as the startup beep.
 */
IAP_Method server_decide(uint8_t boot0Pressed) {
	IAP_Method mode = IAP_NONE;
	boot_req_t req;
	bool app_signature_valid = false;

	bootloader_state_init();

	app_base  = (uint32_t)IAP_APP_ADDRESS;
	app_msp   = *(__IO uint32_t *)IAP_APP_ADDRESS;
	app_reset = *(__IO uint32_t *)(IAP_APP_ADDRESS + 4);

	/* Take the request first: boot_handoff_take() is also what reads and clears
	 * RCC->RSR, so it has to run before anything else can look at the reset
	 * cause. It consumes the record, so a stale request can never pin us here. */
	req = boot_handoff_take();

	/* boot0Pressed is latched by the caller across the whole startup relay
	 * window, not sampled here: reading the pin once at this point would need
	 * the operator to already be holding the button a few milliseconds after
	 * reset, which is not a reaction time a human has. */
	printf(boot0Pressed ? "BOOT0 button is pressed!\r\n"
	                    : "BOOT0 button is not pressed.\r\n");

	// Is the app region's actual content the one that was signed and accepted
	// at the last successful update? This replaces the old "does the MSP
	// value merely look like a plausible RAM address" heuristic, which could
	// not tell a legitimate app from corrupted or maliciously-written flash
	// as long as the first word happened to look like a stack pointer.
	if (bootloader_state_crypto_selftest_passed()) {
		iap_fw_metadata_t meta;
		if (bootloader_state_get_metadata(&meta) && meta.app_size > 0U && meta.app_size <= IAP_APP_MAX_SIZE) {
			uint8_t hash[32];
			bootloader_state_hash_app(app_base, meta.app_size, hash);
			app_signature_valid = fw_verify_signature(hash, meta.signature);
		}
	}
	bootloader_state_set_app_valid(app_signature_valid);

	switch (req) {
	case BOOT_REQ_CDC:
		mode = IAP_CDC;
		break;
	case BOOT_REQ_ETH:
		mode = IAP_ETHERNET;
		break;
	case BOOT_REQ_ALL:
		/* Somebody asked for an upload but the record did not say over which
		 * channel. Open both rather than guess -- see IAP_ALL in IAP_config.h. */
		mode = IAP_ALL;
		break;
	default:
		if (!app_signature_valid) {
			printf("** App signature invalid or absent - staying in bootloader **\r\n");
			bootloader_state_log_event(IAP_EVT_BOOT_VERIFY_FAIL, (uint32_t)IAP_NONE, 0U,
					HAL_GetTick(), iap_auth_get_counter());
			mode = IAP_ALL;
		} else if (boot0Pressed > 0U) {
			/* The physical escape hatch. We cannot know which channel the
			 * operator will use, so serve all of them. */
			mode = IAP_ALL;
		}
		break;
	}

	/* current_method tracks the channel actually in use and is (re)set by
	 * IAP_data_recv() as soon as a command arrives. Pre-seed it for the
	 * single-channel modes to keep the old behaviour; IAP_ALL has no single
	 * channel yet, so leave it until the first command says which one. */
	if ((mode == IAP_CDC) || (mode == IAP_ETHERNET)) {
		current_method = mode;
	}

	if (mode != IAP_NONE) {
		printf("** UPLOAD Mod ...\r\n");
		printf("** Please input your command: \r\n");
	} else {
		printf("** APP Mod ...\r\n");
	}
	return mode;
}

/*
 * Hand the hardware back and jump. Only valid when server_decide() returned
 * IAP_NONE, and only on the boot path -- a completed upload resets instead (see
 * process_command()), which is what keeps the I-cache out of the picture.
 *
 * There is no MX_USB_DEVICE_DeInit() here any more: on this path USB was never
 * brought up in the first place.
 */
void server_jump_to_app(void) {
	/* Give the application a clean slate. Our MPU regions describe the
	 * bootloader's lwIP buffers and mean nothing to it, and leaving the 4GB
	 * no-access blanket in place would deny it the external SDRAM. One register
	 * write, so unlike a peripheral de-init there is nothing to forget. */
	HAL_MPU_Disable();

	/* UART4 is the only peripheral Phase 1 brought up (for the boot log). Its
	 * MspDeInit also releases the PC10/PC11 pins. */
	HAL_UART_DeInit(&huart4);

	__disable_irq();

	/* stop SysTick */
	SysTick->CTRL = 0;
	SysTick->LOAD = 0;
	SysTick->VAL = 0;

	/* clear interrupt pending -- NVIC->ICER/ICPR are 8 words (256 IRQs,
	 * the ARMv7-M architectural max), not 16; looping past 8 wrote into
	 * the reserved padding between register banks. */
	for (uint32_t i = 0; i < 8; i++) {
		NVIC->ICER[i] = 0xFFFFFFFFU;
		NVIC->ICPR[i] = 0xFFFFFFFFU;
	}

	/* */
	HAL_RCC_DeInit();
	HAL_DeInit();

	SCB->VTOR = app_base;   // set vtor

	jump_to_app(app_msp, app_reset);
}

/*
 * Phase 2: open the command channels for the mode we settled on. Ethernet needs
 * lwIP already initialised; CDC needs no server object at all, because
 * usbd_cdc_if.c calls IAP_data_recv() straight from the USB receive callback.
 */
void IAP_servers_start(IAP_Method mode) {
	if ((mode == IAP_ETHERNET) || (mode == IAP_ALL)) {
		tcp_server_start();
		openplc_udp_server_start();
	}
}

#define IAP_IDLE_RX_STALE_MS 500U
static volatile uint32_t s_idle_rx_last_tick;

void IAP_task(void) {
	if (data_is_ready > 0) {
		data_is_ready = 0;
		process_command();
	} else if (current_status == IDLE && len_in_RX_buffer > 0
			&& (HAL_GetTick() - s_idle_rx_last_tick) > IAP_IDLE_RX_STALE_MS) {
		// A partial command with no terminator (missing '\n'/'\r', or a
		// client that never finished writing) has gone quiet for too long --
		// discard it instead of waiting forever, so it doesn't silently
		// corrupt whatever the next real command attempt sends.
		printf("Discarding stale partial command (%" PRIu32 " bytes, no terminator)\r\n", len_in_RX_buffer);
		len_in_RX_buffer = 0;
	}
}

void IAP_data_recv(IAP_Method iapM, uint8_t *Buf, uint32_t Len) {
	current_method = iapM;

	if (current_status == IDLE) {
		uint32_t remaining = IAP_RX_BUFFER_SIZE - len_in_RX_buffer;
		uint32_t copyLen = (Len > remaining) ? remaining : Len;

		memcpy(RXBuffer + len_in_RX_buffer, Buf, copyLen);
		len_in_RX_buffer += copyLen;
		s_idle_rx_last_tick = HAL_GetTick();

		// A text command is framed by a trailing '\n' or '\r' (see
		// IAPTranfer_Tool's SendCommandReadResponse/SendCommandWaitForResponse
		// and the TCP sendCommand* helpers, which send '\n'; either is
		// accepted so a raw terminal whose Enter key only sends '\r' still
		// works). Without this, a command longer than one USB CDC packet or
		// one TCP segment -- e.g. "flash <size> <crc32> <128-hex-sig>
		// <64-hex-hmac>", ~214 bytes -- can arrive split across several
		// IAP_data_recv() calls; overwriting RXBuffer from offset 0 on every
		// call (the old behavior) meant only the LAST fragment survived by
		// the time process_command() ran, which is why "flash" landed as
		// "Unknown command" the first time this was tested end-to-end on
		// real hardware. Waiting for a terminator (or the buffer filling up,
		// as a safety net) fixes that; IAP_task()'s stale-buffer check
		// handles a terminator that never arrives at all.
		if (memchr(RXBuffer, '\n', len_in_RX_buffer) != NULL
				|| memchr(RXBuffer, '\r', len_in_RX_buffer) != NULL
				|| len_in_RX_buffer >= IAP_RX_BUFFER_SIZE) {
			data_is_ready = 1;
			printf("IDLE ready \r\n");
		}
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

/*
 * Both triggers go through the shared handoff module, which stores the request,
 * verifies it actually landed, and only then resets. The previous versions wrote
 * an RTC backup register directly and had to remember to open the backup domain
 * first -- forgetting that is precisely what made the ethernet path fail in a
 * way no error code or log line could show.
 *
 * boot_handoff_request() does not return on success. If it does return, the
 * request was not stored, so we must NOT reset: resetting would boot the
 * application again and look exactly like "the device ignored the command".
 */
void IAP_CDC_reboot_trigger(uint32_t bitrate) {
	if (bitrate == MAGIC_CDC_RATE) {
		if (!boot_handoff_request(BOOT_REQ_CDC)) {
			printf("Refusing to reset: CDC boot request could not be stored\r\n");
		}
	}
}

void IAP_ETH_reboot_trigger(void) {
	if (!boot_handoff_request(BOOT_REQ_ETH)) {
		printf("Refusing to reset: ethernet boot request could not be stored\r\n");
	}
}

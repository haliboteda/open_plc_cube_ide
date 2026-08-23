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
#include "fw_pubkey.h"
#include "owner_slot.h"
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
static bool s_boot0_held;         /* BOOT0 was down at this boot's decision point */

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

/* The staging buffer has to hold the largest image the app region accepts, plus
 * the padding that rounds the last flash word up to 32 bytes. */
_Static_assert(IAP_STAGE_SIZE >= (uint32_t)IAP_APP_MAX_SIZE + 32U,
		"staging buffer is smaller than the application region it must hold");

/*
 * Erase the application region and copy the verified image out of SDRAM.
 *
 * Only reached once the staged image has passed both CRC and signature, which is
 * the entire point of staging: this is the first moment the currently-installed
 * application is at risk, and the exposure lasts the erase plus the write rather
 * than the whole transfer.
 *
 * On failure the app region is left half-written with stale metadata still
 * describing the old image, so the next boot reports an invalid application and
 * asks for another upload -- the same recoverable state an interrupted transfer
 * used to leave behind.
 */
static bool stage_commit_to_flash(void) {
	/* HAL_FLASH_Program always consumes a full 32-byte flash word, so the copy
	 * length is rounded up. Pad with the erased value so anything reading past
	 * the image sees erased flash instead of leftover SDRAM contents. */
	const uint32_t padded = (expected_size + 31U) & ~31U;
	const uint32_t pad = padded - expected_size;

	if (pad > 0U) {
		memset((uint8_t *)(IAP_STAGE_BASE + expected_size), 0xFF, pad);
	}

	printf("Erasing application region (%" PRIu32 " bytes)...\r\n", expected_size);
	if (Erase_FLASH((uint8_t *)app_base, expected_size) != HAL_OK) {
		printf("Erase FAILED\r\n");
		return false;
	}

	printf("Writing %" PRIu32 " bytes from SDRAM to flash...\r\n", padded);
	if (Flash_If_Write((uint8_t *)IAP_STAGE_BASE, (uint8_t *)app_base, padded) != HAL_OK) {
		printf("Flash write FAILED\r\n");
		return false;
	}
	return true;
}

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

void iap_identity_string(char *out, uint32_t out_len)
{
	char uid_hex[IAP_MACHINE_ID_HEX_LEN + 1U] = {0};
	/* Split on "_" by the PC tool (parseBoardInfoFromReply), so no field may
	 * contain one. */
	const char *role = bootloader_state_app_is_valid() ? UDP_SERVER_NAME : UDP_SERVER_NAME "-INVALID";

	iap_keyderive_get_machine_id_hex(uid_hex);
	(void)snprintf(out, out_len, "%s_%s_%s_%s",
			OPENPLC_DEVICE_NAME, uid_hex, role, OPENPLC_FW_VERSION);
}

void process_command() {
	if (current_status == IDLE) {
		// check the command type
		if (strncmp((char *)RXBuffer, "openplc_server_where_r_y", 24) == 0) {
			// Same string the UDP discovery reply carries, so a PC tool identifies
			// a board the same way on both channels: name_uid_role_version, with
			// role telling BOOTLD from BOOTLD-INVALID.
			char identity[96];
			iap_identity_string(identity, sizeof(identity));
			send_response(identity);
		} else if (strncmp((char *)RXBuffer, "ping", 4) == 0) {
			send_response("OK");
		} else if (strncmp((char *)RXBuffer, "info", 4) == 0) {
			send_response(BOOT_LOADER_VERSION);
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
		} else if (strncmp((char *)RXBuffer, "getpubkey", 9) == 0) {
			// Lets the PC tool confirm its signing key matches the key this
			// bootloader verifies against, before it spends time sending an
			// image that would be rejected at the end.
			//
			// owner_slot_root(), not fw_public_key: on a claimed board those
			// differ, and answering with the built-in key would make the tool
			// compare against a key this board no longer trusts -- refusing
			// good images and accepting bad ones, both silently.
			const uint8_t *root = owner_slot_root();
			char pubhex[64 * 2U + 1U];
			uint32_t i;
			for (i = 0; i < 64U; i++) {
				(void)sprintf(&pubhex[i * 2U], "%02x", root[i]);
			}
			send_response(pubhex);
		} else if (strncmp((char *)RXBuffer, "takeown ", 8) == 0) {
			// "takeown <128 hex chars>" -- bind this board to a signing key.
			//
			// Only ever the FIRST claim. Changing an existing owner needs the
			// current owner's signature and is a different command (M1 step 5),
			// because this one is gated by physical presence alone.
			uint8_t key[64];
			const char *hex = (const char *)RXBuffer + 8;
			uint32_t i;
			bool ok = true;

			for (i = 0U; i < 64U; i++) {
				unsigned int byte;
				if (sscanf(&hex[i * 2U], "%2x", &byte) != 1) {
					ok = false;
					break;
				}
				key[i] = (uint8_t)byte;
			}
			if (!ok) {
				send_response("Bad key");
			} else if (owner_slot_claim(key, s_boot0_held)) {
				send_response("OK");
			} else {
				send_response("Refused");
			}
		} else if (strncmp((char *)RXBuffer, "getowner", 8) == 0) {
			// Generation of the record in force, 0 when unclaimed. The host
			// needs it to build the next record's signed prefix -- both sides
			// must agree on the exact bytes, and the generation is in them.
			char genbuf[16];
			snprintf(genbuf, sizeof(genbuf), "%" PRIu32, owner_slot_generation());
			send_response(genbuf);
		} else if (strncmp((char *)RXBuffer, "setowner ", 9) == 0) {
			// "setowner <generation> <newkey_hex> <sig_hex>"
			//
			// No BOOT0 here, deliberately: the current owner's signature IS the
			// authorisation, and handing a board over remotely is a case the
			// design supports. Physical presence gates only the operations that
			// have no signature to check -- the first claim and factory reset.
			uint32_t gen = 0U;
			char keyhex[129];
			char sighex[129];
			uint8_t key[64];
			uint8_t sig[64];
			bool ok = true;
			uint32_t i;

			if (sscanf((char *)RXBuffer + 9, "%" SCNu32 " %128s %128s",
					&gen, keyhex, sighex) != 3) {
				send_response("Bad args");
			} else if ((strlen(keyhex) != 128U) || (strlen(sighex) != 128U)) {
				send_response("Bad length");
			} else {
				for (i = 0U; i < 64U; i++) {
					unsigned int kb, sb;
					if ((sscanf(&keyhex[i * 2U], "%2x", &kb) != 1) ||
							(sscanf(&sighex[i * 2U], "%2x", &sb) != 1)) {
						ok = false;
						break;
					}
					key[i] = (uint8_t)kb;
					sig[i] = (uint8_t)sb;
				}
				if (!ok) {
					send_response("Bad hex");
				} else if (owner_slot_set_owner(gen, key, sig)) {
					send_response("OK");
				} else {
					send_response("Refused");
				}
			}
		} else if (strncmp((char *)RXBuffer, "flash", 5) == 0) {
			// decode flash command: "flash <size> <crc32hex> <signature_hex> <hmac_hex> [version]"
			// - signature_hex: 64-byte ECDSA r||s from IAPTool sign / IAPTool cdc (128 hex chars)
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
					send_response("ERR");
					/* RAM only: this is the one rejection path an unauthenticated
					 * caller can reach, so it must never touch Flash. */
					bootloader_state_note_auth_fail((uint32_t)current_method,
							tcp_server_get_client_ip(), HAL_GetTick());
				} else {
					/* Nothing is erased here any more: the image goes to SDRAM
					 * first and the app region is only touched once it has
					 * passed CRC and signature. So a rejected, corrupt or
					 * interrupted upload leaves the running app alone. */
					printf("File size %" PRIu32 ", checksum %" PRIx32 ". Staging in SDRAM.\r\n",
	                        expected_size, expected_checksum);
					received_bytes = 0;
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
		/* A peer that sends more than the size it announced would run off the end
		 * of the staging buffer, so bound the copy by the buffer rather than by
		 * the announced size. */
		if (received_bytes + len_in_RX_buffer > IAP_STAGE_SIZE) {
			printf("Staging overflow: %" PRIu32 " + %" PRIu32 " exceeds %" PRIu32 " bytes. Aborting.\r\n",
					received_bytes, len_in_RX_buffer, (uint32_t)IAP_STAGE_SIZE);
			bootloader_state_log_event(IAP_EVT_OVERFLOW_ABORT, (uint32_t)current_method,
					tcp_server_get_client_ip(), HAL_GetTick(), iap_auth_get_counter());
			/* Answer before resetting: reset_buf() clears current_method, and
			 * send_response() routes on it -- afterwards the reply goes nowhere. */
			send_response("Failed");
			reset_buf();
		} else {
			memcpy((uint8_t *)(IAP_STAGE_BASE + received_bytes), RXBuffer, len_in_RX_buffer);
			received_bytes += len_in_RX_buffer;
			memset(RXBuffer, 0, len_in_RX_buffer);
			printf("Staged %" PRIu32 " bytes (%" PRIu32 " of total %" PRIu32 ")\r\n",
                    len_in_RX_buffer, received_bytes, expected_size);
			len_in_RX_buffer = 0;

			send_response("OK");
		}

		// 检查是否传输完成
		if (received_bytes >= expected_size) {
			printf("Transfer complete, verifying the staged image...\r\n");

			/* CRC, hash and signature are all computed over the staging buffer.
			 * Flash is not touched until every one of them has passed. */
			uint32_t caledCRC = HAL_CRC_Calculate(&hcrc, (uint32_t *)IAP_STAGE_BASE, expected_size);
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
				/* Hashes the staging buffer, not the app region -- the app region
				 * still holds the previous image at this point. */
				bootloader_state_hash_app(IAP_STAGE_BASE, expected_size, hash);
				if (!fw_verify_signature(hash, expected_signature)) {
					printf("Signature verification FAILED - firmware not trusted. "
							"Application region untouched.\r\n");
					send_response("Signature Failed");
					bootloader_state_log_event(IAP_EVT_SIG_FAIL, (uint32_t)current_method, peer_ip, nowTick, authCtr);
				} else if (!stage_commit_to_flash()) {
					printf("The image was good but the flash write was not - "
							"retry the upload.\r\n");
					send_response("Flash Failed");
					bootloader_state_log_event(IAP_EVT_FLASH_WRITE_FAIL, (uint32_t)current_method, peer_ip, nowTick, authCtr);
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
 * Raw, silent read of the BOOT0 / KNX-programming button (SW2 on PG9). Silent
 * because the caller polls it throughout the startup relay window. Reads the
 * pin as MX_GPIO_Init() left it; HIGH means pressed.
 */
uint8_t boot0_is_pressed(void) {
	return (HAL_GPIO_ReadPin(BOOT0_GPIO_Port, BOOT0_Pin) == GPIO_PIN_SET) ? 1U : 0U;
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

	/* Remembered for the whole session: takeown needs to know the operator was
	 * physically present, and by the time a command arrives over the network
	 * they have long since let the button go. */
	s_boot0_held = (boot0Pressed != 0U);

	bootloader_state_init();

	/* Every boot, not only the ones that stay in the bootloader: which key this
	 * board trusts is the kind of thing that must never be visible only when
	 * somebody happens to be looking. Read-only -- see owner_slot.h. */
	owner_slot_report();

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
	 * reset, which is not a reaction time a human has. A press decides the
	 * outcome on its own -- see the branch below. */
	const char *reason = "";

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

	if (boot0Pressed > 0U) {
		/* The physical escape hatch wins outright. Whoever held the button
		 * through the relay window wants the bootloader, so nothing else gets
		 * a vote -- not the handoff request, not the signature. We cannot know
		 * which channel they will use, so serve all of them.
		 *
		 * The two reads above still had to happen: boot_handoff_take() is what
		 * consumes the request record (leaving it would pin the *next* boot
		 * here), and the signature result feeds the startup beep and the UDP
		 * server name, neither of which is part of this decision. */
		mode = IAP_ALL;
		reason = "BOOT0 held";
	} else {
		switch (req) {
		case BOOT_REQ_CDC:
			mode = IAP_CDC;
			reason = "CDC upload requested";
			break;
		case BOOT_REQ_ETH:
			mode = IAP_ETHERNET;
			reason = "ethernet upload requested";
			break;
		case BOOT_REQ_ALL:
			/* Somebody asked for an upload but the record did not say over which
			 * channel. Open both rather than guess -- see IAP_ALL in IAP_config.h. */
			mode = IAP_ALL;
			reason = "upload requested, channel unknown";
			break;
		default:
			if (!app_signature_valid) {
				printf("** App signature invalid or absent - staying in bootloader **\r\n");
				/* Only the first boot in a run of failures is recorded: this path
				 * needs no credential, so logging every power-up would let anyone
				 * with a power switch fill the journal and push older evidence out.
				 * Counter is 0 here by definition: no challenge has been issued
				 * this boot, and the RTC is not initialised until Phase 2. */
				if (bootloader_state_last_log_event() != (uint32_t)IAP_EVT_BOOT_VERIFY_FAIL) {
					bootloader_state_log_event(IAP_EVT_BOOT_VERIFY_FAIL, (uint32_t)IAP_NONE, 0U,
							HAL_GetTick(), 0U);
				}
				mode = IAP_ALL;
				reason = "no valid application";
			}
			break;
		}
	}

	/* current_method tracks the channel actually in use and is (re)set by
	 * IAP_data_recv() as soon as a command arrives. Pre-seed it for the
	 * single-channel modes to keep the old behaviour; IAP_ALL has no single
	 * channel yet, so leave it until the first command says which one. */
	if ((mode == IAP_CDC) || (mode == IAP_ETHERNET)) {
		current_method = mode;
	}

	if (mode != IAP_NONE) {
		printf("** UPLOAD Mod ... (%s)\r\n", reason);
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
	 * MspDeInit also releases the PC10/PC11 pins; the MAX3221 enable is a plain
	 * GPIO and has to be handed back separately, so the application starts with
	 * the transceiver in the same shutdown state a cold board has.
	 *
	 * Order matters: shut the transceiver down BEFORE releasing the pins.
	 * MspDeInit leaves PC10 -- the MAX3221's data input -- floating, and a
	 * floating input on a still-powered transceiver drives whatever it picks up
	 * onto the line. Doing it the other way round put a garbage byte in front of
	 * the application's first log line (docs/work/ISSUES.md ISS-A2). */
	Disable_RX_RS232();
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

	/* Hand back a cold cache. No-op while the I-cache is off, and correct the
	 * moment it is switched on in the .ioc -- the application must not inherit
	 * cache lines holding bootloader instructions. CMSIS invalidates as part of
	 * the disable, so nothing else is needed. */
	SCB_DisableICache();

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
			bootloader_state_log_event(IAP_EVT_OVERFLOW_ABORT, (uint32_t)current_method,
					tcp_server_get_client_ip(), HAL_GetTick(), iap_auth_get_counter());
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

/* The CDC 1200-baud knock and the UDP reboot request are issued by the
 * application image, which calls boot_handoff_request() directly. The
 * bootloader only ever consumes the record, in server_decide(). */

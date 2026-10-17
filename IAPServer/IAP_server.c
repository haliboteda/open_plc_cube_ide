/*
 * IAP_server.c -- IAP command state machine and the boot decision.
 */
#include <inttypes.h>
#include "IAP_server.h"
#include "tcp_server.h"
#include "udp_server.h"
#include "crc.h"
#include "sha256.h"
#include "fw_verify.h"
#include "iap_cert.h"
#include "owner_slot.h"
#include "bootloader_state.h"
#include "safe_outputs.h"
#include "iap_auth.h"
#include "iap_keyderive.h"
#include "IAP_boot_handoff.h"

#include "main.h"

#include "fmc.h"            /* iap_sdram_selftest_passed(), for the reject text */
#include "lwip/netif.h"     /* netif_default, to notice a link that never came up */
#include "usbd_cdc_flash.h"

#include "usbd_cdc_if.h"
#include "boot_selfupgrade.h"

uint32_t expected_size;
uint32_t expected_checksum;
static uint8_t expected_signature[FW_SIGNATURE_SIZE];
static bool have_expected_signature;
static iap_cert_t expected_cert;   /* the leaf cert that (claims to) authorise expected_signature */
static bool s_staging_is_bootloader; /* this transfer is a flashboot, not a flash */

/* volatile: written from the USB/lwIP receive callbacks, read and cleared
 * from the superloop. */
volatile uint8_t data_is_ready = 0;
volatile uint32_t len_in_RX_buffer = 0;
uint8_t RXBuffer[IAP_RX_BUFFER_SIZE];
volatile uint32_t received_bytes = 0;   /* image bytes staged so far */

volatile IAP_STATUS current_status = IDLE;

static IAP_Method current_method = IAP_NONE;

extern uint16_t Erase_FLASH(uint8_t *flashAddress, uint32_t Len);

uint32_t app_base, app_msp, app_reset;

/* The staging buffer has to hold the largest image the app region accepts, plus
 * the padding that rounds the last flash word up to 32 bytes. */
_Static_assert(IAP_STAGE_SIZE >= (uint32_t)IAP_APP_MAX_SIZE + 32U,
		"staging buffer is smaller than the application region it must hold");

/*
 * Erase the application region and copy the verified image out of SDRAM.
 * Only reached once the staged image has passed CRC and signature. On failure
 * the stale metadata no longer matches the app region, so the next boot stays
 * in the bootloader and asks for another upload.
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
	s_staging_is_bootloader = false;
}

/* Reply on the channel the current command came in on. */
void send_response(char *msg) {
	switch (current_method) {
	case IAP_CDC: {
		uint8_t st = CDC_Transmit_FS((uint8_t*) msg, strlen(msg));
		if (st != USBD_OK) {
			printf("CDC_Transmit_FS failed (%u), response \"%s\" not sent\r\n",
					(unsigned)st, msg);
		}
		break;
	}
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
	 * contain one. Five fields: name_uid_role_pkgversion_appversion.
	 *
	 * The last field is "-", not a version: only a running application knows
	 * which sketch version is installed, and by the time the bootloader is
	 * answering, it is not running. Writing 0.0.0 here would read as a real
	 * version rather than an absent one. The tool keys off `role` anyway --
	 * it only compares versions when the answer came from CUSAPP.
	 *
	 * The core builds the same string in libraries/OpenPLC_IAP/src/udp_server.c
	 * and the two cannot share code; the pair is a tracked cross-repo mirror
	 * (see $PROD/docs/repo/ARCHITECTURE.md, checked by P2). */
	const char *role = bootloader_state_app_is_valid() ? UDP_SERVER_NAME : UDP_SERVER_NAME "-INVALID";

	iap_keyderive_get_machine_id_hex(uid_hex);
	(void)snprintf(out, out_len, "%s_%s_%s_%s_%s",
			OPENPLC_DEVICE_NAME, uid_hex, role, OPENPLC_FW_VERSION, "-");
}

/* TCP/CDC keep their prefix match; the reply text comes from the builder UDP
 * uses too (udp_server.c, mirror 21). */
static void send_query_reply(const char *name)
{
	char reply[IAP_QUERY_REPLY_MAX];

	if (iap_query_reply(name, reply, sizeof(reply))) {
		send_response(reply);
	}
}

void process_command() {
	if (current_status == IDLE) {
		// check the command type
		if (strncmp((char *)RXBuffer, "openplc_server_where_r_y", 24) == 0) {
			// Same string the UDP discovery reply carries, so a PC tool identifies
			// a board the same way on both channels (see iap_identity_string()), with
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
			/* No nonce rather than a predictable one: without a challenge the
			 * caller simply cannot authorize anything, which is the safe end. */
			if (iap_auth_issue_challenge(nonce_hex)) {
				send_response(nonce_hex);
			} else {
				send_response("ERR");
			}
		} else if (strncmp((char *)RXBuffer, "getuid", 6) == 0) {
			send_query_reply("getuid");
		} else if (strncmp((char *)RXBuffer, "getpubkey", 9) == 0) {
			send_query_reply("getpubkey");
		} else if (strncmp((char *)RXBuffer, "takeown ", 8) == 0) {
			// "takeown <128 hex chars>" -- bind this board to a signing key.
			//
			// Only while the board has no root, over any channel, no button
			// (decision 72). Changing an existing owner needs the current
			// owner's signature and is a different command (setowner).
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
			} else if (owner_slot_claim(key)) {
				send_response("OK");
			} else {
				send_response("Refused");
			}
		} else if (strncmp((char *)RXBuffer, "getowner", 8) == 0) {
			send_query_reply("getowner");
		} else if (strncmp((char *)RXBuffer, "getapprevoked", 13) == 0) {
			send_query_reply("getapprevoked");
		} else if ((strncmp((char *)RXBuffer, "setownerwipe ", 13) == 0)
				|| (strncmp((char *)RXBuffer, "setowner ", 9) == 0)) {
			// "setowner <generation> <newkey_hex> <sig_hex>"
			// "setownerwipe <generation> <newkey_hex> <sig_hex>"
			//
			// Same handover, same signature over the same bytes. The wipe form
			// reclaims sector 15 with a root area holding nothing but the new
			// record -- the only way to get revocation slots back, so the
			// operator has to ask for it (OWN-07).
			//
			// No button: the current owner's signature IS the authorisation.
			const bool wipe = (RXBuffer[8] == (uint8_t)'w');
			const char *const verb = wipe ? "setownerwipe" : "setowner";
			uint32_t gen = 0U;
			char keyhex[129];
			char sighex[129];
			uint8_t key[64];
			uint8_t sig[64];
			bool ok = true;
			uint32_t i;

			if (sscanf((char *)RXBuffer + strlen(verb), " %" SCNu32 " %128s %128s",
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
				} else if (wipe) {
					// Success resets without a reply, as the tool expects
					// (IAP-PROTOCOL.md); "Refused" means nothing was erased.
					static owner_carry_t wipe_carry;
					if (owner_slot_build_wipe_carry(gen, key, sig, &wipe_carry)
							&& bootloader_state_reclaim(&wipe_carry)) {
						printf("setownerwipe done. Rebooting...\r\n");
						fflush(stdout);
						HAL_Delay(200);
						HAL_NVIC_SystemReset();
					}
					send_response("Refused");
				} else if (owner_slot_set_owner(gen, key, sig)) {
					send_response("OK");
				} else {
					send_response("Refused");
				}
			}
		} else if (strncmp((char *)RXBuffer, "revoke ", 7) == 0) {
			// "revoke <leaf_prefix_hex> <sig_hex>"
			//
			// leaf_prefix_hex names ONE leaf: the first OWNER_REVOKE_PREFIX_LEN
			// bytes of its public key, 32 hex chars. One 'R' record holds
			// exactly one name (decision I-D3: batching was declined).
			//
			// No BOOT0, same reasoning as setowner: the current owner's
			// signature IS the authorisation.
			//
			// No generation on the wire either: an 'R' record is not a link in
			// the ownership chain, so it has no position in one (decision 59).
			char leafhex[(OWNER_REVOKE_PREFIX_LEN * 2U) + 1U];
			char sighex[129];
			uint8_t leaf_prefix[OWNER_REVOKE_PREFIX_LEN];
			uint8_t sig[64];
			bool ok = true;
			bool already = false;
			uint32_t i;

			if (sscanf((char *)RXBuffer + 7, "%32s %128s",
					leafhex, sighex) != 2) {
				send_response("Bad args");
			} else if ((strlen(leafhex) != (OWNER_REVOKE_PREFIX_LEN * 2U)) ||
					(strlen(sighex) != 128U)) {
				send_response("Bad length");
			} else {
				memset(leaf_prefix, 0, sizeof(leaf_prefix));
				for (i = 0U; i < OWNER_REVOKE_PREFIX_LEN; i++) {
					unsigned int lb;
					if (sscanf(&leafhex[i * 2U], "%2x", &lb) != 1) {
						ok = false;
						break;
					}
					leaf_prefix[i] = (uint8_t)lb;
				}
				for (i = 0U; ok && (i < 64U); i++) {
					unsigned int sb;
					if (sscanf(&sighex[i * 2U], "%2x", &sb) != 1) {
						ok = false;
						break;
					}
					sig[i] = (uint8_t)sb;
				}
				if (!ok) {
					send_response("Bad hex");
				} else if (owner_slot_revoke(leaf_prefix, sig, &already)) {
					// "already" keeps a repeat call honest: it succeeded, but
					// nothing was written. IAPTool matches on "OK", so the
					// extra words reach the operator without changing the
					// verdict.
					send_response(already ? "OK already revoked" : "OK");
				} else {
					send_response("Refused");
				}
			}
		} else if ((strncmp((char *)RXBuffer, "flashboot ", 10) == 0)
				|| (strncmp((char *)RXBuffer, "flash ", 6) == 0)) {
			// decode flash command:
			//   "flash <size> <crc32hex> <signature_hex> <cert_hex> <noncesig_hex>"
			// - signature_hex: 64-byte ECDSA r||s over the image, by the cert's
			//   leaf key (128 hex chars)
			// - cert_hex: the IAP_CERT_SIZE-byte certificate naming that leaf key
			//   (IAP_CERT_SIZE*2 hex chars) -- see iap_cert.h
			// - noncesig_hex: 64-byte ECDSA r||s, by the same leaf key, over
			//   sha256(nonce || "flash <size> <crc32hex> <signature_hex>") for the
			//   nonce most recently returned by "authchallenge" (128 hex chars)
			//
			// "flashboot" is the same frame with a different destination: it
			// replaces the bootloader in sector 0 and its image signature is
			// checked against the owner root rather than the cert's leaf. Everything else -- staging, CRC,
			// session auth, replay -- is this one path.
			// $PROD/docs/modules/M1/FLASHBOOT.md.
			const bool is_bootloader = (RXBuffer[5] == (uint8_t)'b');
			const char *const verb = is_bootloader ? "flashboot" : "flash";
			const uint32_t size_limit = is_bootloader
					? boot_selfupgrade_max_size() : (uint32_t)IAP_APP_MAX_SIZE;
			char sig_hex[129] = {0};
			char cert_hex[(IAP_CERT_SIZE * 2U) + 1U] = {0};
			char noncesig_hex[129] = {0};
			/* %256s is IAP_CERT_SIZE*2 written out: a scanf field width cannot
			 * be a macro. Update it by hand with IAP_CERT_SIZE, here and in
			 * open_plc_arduino's udp_server.c. */
			int nParsed = sscanf((char *)RXBuffer + strlen(verb),
					" %" SCNu32 " %" SCNx32 " %128s %256s %128s",
					&expected_size, &expected_checksum, sig_hex, cert_hex, noncesig_hex);

			have_expected_signature = false;
			if (nParsed >= 3) {
				if (hex_decode(sig_hex, expected_signature, FW_SIGNATURE_SIZE)) {
					have_expected_signature = true;
				} else {
					printf("Invalid signature hex in flash command\r\n");
				}
			}

			bool authOk = false;
			uint8_t noncesigBytes[FW_SIGNATURE_SIZE];
			if (nParsed == 5 && have_expected_signature
					&& hex_decode(cert_hex, (uint8_t *)&expected_cert, IAP_CERT_SIZE)
					&& hex_decode(noncesig_hex, noncesigBytes, FW_SIGNATURE_SIZE)) {
				char authMsg[220];
				int authMsgLen = snprintf(authMsg, sizeof(authMsg), "%s %" PRIu32 " %" PRIx32 " %s",
						verb, expected_size, expected_checksum, sig_hex);
				authOk = iap_auth_verify_and_consume((const uint8_t *)authMsg, (uint32_t)authMsgLen,
						&expected_cert, noncesigBytes);
			}

			if (nParsed >= 2) {
				if (expected_size == 0 || expected_size > size_limit) {
					printf("Invalid %s size %" PRIu32 ", the target region only has %" PRIu32 " bytes\r\n",
							verb, expected_size, size_limit);
					send_response("ERR");
				} else if (owner_slot_root() == NULL) {
					/* No root, nothing can authorise an image: claim first. */
					printf("%s refused: this board has no root - claim it first\r\n", verb);
					send_response("Refused");
				} else if (!have_expected_signature) {
					// no valid signature was provided -- refuse before erasing
					// anything, so a rejected/malformed upload can never
					// destroy the currently-trusted app for nothing
					printf("flash command missing/invalid signature - refusing to erase\r\n");
					send_response("ERR");
				} else if (!authOk) {
					printf("flash command failed authentication - refusing\r\n");
					send_response("ERR");
					/* Log only: this is the one rejection path an unauthenticated
					 * caller can reach, so it must never touch Flash. */
					bootloader_state_note_auth_fail(tcp_server_get_client_ip());
				} else {
					/* Nothing is erased yet: the image is staged in SDRAM and
					 * the app region is only touched once it has passed CRC
					 * and signature. */
					printf("File size %" PRIu32 ", checksum %" PRIx32 ". Staging in SDRAM.\r\n",
	                        expected_size, expected_checksum);
					received_bytes = 0;
					s_staging_is_bootloader = is_bootloader;
					current_status = FLASH_RECEIVE;
					send_response("OK");
				}
			} else {
				printf("Invalid %s command: only %d of the required size+checksum fields parsed\r\n",
						verb, nParsed);
				send_response("ERR");
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

		/* Whole image staged? */
		if (received_bytes >= expected_size) {
			printf("Transfer complete, verifying the staged image...\r\n");

			/* CRC, hash and signature are all computed over the staging buffer.
			 * Flash is not touched until every one of them has passed. */
			uint32_t caledCRC = HAL_CRC_Calculate(&hcrc, (uint32_t *)IAP_STAGE_BASE, expected_size);

			if (expected_checksum != ~caledCRC) {
				printf("Checksum FAIL. Expected: %08" PRIX32 ", Got: %08" PRIX32 "\r\n",
						expected_checksum, ~caledCRC);
				/* Name the staging buffer when it is the likely cause. The
				 * self-test already says so on the log port, but whoever is
				 * upgrading may only be watching the upload channel, where a
				 * bare "Checksum Failed" sends them after their own image
				 * first. See $PROD/docs/modules/M1-firmware-upgrade.md. */
				if (!iap_sdram_selftest_passed()) {
					send_response("Checksum Failed - SDRAM staging buffer failed its self-test");
				} else {
					send_response("Checksum Failed");
				}
			} else if (!have_expected_signature) {
				printf("No valid signature was provided with this upload - refusing to trust it.\r\n");
				send_response("No Signature");
			} else if (!bootloader_state_crypto_selftest_passed()) {
				// Same precondition server_decide() applies to the boot-time
				// check (Step 1) -- don't trust a signature verification
				// result computed with a crypto implementation the self-test
				// already found broken at boot.
				printf("Crypto self-test failed earlier - refusing to trust signature verification.\r\n");
				send_response("Signature Failed");
			} else {
				uint8_t hash[32];
				/* Hashes the staging buffer, not the app region -- the app region
				 * still holds the previous image at this point. */
				bootloader_state_hash_app(IAP_STAGE_BASE, expected_size, hash);
				/* A bootloader image answers to the owner root itself. A leaf
				 * certificate authorises applications; replacing the bootloader
				 * is the same weight of act as handing over ownership.
				 * $PROD/docs/modules/M1/FLASHBOOT.md. */
				const uint8_t *root = owner_slot_root();
				if ((root == NULL)
						|| (s_staging_is_bootloader
						? !fw_verify_signature_with_key(root, hash, expected_signature)
						: !iap_cert_verify_image(hash, expected_signature, &expected_cert,
							root, owner_slot_is_revoked(expected_cert.leaf_pubkey)))) {
					printf("Signature verification FAILED - firmware not trusted "
							"(bad signature, or the leaf has been revoked). "
							"%s untouched.\r\n",
							s_staging_is_bootloader ? "Bootloader" : "Application region");
					send_response("Signature Failed");
				} else if (s_staging_is_bootloader) {
					/* Does not come back: it resets, because the code that
					 * called it no longer exists once the sector is rewritten.
					 * It returns only for failures it catches before erasing. */
					(void)boot_selfupgrade_commit(expected_size);
					/* Only reached when it refused before erasing. A successful
					 * one resets, which the tool reads the same way it reads the
					 * end of an ordinary flash. */
					send_response("Flash Failed");
				} else if (!stage_commit_to_flash()) {
					printf("The image was good but the flash write was not - "
							"retry the upload.\r\n");
					send_response("Flash Failed");
				} else {
					printf("Checksum and signature OK. Rebooting...\r\n");
					bootloader_state_save_metadata(expected_size, expected_signature, &expected_cert);
					/* No "OK": the reset below takes the MAC down before the
					 * reply leaves, so the tool confirms success by watching
					 * the board come back. See $PROD/docs/modules/M1-firmware-upgrade.md. */
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
 * because the caller polls it while the button stays down after the boot
 * window. Reads the pin as MX_GPIO_Init() left it; HIGH means pressed.
 */
uint8_t boot0_is_pressed(void) {
	return (HAL_GPIO_ReadPin(BOOT0_GPIO_Port, BOOT0_Pin) == GPIO_PIN_SET) ? 1U : 0U;
}
/* Naked: no compiler epilogue may run between setting MSP and the jump, or it
 * pops registers relative to the app's fresh stack and bus-faults. */
#ifndef IAP_SERVER_HOST_TEST
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
#else
/* The PC stand-in has no Cortex-M to jump into; it ends this boot and starts
 * the application half instead. $PROD/docs/engineering/BOOTLOADER-STAND-IN.md */
void iap_server_host_jump_to_app(uint32_t msp, uint32_t reset_vector);
#define jump_to_app iap_server_host_jump_to_app
#endif

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
 * Deliberately does not jump: the caller owns that step (server_jump_to_app).
 */
IAP_Method server_decide(uint8_t boot0Pressed) {
	IAP_Method mode = IAP_NONE;
	boot_req_t req;
	bool app_signature_valid = false;

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

	/* boot0Pressed is read by the caller at the end of the boot window, not
	 * sampled here: reading the pin once at this point would need the operator
	 * to already be holding the button a few milliseconds after
	 * reset, which is not a reaction time a human has. A press decides the
	 * outcome on its own -- see the branch below. */
	const char *reason = "";

	printf(boot0Pressed ? "BOOT0 button is pressed!\r\n"
	                    : "BOOT0 button is not pressed.\r\n");

	/* Is the app region's actual content the one that was signed and accepted
	 * at the last successful update? */
	if (bootloader_state_crypto_selftest_passed() && (owner_slot_root() != NULL)) {
		iap_fw_metadata_t meta;
		if (bootloader_state_get_metadata(&meta) && meta.app_size > 0U && meta.app_size <= IAP_APP_MAX_SIZE) {
			uint8_t hash[32];
			bootloader_state_hash_app(app_base, meta.app_size, hash);
			/* Re-checks meta.cert's root_sig too, not just the leaf signature --
			 * that is what makes a setowner handover retroactively invalidate
			 * firmware certified by the old owner: the stored cert's root_sig
			 * only verifies against the root that signed it, and owner_slot_root()
			 * has since moved on.
			 *
			 * Revocation deliberately does NOT take part here: a revoked leaf's
			 * already-installed image keeps booting, and revocation only blocks
			 * the next upload. Revoking asks whether a person is still trusted,
			 * and a colleague leaving must not stop the machines they once
			 * touched. $PROD/docs/tables/DECISIONS.md, decision 60.
			 *
			 * The operator is not left blind: "getapprevoked" reports the
			 * boolean this call no longer consumes (decision 63). */
			app_signature_valid = iap_cert_verify_image(hash, meta.signature, &meta.cert,
					owner_slot_root(), false);
		}
	}
	bootloader_state_set_app_valid(app_signature_valid);

	if (boot0Pressed > 0U) {
		/* The physical escape hatch wins outright. Whoever held the button
		 * at the end of the boot window wants the bootloader, so nothing else
		 * gets a vote -- not the handoff request, not the signature. We cannot know
		 * which channel they will use, so serve all of them.
		 *
		 * The two reads above still had to happen: boot_handoff_take() is what
		 * consumes the request record (leaving it would pin the *next* boot
		 * here), and the signature result feeds the UDP server name and
		 * getapprevoked, neither of which is part of this decision. */
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
				mode = IAP_ALL;
				reason = "no valid application";
			}
			break;
		}
	}

	/* current_method is (re)set by IAP_data_recv() when a command arrives.
	 * Pre-seed it for the single-channel modes; IAP_ALL waits for the first
	 * command to say which channel. */
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
 * USB, ethernet and FMC are never brought up on this path, so they need no
 * de-init.
 */
void server_jump_to_app(void) {
	/* Give the application a clean slate. Our MPU regions describe the
	 * bootloader's lwIP buffers and mean nothing to it, and leaving the 4GB
	 * no-access blanket in place would deny it the external SDRAM. One register
	 * write, so unlike a peripheral de-init there is nothing to forget. */
	HAL_MPU_Disable();

	/* No UART or transceiver to hand back: the bootloader never opens them
	 * (R1-43). A last line without its newline would otherwise be lost. */
	boot_log_flush_stdout();

	__disable_irq();

	/* stop SysTick */
	SysTick->CTRL = 0;
	SysTick->LOAD = 0;
	SysTick->VAL = 0;

	HAL_RCC_DeInit();
	HAL_DeInit();

	/* Leave nothing on that a cold board has off (decision 4): HAL_RCC_DeInit()
	 * re-arms the TIM6 time base, and the clock enables below survive both
	 * DeInits. See $PROD/docs/modules/M3-app-runtime.md R3-02. */
	__HAL_RCC_TIM6_CLK_DISABLE();
	__HAL_RCC_SYSCFG_CLK_DISABLE();
	RCC->AHB4ENR = 0U;   /* GPIOA-K, BKPRAM: reset value; safe_outputs_init() re-enables its ports */
	HAL_PWR_DisableBkUpAccess();

	/* clear interrupt pending -- NVIC->ICER/ICPR are 8 words (256 IRQs,
	 * the ARMv7-M architectural max), not 16; looping past 8 wrote into
	 * the reserved padding between register banks. After the DeInits, which
	 * enable the TIM6 IRQ again. */
	for (uint32_t i = 0; i < 8; i++) {
		NVIC->ICER[i] = 0xFFFFFFFFU;
		NVIC->ICPR[i] = 0xFFFFFFFFU;
	}

	/* HAL_DeInit() just reset every GPIO port, so the outputs float again: drive
	 * them back to 0 and leave them for the sketch's first write. The one
	 * exception to the cold-board handover (decision 4), decision 81. */
	safe_outputs_init();

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
#define IAP_LINK_REPORT_MS 5000U
static bool s_link_report_pending;
static uint32_t s_link_report_start;

/* Say so when the link is still not usable 5 s after the channels opened.
 * ethernetif.c returns silently when LAN8742_Init() fails, in a CubeMX region,
 * so the report has to happen here. Not earlier: right after lwIP init the PHY
 * is often still auto-negotiating. See $PROD/docs/modules/M1/BOOT-SEQUENCE.md. */
static void IAP_link_report(void) {
	if (!s_link_report_pending
			|| (HAL_GetTick() - s_link_report_start) < IAP_LINK_REPORT_MS) {
		return;
	}
	s_link_report_pending = false;
	if ((netif_default == NULL) || !netif_is_link_up(netif_default)) {
		printf("** Ethernet link is DOWN - this board will not answer discovery. **\r\n"
		       "** Check the cable; if it is in, the PHY did not initialise. **\r\n");
	}
}

void IAP_servers_start(IAP_Method mode) {
	if ((mode == IAP_ETHERNET) || (mode == IAP_ALL)) {
		s_link_report_pending = true;
		s_link_report_start = HAL_GetTick();
		tcp_server_start();
		openplc_udp_server_start();
	}
}

#define IAP_IDLE_RX_STALE_MS 500U
static volatile uint32_t s_idle_rx_last_tick;

void IAP_task(void) {
	IAP_link_report();
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

		// A text command ends at '\n' or '\r' (IAPTool sends '\n'; a raw
		// terminal may send only '\r'). The "flash" command (~610 bytes)
		// spans several USB packets / TCP segments, so fragments accumulate
		// until the terminator or a full buffer; IAP_task() drops a partial
		// command that never gets one.
		if (memchr(RXBuffer, '\n', len_in_RX_buffer) != NULL
				|| memchr(RXBuffer, '\r', len_in_RX_buffer) != NULL
				|| len_in_RX_buffer >= IAP_RX_BUFFER_SIZE) {
			data_is_ready = 1;
			printf("IDLE ready \r\n");
		}
	} else {
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

		// Hand over when the buffer is full or the image is complete.
		if (len_in_RX_buffer >= IAP_RX_BUFFER_SIZE
				|| received_bytes + len_in_RX_buffer >= expected_size) {
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

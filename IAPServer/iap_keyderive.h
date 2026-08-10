/*
 * iap_keyderive.h
 *
 * Derives each device's own IAP auth key from one fixed shared password
 * mixed with its machine ID (STM32 96-bit UID), so every device
 * authenticates with a different effective key even though the same
 * password is embedded in every firmware image. The construction is
 * isolated to this file: switching to a stronger scheme later (a real
 * provisioned per-device secret, a different KDF...) means changing
 * iap_keyderive_get_device_key() only -- iap_auth.c and every other caller
 * stay untouched.
 *
 * Must match, byte-for-byte:
 *   - libraries/OpenPLC_IAP/src/iap_keyderive.h/.c (Arduino-core mirror)
 *   - IAPTranfer_Tool/iapcrypto (PC tool, Go)
 * See IAPServer/keys/README.md for key-management notes.
 */

#ifndef IAPSERVER_IAP_KEYDERIVE_H_
#define IAPSERVER_IAP_KEYDERIVE_H_

#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

#define IAP_DEVICE_KEY_SIZE    32U
#define IAP_MACHINE_ID_SIZE    12U /* STM32 96-bit UID: UIDW2||UIDW1||UIDW0 */
#define IAP_MACHINE_ID_HEX_LEN (IAP_MACHINE_ID_SIZE * 2U)

/* This device's machine ID as raw bytes, big-endian UIDW2||UIDW1||UIDW0. */
void iap_keyderive_get_machine_id(uint8_t out_id[IAP_MACHINE_ID_SIZE]);

/* This device's machine ID as an uppercase hex string (as sent in "getuid"
 * responses and UDP discovery/ping replies), null-terminated. */
void iap_keyderive_get_machine_id_hex(char out_hex[IAP_MACHINE_ID_HEX_LEN + 1U]);

/* Derives this device's own key: HMAC-SHA256(fixed_password, machine_id). */
void iap_keyderive_get_device_key(uint8_t out_key[IAP_DEVICE_KEY_SIZE]);

#ifdef __cplusplus
}
#endif

#endif /* IAPSERVER_IAP_KEYDERIVE_H_ */

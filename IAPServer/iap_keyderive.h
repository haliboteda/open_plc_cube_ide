/*
 * iap_keyderive.h
 *
 * This device's machine ID (STM32 96-bit UID) -- used for the "getuid"
 * command, UDP discovery/ping identity strings, and (see owner_slot.h) the
 * uid field of an owner record.
 *
 * Must match, byte-for-byte:
 *   - libraries/OpenPLC_IAP/src/iap_keyderive.h/.c (Arduino-core mirror)
 */

#ifndef IAPSERVER_IAP_KEYDERIVE_H_
#define IAPSERVER_IAP_KEYDERIVE_H_

#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

#define IAP_MACHINE_ID_SIZE    12U /* STM32 96-bit UID: UIDW2||UIDW1||UIDW0 */
#define IAP_MACHINE_ID_HEX_LEN (IAP_MACHINE_ID_SIZE * 2U)

/* This device's machine ID as raw bytes, big-endian UIDW2||UIDW1||UIDW0. */
void iap_keyderive_get_machine_id(uint8_t out_id[IAP_MACHINE_ID_SIZE]);

/* This device's machine ID as an uppercase hex string (as sent in "getuid"
 * responses and UDP discovery/ping replies), null-terminated. */
void iap_keyderive_get_machine_id_hex(char out_hex[IAP_MACHINE_ID_HEX_LEN + 1U]);

#ifdef __cplusplus
}
#endif

#endif /* IAPSERVER_IAP_KEYDERIVE_H_ */

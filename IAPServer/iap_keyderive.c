/*
 * iap_keyderive.c
 *
 * See iap_keyderive.h.
 */

#include "iap_keyderive.h"
#include "sha256.h"
#include "main.h"
#include <stdio.h>

/* Rotate with keys/rotate_keys.sh. */
static const uint8_t iap_fixed_password[] =
#include "keys/iap_fixed_password.txt"
;

void iap_keyderive_get_machine_id(uint8_t out_id[IAP_MACHINE_ID_SIZE])
{
	uint32_t words[3] = { HAL_GetUIDw2(), HAL_GetUIDw1(), HAL_GetUIDw0() };
	uint32_t i;

	for (i = 0; i < 3U; i++) {
		out_id[i * 4U + 0U] = (uint8_t)(words[i] >> 24);
		out_id[i * 4U + 1U] = (uint8_t)(words[i] >> 16);
		out_id[i * 4U + 2U] = (uint8_t)(words[i] >> 8);
		out_id[i * 4U + 3U] = (uint8_t)(words[i]);
	}
}

void iap_keyderive_get_machine_id_hex(char out_hex[IAP_MACHINE_ID_HEX_LEN + 1U])
{
	(void)snprintf(out_hex, IAP_MACHINE_ID_HEX_LEN + 1U, "%08lX%08lX%08lX",
			(unsigned long)HAL_GetUIDw2(), (unsigned long)HAL_GetUIDw1(), (unsigned long)HAL_GetUIDw0());
}

void iap_keyderive_get_device_key(uint8_t out_key[IAP_DEVICE_KEY_SIZE])
{
	uint8_t machine_id[IAP_MACHINE_ID_SIZE];

	iap_keyderive_get_machine_id(machine_id);
	hmac_sha256(iap_fixed_password, sizeof(iap_fixed_password) - 1U,
			machine_id, IAP_MACHINE_ID_SIZE, out_key);
}

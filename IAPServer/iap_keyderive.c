/*
 * iap_keyderive.c
 *
 * See iap_keyderive.h.
 */

#include "iap_keyderive.h"
#include "main.h"
#include <stdio.h>

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


/*
 * bor_check.c -- see bor_check.h.
 */
#include "bor_check.h"
#include "main.h"
#include <stdio.h>

/* Nominal thresholds, from the OB_BOR_LEVELx comments in
 * stm32h7xx_hal_flash_ex.h. */
static const char *const k_level_volts[4] = { "1.6", "2.1", "2.4", "2.7" };

void bor_level_report(void)
{
	const uint32_t level = (FLASH->OPTSR_CUR & FLASH_OPTSR_BOR_LEV) >> FLASH_OPTSR_BOR_LEV_Pos;

	if ((level << FLASH_OPTSR_BOR_LEV_Pos) == OB_BOR_LEVEL3) {
		return;
	}
	printf("** Brown-out reset is at level %lu (%s V), not level 3 (2.7 V): below 2.7 V the "
	       "MCU keeps running and outputs can move. Set BOR_LEV=3 with ST-Link. **\r\n",
	       (unsigned long)level, k_level_volts[level & 3U]);
}

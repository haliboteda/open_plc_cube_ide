/*
 * usbd_cdc_flash.h
 *
 *  Created on: Sep 12, 2024
 *      Author: uu
 */

#ifndef INC_USBD_CDC_FLASH_H_
#define INC_USBD_CDC_FLASH_H_


/* Includes ------------------------------------------------------------------*/
#include "usbd_cdc.h"

/* Exported types ------------------------------------------------------------*/

/* Exported constants --------------------------------------------------------*/

/* Base address of the Flash sectors Bank 1 */
#define ADDR_FLASH_SECTOR_0_BANK1     ((uint32_t)0x08000000) /* Base @ of Sector 0, 128 Kbytes */
#define ADDR_FLASH_SECTOR_1_BANK1     ((uint32_t)0x08020000) /* Base @ of Sector 1, 128 Kbytes */
#define ADDR_FLASH_SECTOR_2_BANK1     ((uint32_t)0x08040000) /* Base @ of Sector 2, 128 Kbytes */
#define ADDR_FLASH_SECTOR_3_BANK1     ((uint32_t)0x08060000) /* Base @ of Sector 3, 128 Kbytes */
#define ADDR_FLASH_SECTOR_4_BANK1     ((uint32_t)0x08080000) /* Base @ of Sector 4, 128 Kbytes */
#define ADDR_FLASH_SECTOR_5_BANK1     ((uint32_t)0x080A0000) /* Base @ of Sector 5, 128 Kbytes */
#define ADDR_FLASH_SECTOR_6_BANK1     ((uint32_t)0x080C0000) /* Base @ of Sector 6, 128 Kbytes */
#define ADDR_FLASH_SECTOR_7_BANK1     ((uint32_t)0x080E0000) /* Base @ of Sector 7, 128 Kbytes */

/* Base address of the Flash sectors Bank 2 */
#define ADDR_FLASH_SECTOR_0_BANK2     ((uint32_t)0x08100000) /* Base @ of Sector 0, 128 Kbytes */
#define ADDR_FLASH_SECTOR_1_BANK2     ((uint32_t)0x08120000) /* Base @ of Sector 1, 128 Kbytes */
#define ADDR_FLASH_SECTOR_2_BANK2     ((uint32_t)0x08140000) /* Base @ of Sector 2, 128 Kbytes */
#define ADDR_FLASH_SECTOR_3_BANK2     ((uint32_t)0x08160000) /* Base @ of Sector 3, 128 Kbytes */
#define ADDR_FLASH_SECTOR_4_BANK2     ((uint32_t)0x08180000) /* Base @ of Sector 4, 128 Kbytes */
#define ADDR_FLASH_SECTOR_5_BANK2     ((uint32_t)0x081A0000) /* Base @ of Sector 5, 128 Kbytes */
#define ADDR_FLASH_SECTOR_6_BANK2     ((uint32_t)0x081C0000) /* Base @ of Sector 6, 128 Kbytes */
#define ADDR_FLASH_SECTOR_7_BANK2     ((uint32_t)0x081E0000) /* Base @ of Sector 7, 128 Kbytes */

/* Exported macro ------------------------------------------------------------*/

#define FLASH_BASE_ADDR      (uint32_t)(FLASH_BASE)
#define FLASH_END_ADDR       (uint32_t)(0x081FFFFF)

/* Exported functions ------------------------------------------------------- */
/* Sector 0 of bank 1 is the bootloader; the app starts at sector 1. */
#define ADDRESS_VECTOR ((uint32_t)0x20000)
#define RESERVED_SECTORS 1

#define IAP_APP_ADDRESS (FLASH_BASE_ADDR | ADDRESS_VECTOR)

/* Programs `Len` bytes at `FlashAddress`. Len and the address must both be
 * multiples of 32: the H7 programs one 256-bit flash word at a time, and a
 * partial word cannot be written without erasing its neighbours. Returns 0 on
 * success. The target must already be erased. */
uint16_t Flash_If_Write(uint8_t *DataAddress, uint8_t *FlashAddress, uint32_t Len);

/* The last sector (bank 2 sector 7, "sector 15") holds bootloader-owned state:
 * calibration, root area and firmware metadata. App erase/write never touches
 * it and it is excluded from the app's maximum size.
 * See $PROD/docs/modules/M1/SECTOR-15.md. */
#define RESERVED_TAIL_SECTORS 1
#define IAP_STATE_SECTOR_ADDR ADDR_FLASH_SECTOR_7_BANK2
#define IAP_APP_MAX_SIZE (IAP_STATE_SECTOR_ADDR - IAP_APP_ADDRESS)

#endif /* INC_USBD_CDC_FLASH_H_ */

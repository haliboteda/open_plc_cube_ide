/*
 * usbd_cdc_flash.c
 *
 *  Created on: Sep 12, 2024
 *      Author: uu
 */
/* Includes ------------------------------------------------------------------ */
#include "usbd_cdc_flash.h"
#include "stm32h7xx_hal_conf.h"

/* Private typedef ----------------------------------------------------------- */
/* Private define ------------------------------------------------------------ */
/* Private macro ------------------------------------------------------------- */
/* Private variables --------------------------------------------------------- */
/* Private function prototypes ----------------------------------------------- */
static uint32_t GetSector(uint32_t Address);

/* Extern function prototypes ------------------------------------------------ */
uint16_t Erase_FLASH(uint8_t *flashAddress, uint32_t Len);
uint16_t Flash_If_Write(uint8_t *DataAddress, uint8_t *FlashAddress, uint32_t Len);

/**
 * @brief  Erases sector.
 * @param  Add: Address of sector to be erased.
 * @param NbSectors: number of sectors
 * @retval 0 if operation is successful, MAL_FAIL else.
 */
uint16_t Flash_If_Erase(uint32_t Add, uint32_t NbSectors)
{
  uint32_t startsector = 0, sectorerror = 0;

  /* Variable contains Flash operation status */
  HAL_StatusTypeDef status;
  FLASH_EraseInitTypeDef eraseinitstruct;

  /* Get the number of sector */
  startsector = GetSector(Add);
  eraseinitstruct.TypeErase = FLASH_TYPEERASE_SECTORS;
  eraseinitstruct.VoltageRange = FLASH_VOLTAGE_RANGE_3;
  eraseinitstruct.Banks = FLASH_BANK_1;
  eraseinitstruct.Sector = startsector;
  eraseinitstruct.NbSectors = NbSectors;

  SCB_DisableICache();
  HAL_FLASH_Unlock();
  status = HAL_FLASHEx_Erase(&eraseinitstruct, &sectorerror);
  HAL_FLASH_Lock();
  SCB_EnableICache();

  if (status != HAL_OK)
  {
    return HAL_ERROR;
  }
  return HAL_OK;
}

/**
 * @brief  Writes Data into Memory.
 * @param  FlashAddress specifies the address to be programmed.
 *         This parameter shall be aligned to the Flash word:
 *          - 256 bits for STM32H74x/5X devices (8x 32bits words)
 *          - 128 bits for STM32H7Ax/BX devices (4x 32bits words)
 *          - 256 bits for STM32H72x/3X devices (8x 32bits words)
 * @param  DataAddress specifies the address of data to be programmed.
 *         This parameter shall be 32-bit aligned
 * @param  Len: Number of data to be written (in bytes).
 * @retval 0 if operation is successful, MAL_FAIL else.
 */

uint16_t Flash_If_Write(uint8_t *DataAddress, uint8_t *FlashAddress, uint32_t Len)
{
  uint32_t i = 0;
  SCB_DisableICache();
  HAL_FLASH_Unlock();

  for (i = 0; i < Len; i += 32)
  {
    /* Device voltage range supposed to be [2.7V to 3.6V], the operation will
     * be done by byte */
    if (HAL_FLASH_Program(FLASH_TYPEPROGRAM_FLASHWORD, (uint32_t)(FlashAddress + i),
                          (uint32_t)(DataAddress + i)) == HAL_OK)
    {
      /* Check the written value */
      if (*(uint64_t *)(DataAddress + i) != *(uint64_t *)(FlashAddress + i))
      {
        /* Flash content doesn't match SRAM content */
        return 2;
      }
    }
    else
    {
      /* Error occurred while writing data in Flash memory */
      return HAL_ERROR;
    }
  }
  HAL_FLASH_Lock();
  SCB_EnableICache();
  return HAL_OK;
}

/**
 * @brief  Gets the sector of a given address
 * @param  Address Address of the FLASH Memory
 * @retval The sector of a given address
 */
static uint32_t GetSector(uint32_t Address)
{
  uint32_t sector = 0;

  if (((Address < ADDR_FLASH_SECTOR_1_BANK1) &&
       (Address >= ADDR_FLASH_SECTOR_0_BANK1)) ||
      ((Address < ADDR_FLASH_SECTOR_1_BANK2) &&
       (Address >= ADDR_FLASH_SECTOR_0_BANK2)))
  {
    sector = FLASH_SECTOR_0;
  }
  else if (((Address < ADDR_FLASH_SECTOR_2_BANK1) &&
            (Address >= ADDR_FLASH_SECTOR_1_BANK1)) ||
           ((Address < ADDR_FLASH_SECTOR_2_BANK2) &&
            (Address >= ADDR_FLASH_SECTOR_1_BANK2)))
  {
    sector = FLASH_SECTOR_1;
  }
  else if (((Address < ADDR_FLASH_SECTOR_3_BANK1) &&
            (Address >= ADDR_FLASH_SECTOR_2_BANK1)) ||
           ((Address < ADDR_FLASH_SECTOR_3_BANK2) &&
            (Address >= ADDR_FLASH_SECTOR_2_BANK2)))
  {
    sector = FLASH_SECTOR_2;
  }
  else if (((Address < ADDR_FLASH_SECTOR_4_BANK1) &&
            (Address >= ADDR_FLASH_SECTOR_3_BANK1)) ||
           ((Address < ADDR_FLASH_SECTOR_4_BANK2) &&
            (Address >= ADDR_FLASH_SECTOR_3_BANK2)))
  {
    sector = FLASH_SECTOR_3;
  }
  else if (((Address < ADDR_FLASH_SECTOR_5_BANK1) &&
            (Address >= ADDR_FLASH_SECTOR_4_BANK1)) ||
           ((Address < ADDR_FLASH_SECTOR_5_BANK2) &&
            (Address >= ADDR_FLASH_SECTOR_4_BANK2)))
  {
    sector = FLASH_SECTOR_4;
  }
  else if (((Address < ADDR_FLASH_SECTOR_6_BANK1) &&
            (Address >= ADDR_FLASH_SECTOR_5_BANK1)) ||
           ((Address < ADDR_FLASH_SECTOR_6_BANK2) &&
            (Address >= ADDR_FLASH_SECTOR_5_BANK2)))
  {
    sector = FLASH_SECTOR_5;
  }
  else if (((Address < ADDR_FLASH_SECTOR_7_BANK1) &&
            (Address >= ADDR_FLASH_SECTOR_6_BANK1)) ||
           ((Address < ADDR_FLASH_SECTOR_7_BANK2) &&
            (Address >= ADDR_FLASH_SECTOR_6_BANK2)))
  {
    sector = FLASH_SECTOR_6;
  }
  else if (((Address < ADDR_FLASH_SECTOR_0_BANK2) &&
            (Address >= ADDR_FLASH_SECTOR_7_BANK1)) ||
           ((Address < FLASH_END_ADDR) && (Address >=
                                           ADDR_FLASH_SECTOR_7_BANK2)))
  {
    sector = FLASH_SECTOR_7;
  }
  else
  {
    sector = FLASH_SECTOR_7;
  }

  return sector;
}

/**
 * @brief  Erase Flash.
 * @param  flashAddress: flash address to be written to.
 * @param  Len: Number of data to be written (in bytes).
 * @retval 0 if operation is successful, MAL_FAIL else.
 */
uint16_t Erase_FLASH(uint8_t *flashAddress, uint32_t Len)
{
  // get number of sectors to be written to.
  uint32_t beginS = GetSector((uint32_t)flashAddress);
  uint32_t endS = GetSector((uint32_t)flashAddress + Len);
  // add 1 more
  uint32_t NbSectors = (endS - beginS + 1) > FLASH_SECTOR_TOTAL - RESERVED_SECTORS ? FLASH_SECTOR_TOTAL - RESERVED_SECTORS : (endS - beginS + 1);
  // erase
  HAL_StatusTypeDef status = Flash_If_Erase((uint32_t)flashAddress, NbSectors);
  if (status != HAL_OK)
  {
    printf("There is error when erasing flash. Addr:%x Sectors:%d Status:%d\r\n", flashAddress, NbSectors, status);
    return HAL_ERROR;
  }
  printf("Erasing flash Done. Addr:%x Sectors:%d\r\n", flashAddress, NbSectors);
  return HAL_OK;
}

/**
 * @brief  Writes Data into Flash.
 * @param  dataAddress: Data Address .
 * @param  flashAddress: flash address to be written to.
 * @param  Len: Number of data to be written (in bytes).
 * @retval 0 if operation is successful, MAL_FAIL else.
 */

uint16_t Write_FLASH(uint8_t *dataAddress, uint8_t *flashAddress, uint32_t Len)
{
  HAL_StatusTypeDef status = Flash_If_Write(dataAddress, flashAddress, Len);
  if (status != HAL_OK)
  {
    printf("There is error when writing flash. Addr:%x Data:%x Status:%d", flashAddress, dataAddress, status);
    return HAL_ERROR;
  }
  return HAL_OK;
}

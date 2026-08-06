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
static uint32_t GetBank(uint32_t Address);

/* Extern function prototypes ------------------------------------------------ */
uint16_t Erase_FLASH(uint8_t *flashAddress, uint32_t Len);
uint16_t Flash_If_Write(uint8_t *DataAddress, uint8_t *FlashAddress, uint32_t Len);

/* Size of one Flash sector in bytes (128K on this device, both banks). */
#define FLASH_SECTOR_SIZE_BYTES (ADDR_FLASH_SECTOR_1_BANK1 - ADDR_FLASH_SECTOR_0_BANK1)

/**
 * @brief  Erases sector(s), transparently splitting the erase across the
 *         Bank1/Bank2 boundary if [Add, Add + NbSectors*sectorSize) spans it.
 *         (GetSector() returns a 0-7 sector index that is reused across both
 *         banks; HAL_FLASHEx_Erase needs the matching Banks field for each
 *         chunk, so a single call can never be used for a range that crosses
 *         banks.)
 * @param  Add: Address of sector to be erased.
 * @param NbSectors: number of sectors
 * @retval 0 if operation is successful, MAL_FAIL else.
 */
uint16_t Flash_If_Erase(uint32_t Add, uint32_t NbSectors)
{
  uint32_t sectorerror = 0;
  uint32_t addr = Add;
  uint32_t remaining = NbSectors;
  HAL_StatusTypeDef status = HAL_OK;
  FLASH_EraseInitTypeDef eraseinitstruct;

  SCB_DisableICache();
  HAL_FLASH_Unlock();

  while ((remaining > 0U) && (status == HAL_OK))
  {
    uint32_t bank = GetBank(addr);
    uint32_t sector = GetSector(addr);
    uint32_t sectorsLeftInBank = FLASH_SECTOR_TOTAL - sector;
    uint32_t chunk = (remaining < sectorsLeftInBank) ? remaining : sectorsLeftInBank;

    eraseinitstruct.TypeErase = FLASH_TYPEERASE_SECTORS;
    eraseinitstruct.VoltageRange = FLASH_VOLTAGE_RANGE_3;
    eraseinitstruct.Banks = bank;
    eraseinitstruct.Sector = sector;
    eraseinitstruct.NbSectors = chunk;

    status = HAL_FLASHEx_Erase(&eraseinitstruct, &sectorerror);

    addr += chunk * FLASH_SECTOR_SIZE_BYTES;
    remaining -= chunk;
  }

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
 * @brief  Gets the bank (FLASH_BANK_1/FLASH_BANK_2) a given address falls in.
 * @param  Address Address of the FLASH Memory
 * @retval FLASH_BANK_1 or FLASH_BANK_2
 */
static uint32_t GetBank(uint32_t Address)
{
  return (Address < ADDR_FLASH_SECTOR_0_BANK2) ? FLASH_BANK_1 : FLASH_BANK_2;
}

/**
 * @brief  Gets the 0-15 global sector index (bank-aware) of a given address,
 *         unlike GetSector() which returns a 0-7 index recycled per bank.
 * @param  Address Address of the FLASH Memory
 * @retval Global sector index, 0-15
 */
static uint32_t GetGlobalSectorIndex(uint32_t Address)
{
  uint32_t bankIndex = (GetBank(Address) == FLASH_BANK_1) ? 0U : 1U;
  return (bankIndex * FLASH_SECTOR_TOTAL) + GetSector(Address);
}

/**
 * @brief  Erase Flash.
 * @param  flashAddress: flash address to be written to.
 * @param  Len: Number of data to be written (in bytes).
 * @retval 0 if operation is successful, MAL_FAIL else.
 */
uint16_t Erase_FLASH(uint8_t *flashAddress, uint32_t Len)
{
  // never erase into the reserved tail sector (bootloader state: firmware
  // metadata + tamper-evident event log)
  if ((uint32_t)flashAddress + Len > IAP_STATE_SECTOR_ADDR)
  {
    return HAL_ERROR;
  }

  // get number of sectors to be erased, using a global (bank-spanning) sector
  // index -- GetSector() alone recycles 0-7 per bank and cannot be subtracted
  // across a Bank1/Bank2 boundary.
  uint32_t beginG = GetGlobalSectorIndex((uint32_t)flashAddress);
  uint32_t endG = GetGlobalSectorIndex((uint32_t)flashAddress + Len - 1U);
  uint32_t NbSectors = endG - beginG + 1U;
  uint32_t maxSectors = (FLASH_SECTOR_TOTAL * 2U) - RESERVED_SECTORS - RESERVED_TAIL_SECTORS;
  if (NbSectors > maxSectors)
  {
    NbSectors = maxSectors;
  }
  // erase
  HAL_StatusTypeDef status = Flash_If_Erase((uint32_t)flashAddress, NbSectors);
  if (status != HAL_OK)
  {
    //printf("There is error when erasing flash. Addr:%x Sectors:%d Status:%d\r\n", flashAddress, NbSectors, status);
    return HAL_ERROR;
  }
  //printf("Erasing flash Done. Addr:%x Sectors:%d\r\n", flashAddress, NbSectors);
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
//    printf("There is error when writing flash. Addr:%x Data:%x Status:%d", flashAddress, dataAddress, status);
    return HAL_ERROR;
  }
  return HAL_OK;
}

/*
 * ota_config.h
 *
 *  Created on: Apr 30, 2025
 *      Author: WhoamIamwhO
 */

#ifndef INC_IAP_CONFIG_H_
#define INC_IAP_CONFIG_H_

#define MAGIC_BKP_REG RTC_BKP_DR0
#define MAGIC_APP_FLAG 0xAA

#define MAGIC_CDC_RATE 1200
#define MAGIC_CDC_FLAG 0xAF
#define MAGIC_ETH_FLAG 0xAE

#define BOOT_LOADER_VERSION "Boot Loader 0.1.2\r\n"

#define IAP_RX_BUFFER_SIZE 8 * 1024

typedef enum {
	IDLE, FLASH_RECEIVE
} IAP_State;

typedef enum {
    IAP_NONE,
	IAP_CDC,
	IAP_ETHERNET
} IAP_Method;

#endif /* INC_IAP_CONFIG_H_ */

/*
 * ota_config.h
 *
 *  Created on: Apr 30, 2025
 *      Author: WhoamIamwhO
 */

#ifndef INC_IAP_CONFIG_H_
#define INC_IAP_CONFIG_H_

#define BOOT_LOADER_VERSION "Boot Loader 0.1.2\r\n"

#define IAP_RX_BUFFER_SIZE 8 * 1024

#ifndef OPENPLC_UDP_PORT
#define OPENPLC_UDP_PORT 16861
#endif

#ifndef OPENPLC_DEVICE_NAME
#define OPENPLC_DEVICE_NAME "STM32H743"
#endif

#ifndef OPENPLC_CUSAPP_VERSION
#define OPENPLC_CUSAPP_VERSION "0.1.0"
#endif

#ifndef UDP_SERVER_NAME
#define UDP_SERVER_NAME "BOOT"
#endif

#ifndef MAGIC_CDC_RATE
#define MAGIC_CDC_RATE 1200U
#endif

#ifndef MAGIC_APP_FLAG
#define MAGIC_APP_FLAG 0xAAU
#endif

#ifndef MAGIC_ETH_FLAG
#define MAGIC_ETH_FLAG 0xAEU
#endif

#ifndef MAGIC_CDC_FLAG
#define MAGIC_CDC_FLAG 0xAFU
#endif

#ifndef MAGIC_BKP_REG
#define MAGIC_BKP_REG RTC_BKP_DR0
#endif

typedef enum {
	IDLE, FLASH_RECEIVE
} IAP_STATUS;

typedef enum {
    IAP_NONE,
	IAP_CDC,
	IAP_ETHERNET
} IAP_Method;

#endif /* INC_IAP_CONFIG_H_ */
